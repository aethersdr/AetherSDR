#include "gui/CopyAssistSettings.h"

#include "core/AppSettings.h"
#include "asr/AsrCrashMarker.h"

#include <QDebug>
#include <QEventLoop>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QTimer>

#ifdef HAVE_KEYCHAIN
#include <qt6keychain/keychain.h>
#endif

namespace AetherSDR {
namespace CopyAssistSettings {

namespace {

// Serializes the whole-object read-modify-write below. AppSettings is
// thread-safe per call, but a field write here is read + insert + write, and the
// ASR worker thread now rewrites the load marker (updateValue, #5190) while the
// GUI thread may be writing any other field. Never held across the api-key
// paths, which spin an event loop.
QMutex& objectMutex()
{
    static QMutex m;
    return m;
}

QJsonObject readObject()
{
    return QJsonDocument::fromJson(
               AppSettings::instance().value(rootKey()).toString().toUtf8())
        .object();
}

// ── Remote-ASR API key: keychain-only (RFC #4603 proposal E) ─────────────────
// Credentials never live in the settings store. The api key's home is the OS
// keychain (service "AetherSDR", key "asr_remote_api_key"); without keychain
// support it is session-only in the AppSettings vault. The nested-doc field it
// used to occupy is stripped by the XML import and, defensively, here.
const QString kApiKeyField = QStringLiteral("AsrRemoteApiKey");
constexpr const char* kApiKeyKeychainKey = "asr_remote_api_key";

QString& apiKeyCache()
{
    static QString cache;
    return cache;
}

bool& apiKeyLoaded()
{
    static bool loaded = false;
    return loaded;
}

// Takes objectMutex itself: never call it with the lock held.
void stripApiKeyFromDoc()
{
    const QMutexLocker lock(&objectMutex());
    auto& s = AppSettings::instance();
    QJsonObject obj = readObject();
    if (!obj.contains(kApiKeyField)) {
        return;
    }
    obj.remove(kApiKeyField);
    s.setValue(rootKey(), QString::fromUtf8(
                   QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    s.save();
}

#ifdef HAVE_KEYCHAIN
QString keychainReadApiKeyBlocking()
{
    auto* job = new QKeychain::ReadPasswordJob(QStringLiteral("AetherSDR"));
    job->setAutoDelete(false);
    job->setKey(QString::fromLatin1(kApiKeyKeychainKey));
    QEventLoop loop;
    QObject::connect(job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    job->start();
    loop.exec();
    QString result;
    if (job->error() == QKeychain::NoError) {
        result = job->textData();
    } else if (job->error() != QKeychain::EntryNotFound) {
        qWarning() << "CopyAssist: keychain read for the remote ASR api key"
                      " failed:" << job->errorString();
    }
    delete job;
    return result;
}

void keychainWriteApiKey(const QString& key)
{
    if (key.isEmpty()) {
        auto* job = new QKeychain::DeletePasswordJob(QStringLiteral("AetherSDR"));
        job->setAutoDelete(true);
        job->setKey(QString::fromLatin1(kApiKeyKeychainKey));
        QObject::connect(job, &QKeychain::Job::finished, job, [](QKeychain::Job* j) {
            if (j->error() != QKeychain::NoError
                && j->error() != QKeychain::EntryNotFound) {
                qWarning() << "CopyAssist: keychain delete for the remote ASR"
                              " api key failed:" << j->errorString();
            }
        });
        job->start();
        return;
    }
    auto* job = new QKeychain::WritePasswordJob(QStringLiteral("AetherSDR"));
    job->setAutoDelete(true);
    job->setKey(QString::fromLatin1(kApiKeyKeychainKey));
    job->setTextData(key);
    QObject::connect(job, &QKeychain::Job::finished, job, [](QKeychain::Job* j) {
        if (j->error() != QKeychain::NoError) {
            qWarning() << "CopyAssist: keychain write for the remote ASR api"
                          " key failed:" << j->errorString();
        }
    });
    job->start();
}
#endif

QString loadApiKey()
{
    if (apiKeyLoaded()) {
        return apiKeyCache();
    }
    auto& s = AppSettings::instance();

    // 1. The XML import may have just diverted it into the session vault
    //    (and written it to the keychain when available).
    QString key = s.takeSessionCredential(QString::fromLatin1(kApiKeyKeychainKey));

#ifdef HAVE_KEYCHAIN
    // 2. The keychain is the persistent home.
    if (key.isEmpty()) {
        key = keychainReadApiKeyBlocking();
    }
#endif

    // 3. Defensive: a doc field written by an older build. Migrate it out.
    if (key.isEmpty()) {
        key = readObject().value(kApiKeyField).toString();
        if (!key.isEmpty()) {
#ifdef HAVE_KEYCHAIN
            keychainWriteApiKey(key);
#else
            s.setSessionCredential(QString::fromLatin1(kApiKeyKeychainKey), key);
            qWarning() << "CopyAssist: built without keychain support — the"
                          " remote ASR api key is session-only";
#endif
            stripApiKeyFromDoc();
        }
    }

    apiKeyCache() = key;
    apiKeyLoaded() = true;
    return key;
}

void storeApiKey(const QString& key)
{
    apiKeyCache() = key;
    apiKeyLoaded() = true;
#ifdef HAVE_KEYCHAIN
    keychainWriteApiKey(key);
#else
    AppSettings::instance().setSessionCredential(
        QString::fromLatin1(kApiKeyKeychainKey), key);
    if (!key.isEmpty()) {
        qWarning() << "CopyAssist: built without keychain support — the remote"
                      " ASR api key is session-only and must be re-entered"
                      " next launch";
    }
#endif
    stripApiKeyFromDoc();
}

// One-time: fold any legacy flat Asr* keys into the nested object and delete
// them, persisting the whole change atomically. Idempotent and cheap after the
// first run (nothing left to migrate). Guarded by a process-lifetime flag so it
// runs at most once regardless of which accessor is hit first (the panel reads
// AsrPanelHeight independently of the controller).
void ensureMigrated()
{
    static bool done = false;
    if (done) {
        return;
    }

    auto& s = AppSettings::instance();
    QMap<QString, QString> present;
    for (const QString& key : legacyFlatKeys()) {
        if (s.contains(key)) {
            present.insert(key, s.value(key).toString());
        }
    }
    if (present.isEmpty()) {
        // Nothing to fold. Latch `done` only once we can confirm this is a real
        // loaded state — the nested root key already exists (migration ran, or a
        // fresh install has since written a field). If BOTH the flat keys and the
        // root object are absent we may simply be pre-load, so leave `done` unset
        // and retry on the next access rather than permanently skipping migration
        // and orphaning the user's settings.
        if (s.contains(rootKey())) {
            done = true;
        }
        return;
    }
    done = true;
    const QJsonObject merged = foldLegacyKeys(present, readObject());
    s.setValue(rootKey(), QString::fromUtf8(QJsonDocument(merged).toJson(QJsonDocument::Compact)));
    for (auto it = present.constBegin(); it != present.constEnd(); ++it) {
        s.remove(it.key());
    }
    s.save();
}

} // namespace

QVariant value(const QString& field, const QVariant& defaultValue)
{
    {
        const QMutexLocker lock(&objectMutex());
        ensureMigrated();
    }
    if (field == kApiKeyField) {
        const QString key = loadApiKey();
        return key.isEmpty() ? defaultValue : QVariant(key);
    }
    const QMutexLocker lock(&objectMutex());
    const QJsonObject obj = readObject();
    const QJsonValue v = obj.value(field);
    return v.isUndefined() ? defaultValue : QVariant(v.toString());
}

void setValue(const QString& field, const QVariant& val)
{
    if (field == kApiKeyField) {
        {
            const QMutexLocker lock(&objectMutex());
            ensureMigrated();
        }
        storeApiKey(val.toString());
        return;
    }
    const QString text = val.toString();
    updateValue(field, [&text](const QString&) { return text; });
}

void updateValue(const QString& field, const std::function<QString(const QString&)>& update)
{
    if (field == kApiKeyField || !update) {
        return; // the api key lives in the keychain, not in this object
    }
    const QMutexLocker lock(&objectMutex());
    ensureMigrated();
    QJsonObject obj = readObject();
    obj.insert(field, update(obj.value(field).toString()));
    auto& s = AppSettings::instance();
    s.setValue(rootKey(), QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    s.save();
}

AsrAttempt adoptSurvivingFault()
{
    const QMutexLocker lock(&objectMutex());
    ensureMigrated();
    QJsonObject obj = readObject();
    const QString load = obj.value(QStringLiteral("AsrInFlight")).toString();
    const QString discovery = obj.value(QStringLiteral("AsrInFlightDiscovery")).toString();
    AsrAttempt died = asrAttemptFromJson(load);
    if (!died.isValid()) {
        died = asrAttemptFromJson(discovery);
    }
    if (load.isEmpty() && discovery.isEmpty()) {
        return {};
    }
    AsrAttempt adopted;
    if (died.isValid()) {
        adopted = asrMergeFault(
            asrAttemptFromJson(obj.value(QStringLiteral("AsrLastFault")).toString()), died);
        obj.insert(QStringLiteral("AsrLastFault"), asrAttemptToJson(adopted));
    }
    obj.insert(QStringLiteral("AsrInFlight"), QString());
    obj.insert(QStringLiteral("AsrInFlightDiscovery"), QString());
    // One document replacement and one SQLite transaction: interruption leaves
    // either the surviving markers or their adopted fault, never neither.
    auto& settings = AppSettings::instance();
    settings.setValue(rootKey(), QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    settings.save();
    return adopted;
}

} // namespace CopyAssistSettings
} // namespace AetherSDR
