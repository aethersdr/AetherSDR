#include "core/AutomationBridgeSettings.h"

#include "core/AppSettings.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>

#ifdef HAVE_KEYCHAIN
#include <qt6keychain/keychain.h>
#endif

namespace AetherSDR {

namespace {
// Single nested-JSON key holding the bridge's non-secret config (Principle V).
// Shape: {"enabled":bool,"txAllowed":bool,"txAck":bool}.
const QString kRootKey = QStringLiteral("AutomationBridge");

// Legacy flat keys (pre-nesting). Migrated one-shot into kRootKey on first read.
const QString kLegacyEnabled   = QStringLiteral("AutomationBridgeEnabled");
const QString kLegacyTxAllowed = QStringLiteral("AutomationBridgeTxAllowed");
const QString kLegacyTxAck     = QStringLiteral("AutomationBridgeTxAck");

constexpr const char* kFieldEnabled   = "enabled";
constexpr const char* kFieldTxAllowed = "txAllowed";
constexpr const char* kFieldTxAck     = "txAck";

// Keychain coordinates for the token (analogous to MqttSettings).
constexpr const char* kKeychainService  = "AetherSDR";
constexpr const char* kKeychainKey      = "automation_bridge_token";
const QString kLegacyTokenKey = QStringLiteral("AutomationBridgeToken");
} // namespace

QJsonObject AutomationBridgeSettings::readObj()
{
    auto& s = AppSettings::instance();
    const QString json = s.value(kRootKey, QString{}).toString();
    if (!json.isEmpty())
        return QJsonDocument::fromJson(json.toUtf8()).object();

    // No nested object yet. If any legacy flat key exists, migrate them once
    // into the nested object and drop the flat keys; otherwise return defaults.
    const bool hasLegacy = !s.value(kLegacyEnabled).toString().isEmpty()
                        || !s.value(kLegacyTxAllowed).toString().isEmpty()
                        || !s.value(kLegacyTxAck).toString().isEmpty();
    if (!hasLegacy)
        return {};

    QJsonObject o;
    o[QLatin1String(kFieldEnabled)]   = s.value(kLegacyEnabled, false).toBool();
    o[QLatin1String(kFieldTxAllowed)] = s.value(kLegacyTxAllowed, false).toBool();
    o[QLatin1String(kFieldTxAck)]     = s.value(kLegacyTxAck, false).toBool();
    s.setValue(kRootKey, QString::fromUtf8(
                   QJsonDocument(o).toJson(QJsonDocument::Compact)));
    s.remove(kLegacyEnabled);
    s.remove(kLegacyTxAllowed);
    s.remove(kLegacyTxAck);
    s.save();
    return o;
}

void AutomationBridgeSettings::writeBool(const char* field, bool value)
{
    // Read-modify-write the whole object so the three bools are always
    // persisted together (atomic — never enabled=true with a half-written peer).
    QJsonObject o = readObj();
    o[QLatin1String(field)] = value;
    auto& s = AppSettings::instance();
    s.setValue(kRootKey, QString::fromUtf8(
                   QJsonDocument(o).toJson(QJsonDocument::Compact)));
    s.save();
}

bool AutomationBridgeSettings::enabled()
{
    return readObj().value(QLatin1String(kFieldEnabled)).toBool(false);
}
void AutomationBridgeSettings::setEnabled(bool on) { writeBool(kFieldEnabled, on); }

bool AutomationBridgeSettings::txAllowed()
{
    return readObj().value(QLatin1String(kFieldTxAllowed)).toBool(false);
}
void AutomationBridgeSettings::setTxAllowed(bool on) { writeBool(kFieldTxAllowed, on); }

bool AutomationBridgeSettings::txAck()
{
    return readObj().value(QLatin1String(kFieldTxAck)).toBool(false);
}
void AutomationBridgeSettings::setTxAck(bool on) { writeBool(kFieldTxAck, on); }

QString AutomationBridgeSettings::keychainService()
{
    return QString::fromLatin1(kKeychainService);
}
QString AutomationBridgeSettings::keychainKey()
{
    return QString::fromLatin1(kKeychainKey);
}
QString AutomationBridgeSettings::legacyTokenSettingKey() { return kLegacyTokenKey; }

void AutomationBridgeSettings::loadToken(QObject* ctx,
                                         std::function<void(const QString&)> cb)
{
    // Env override wins (headless / CI), regardless of keychain availability.
    const QByteArray envTok = qgetenv("AETHER_MCP_TOKEN");
    if (!envTok.isEmpty()) {
        cb(QString::fromUtf8(envTok));
        return;
    }

#ifdef HAVE_KEYCHAIN
    auto& s = AppSettings::instance();
    const QString legacy = s.value(kLegacyTokenKey).toString();
    if (!legacy.isEmpty()) {
        // One-shot migrate the old plaintext token into the OS secret store,
        // then drop the plaintext copy. Hand the caller the value immediately.
        auto* job = new QKeychain::WritePasswordJob(keychainService());
        job->setAutoDelete(true);
        job->setKey(keychainKey());
        job->setTextData(legacy);
        QObject::connect(job, &QKeychain::Job::finished, ctx ? ctx : job,
                         [](QKeychain::Job* j) {
            if (j->error() == QKeychain::NoError) {
                AppSettings::instance().remove(kLegacyTokenKey);
                AppSettings::instance().save();
            } else {
                qWarning() << "AutomationBridge: token keychain migration failed:"
                           << j->errorString() << "- plaintext preserved for retry";
            }
        });
        job->start();
        cb(legacy);
        return;
    }
    auto* job = new QKeychain::ReadPasswordJob(keychainService());
    job->setAutoDelete(true);
    job->setKey(keychainKey());
    QObject::connect(job, &QKeychain::Job::finished, ctx ? ctx : job,
                     [cb = std::move(cb)](QKeychain::Job* j) {
        if (j->error() == QKeychain::NoError) {
            cb(static_cast<QKeychain::ReadPasswordJob*>(j)->textData());
        } else {
            if (j->error() != QKeychain::EntryNotFound)
                qWarning() << "AutomationBridge: token keychain read failed:"
                           << j->errorString();
            cb(QString{});
        }
    });
    job->start();
#else
    const QString legacy = AppSettings::instance().value(kLegacyTokenKey).toString();
    if (!legacy.isEmpty())
        qWarning() << "AutomationBridge: HAVE_KEYCHAIN not set - token remains in "
                      "plaintext AppSettings";
    cb(legacy);
#endif
}

void AutomationBridgeSettings::saveToken(const QString& token)
{
#ifdef HAVE_KEYCHAIN
    // Never leave a plaintext copy once the keychain is authoritative.
    AppSettings::instance().remove(kLegacyTokenKey);
    AppSettings::instance().save();
    if (token.isEmpty()) {
        auto* job = new QKeychain::DeletePasswordJob(keychainService());
        job->setAutoDelete(true);
        job->setKey(keychainKey());
        QObject::connect(job, &QKeychain::Job::finished, job, [](QKeychain::Job* j) {
            if (j->error() != QKeychain::NoError
                && j->error() != QKeychain::EntryNotFound) {
                qWarning() << "AutomationBridge: token keychain delete failed:"
                           << j->errorString();
            }
        });
        job->start();
        return;
    }
    auto* job = new QKeychain::WritePasswordJob(keychainService());
    job->setAutoDelete(true);
    job->setKey(keychainKey());
    job->setTextData(token);
    QObject::connect(job, &QKeychain::Job::finished, job, [](QKeychain::Job* j) {
        if (j->error() != QKeychain::NoError)
            qWarning() << "AutomationBridge: token keychain save failed:"
                       << j->errorString();
    });
    job->start();
#else
    auto& s = AppSettings::instance();
    if (token.isEmpty()) {
        s.remove(kLegacyTokenKey);
    } else {
        qWarning() << "AutomationBridge: HAVE_KEYCHAIN not set - saving token to "
                      "plaintext AppSettings";
        s.setValue(kLegacyTokenKey, token);
    }
    s.save();
#endif
}

} // namespace AetherSDR
