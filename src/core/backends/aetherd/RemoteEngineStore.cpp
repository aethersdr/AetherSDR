#include "RemoteEngineStore.h"

#include "core/AppSettings.h"
#include "core/SettingsCredentialPolicy.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

#include <memory>

#ifdef HAVE_KEYCHAIN
#include <qt6keychain/keychain.h>
#endif

namespace AetherSDR::aetherd {

namespace {

#ifdef HAVE_KEYCHAIN
Q_LOGGING_CATEGORY(lcRemoteEngineStore, "aether.backend.aetherd.store")
#endif

const QString kRootKey = QStringLiteral("RemoteEngines");
constexpr const char* kKeychainService = "AetherSDR";

bool validEngineId(const QString& id)
{
    static const QRegularExpression hex(QStringLiteral("^[a-f0-9]{64}$"));
    return hex.match(id).hasMatch();
}

bool validUrl(const QString& text, const char* scheme)
{
    const QUrl url(text, QUrl::StrictMode);
    QHostAddress host;
    return url.isValid() && url.scheme() == QLatin1String(scheme) && host.setAddress(url.host())
        && url.port() > 0 && url.path().isEmpty() && url.userInfo().isEmpty() && !url.hasQuery();
}

QJsonObject readRoot()
{
    const QString json = AppSettings::instance().value(kRootKey, QString{}).toString();
    return json.isEmpty() ? QJsonObject{} : QJsonDocument::fromJson(json.toUtf8()).object();
}

void writeRoot(const QJsonObject& root)
{
    auto& settings = AppSettings::instance();
    settings.setValue(kRootKey, QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    settings.save();
}

RemoteEngineRecord fromJson(const QJsonObject& object)
{
    RemoteEngineRecord record;
    record.engineId = object.value(QStringLiteral("engineId")).toString();
    record.name = object.value(QStringLiteral("name")).toString().left(64);
    record.lanUrl = object.value(QStringLiteral("lanUrl")).toString();
    record.tailnetUrl = object.value(QStringLiteral("tailnetUrl")).toString();
    return record;
}

QJsonObject toJson(const RemoteEngineRecord& record)
{
    QJsonObject object{{QStringLiteral("engineId"), record.engineId},
                       {QStringLiteral("name"), record.name.left(64)},
                       {QStringLiteral("lanUrl"), record.lanUrl}};
    if (!record.tailnetUrl.isEmpty()) { object.insert(QStringLiteral("tailnetUrl"), record.tailnetUrl); }
    return object;
}

void writeRecords(const QList<RemoteEngineRecord>& records)
{
    QJsonArray engines;
    for (const RemoteEngineRecord& record : records) { engines.append(toJson(record)); }
    QJsonObject root = readRoot();
    root.insert(QStringLiteral("engines"), engines);
    writeRoot(root);
}

} // namespace

bool RemoteEngineStore::validLanUrl(const QString& url) { return validUrl(url, "wss"); }
bool RemoteEngineStore::validTailnetUrl(const QString& url) { return validUrl(url, "ws"); }

QList<RemoteEngineRecord> RemoteEngineStore::records()
{
    QList<RemoteEngineRecord> out;
    for (const QJsonValue& value : readRoot().value(QStringLiteral("engines")).toArray()) {
        RemoteEngineRecord record = fromJson(value.toObject());
        if (!validEngineId(record.engineId) || !validLanUrl(record.lanUrl)) { continue; }
        if (!record.tailnetUrl.isEmpty() && !validTailnetUrl(record.tailnetUrl)) { record.tailnetUrl.clear(); }
        out.append(record);
    }
    return out;
}

std::optional<RemoteEngineRecord> RemoteEngineStore::record(const QString& engineId)
{
    for (const RemoteEngineRecord& record : records()) {
        if (record.engineId == engineId) { return record; }
    }
    return std::nullopt;
}

bool RemoteEngineStore::persistentStoreAvailable()
{
#ifdef HAVE_KEYCHAIN
    return true;
#else
    return false;
#endif
}

void RemoteEngineStore::save(const RemoteEngineRecord& record)
{
    if (!validEngineId(record.engineId) || !validLanUrl(record.lanUrl) || record.token.isEmpty()) { return; }
    RemoteEngineCredentials::setSession(record);
    QList<RemoteEngineRecord> all = records();
    all.erase(std::remove_if(all.begin(), all.end(),
                             [&](const RemoteEngineRecord& r) { return r.engineId == record.engineId; }),
              all.end());
    RemoteEngineRecord stored = record;
    stored.token.clear();
    all.append(stored);
    writeRecords(all);
    const QString key = SettingsCredentialPolicy::aetherdCredentialKey(record.engineId);
#ifdef HAVE_KEYCHAIN
    auto* job = new QKeychain::WritePasswordJob(QLatin1String(kKeychainService));
    job->setAutoDelete(true);
    job->setKey(key);
    job->setInsecureFallback(false);
    job->setTextData(record.token);
    QObject::connect(job, &QKeychain::Job::finished, job, [](QKeychain::Job* finished) {
        if (finished->error() != QKeychain::NoError) {
            qCWarning(lcRemoteEngineStore) << "remote engine token write failed:" << finished->errorString();
        }
    });
    job->start();
#else
    AppSettings::instance().setSessionCredential(key, record.token);
#endif
}

bool RemoteEngineStore::setTailnetUrl(const QString& engineId, const QString& tailnetUrl)
{
    if (!tailnetUrl.isEmpty() && !validTailnetUrl(tailnetUrl)) { return false; }
    QList<RemoteEngineRecord> all = records();
    bool found = false;
    for (RemoteEngineRecord& record : all) {
        if (record.engineId == engineId) {
            record.tailnetUrl = tailnetUrl;
            found = true;
        }
    }
    if (!found) { return false; }
    writeRecords(all);
    if (std::optional<RemoteEngineRecord> session = RemoteEngineCredentials::session(engineId)) {
        session->tailnetUrl = tailnetUrl;
        RemoteEngineCredentials::setSession(*session);
    }
    return true;
}

void RemoteEngineStore::forget(const QString& engineId)
{
    QList<RemoteEngineRecord> all = records();
    all.erase(std::remove_if(all.begin(), all.end(),
                             [&](const RemoteEngineRecord& r) { return r.engineId == engineId; }),
              all.end());
    writeRecords(all);
    RemoteEngineCredentials::clearSession(engineId);
    const QString key = SettingsCredentialPolicy::aetherdCredentialKey(engineId);
#ifdef HAVE_KEYCHAIN
    auto* job = new QKeychain::DeletePasswordJob(QLatin1String(kKeychainService));
    job->setAutoDelete(true);
    job->setKey(key);
    job->setInsecureFallback(false);
    job->start();
#else
    (void)AppSettings::instance().takeSessionCredential(key);
#endif
}

void RemoteEngineStore::primeSessions(QObject* context, std::function<void(int)> done)
{
    if (!context) { return; }
    const QList<RemoteEngineRecord> all = records();
    auto remaining = std::make_shared<int>(int(all.size()));
    auto primed = std::make_shared<int>(0);
    const QPointer<QObject> guard(context);
    const auto finishOne = [remaining, primed, done, guard] {
        if (--*remaining == 0 && done && guard) { done(*primed); }
    };
    if (all.isEmpty()) {
        QTimer::singleShot(0, context, [done] { if (done) { done(0); } });
        return;
    }
    for (const RemoteEngineRecord& record : all) {
        if (const auto cached = RemoteEngineCredentials::session(record.engineId); cached && cached->usable()) {
            ++*primed;
            QTimer::singleShot(0, context, finishOne);
            continue;
        }
#ifdef HAVE_KEYCHAIN
        auto* job = new QKeychain::ReadPasswordJob(QLatin1String(kKeychainService));
        job->setAutoDelete(true);
        job->setKey(SettingsCredentialPolicy::aetherdCredentialKey(record.engineId));
        job->setInsecureFallback(false);
        QObject::connect(job, &QKeychain::Job::finished, context, [record, primed, finishOne](QKeychain::Job* finished) {
            const QString token = finished->error() == QKeychain::NoError
                ? static_cast<QKeychain::ReadPasswordJob*>(finished)->textData() : QString();
            if (!token.isEmpty()) {
                RemoteEngineRecord withToken = record;
                withToken.token = token;
                RemoteEngineCredentials::setSession(withToken);
                ++*primed;
            }
            finishOne();
        });
        job->start();
#else
        QTimer::singleShot(0, context, finishOne);
#endif
    }
}

} // namespace AetherSDR::aetherd
