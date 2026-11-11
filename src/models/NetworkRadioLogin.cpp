#include "NetworkRadioLogin.h"

#include "core/backends/IRadioBackend.h"
#include "core/AppSettings.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMap>
#include <QPointer>
#include <utility>

#ifdef HAVE_KEYCHAIN
#include <qt6keychain/keychain.h>
#endif

namespace AetherSDR {
namespace {
Q_LOGGING_CATEGORY(lcNetworkRadioLogin, "aether.radio.network-login")
QMap<QString, NetworkRadioLogin> s_staged;
QMap<QString, quint64> s_revisions;
quint64 s_nextRevision = 1;

NetworkRadioLogin storedLogin(const NetworkRadioProfile& profile)
{
    NetworkRadioLogin login;
    const QJsonObject document = QJsonDocument::fromJson(
        AppSettings::instance().value(profile.settingsKey).toString().toUtf8()).object();
    login.host = document.value(QStringLiteral("Host")).toString();
    login.username = document.value(QStringLiteral("Username")).toString();
    const int port = document.value(QStringLiteral("ControlPort")).toInt(profile.controlPort);
    login.controlPort = port >= 1 && port <= 65535 ? static_cast<quint16>(port) : profile.controlPort;
    login.audioPort = profile.audioPort;
    login.administrator = document.value(QStringLiteral("Administrator")).toBool(false);
    login.savePassword = document.value(QStringLiteral("SavePassword")).toBool(false);
    const QString variant = document.value(QStringLiteral("Variant")).toString();
    for (const NetworkRadioVariant& option : profile.variants) {
        if (option.id == variant) { login.variant = variant; break; }
    }
    return login;
}
}

QString networkRadioCredentialKey(const NetworkRadioProfile& profile, const NetworkRadioLogin& login)
{
    // Bind vault entries to the endpoint and account. A failed write for a new
    // connection must not expose a previous connection's password on reload.
    const QJsonArray identity{login.host.trimmed().toLower(), login.username,
        int(login.controlPort), login.administrator};
    const QByteArray digest = QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex();
    return profile.credentialKey + QLatin1Char('.') + QString::fromLatin1(digest);
}

QList<NetworkRadioProfile> networkRadioProfiles()
{
    return {{QStringLiteral("kenwood"), QStringLiteral("Kenwood TS-890S (network, experimental)"),
        QStringLiteral("TS-890S"), QStringLiteral("KenwoodConnection"),
        QStringLiteral("kenwood_password"), QStringLiteral("kenwood."), 60000, 60001,
        {{QString(), QStringLiteral("Unspecified (up to 60 MHz)")},
         {QStringLiteral("k"), QStringLiteral("K (up to 60 MHz)")},
         {QStringLiteral("e"), QStringLiteral("E/E2 (up to 74.8 MHz)")}}}};
}

std::optional<NetworkRadioProfile> networkRadioProfile(const QString& family)
{
    for (const NetworkRadioProfile& profile : networkRadioProfiles()) {
        if (profile.family.compare(family, Qt::CaseInsensitive) == 0) { return profile; }
    }
    return std::nullopt;
}

void NetworkRadioLoginTestAccess::clearSession()
{
    s_staged.clear();
    s_revisions.clear();
    for (const NetworkRadioProfile& profile : networkRadioProfiles()) {
        AppSettings::instance().setSessionCredential(profile.credentialKey, {});
    }
}

void loadNetworkRadioLogin(const QString& family, QObject* context,
                          std::function<void(NetworkRadioLogin)> callback,
                          const NetworkRadioCredentialStore* store)
{
    const std::optional<NetworkRadioProfile> profile = networkRadioProfile(family);
    if (!profile || !context) { return; }
    if (s_staged.contains(profile->family)) { callback(s_staged.value(profile->family)); return; }
    NetworkRadioLogin login = storedLogin(*profile);
    login.password = AppSettings::instance().takeSessionCredential(profile->credentialKey);
    if (!login.password.isEmpty() || !login.savePassword) {
        s_staged.insert(profile->family, login);
        s_revisions.insert(profile->family, s_nextRevision++);
        callback(login);
        return;
    }
    const QPointer<QObject> alive(context);
    auto completed = [alive, family = profile->family, login,
                           callback = std::move(callback)](QString password) mutable {
        if (!alive) { return; }
        login.password = std::move(password);
        // A newer operator edit wins over a delayed credential-store result.
        if (!s_staged.contains(family)) {
            s_staged.insert(family, login);
            s_revisions.insert(family, s_nextRevision++);
        }
        callback(s_staged.value(family));
    };
    if (store && store->read) {
        store->read(networkRadioCredentialKey(*profile, login), context, completed);
        return;
    }
#ifdef HAVE_KEYCHAIN
    auto* job = new QKeychain::ReadPasswordJob(QStringLiteral("AetherSDR"), context);
    job->setKey(networkRadioCredentialKey(*profile, login));
    QObject::connect(job, &QKeychain::Job::finished, context,
        [completed](QKeychain::Job* result) mutable {
            QString password;
            if (result->error() == QKeychain::NoError) {
                password = static_cast<QKeychain::ReadPasswordJob*>(result)->textData();
            } else if (result->error() != QKeychain::EntryNotFound) {
                qCWarning(lcNetworkRadioLogin) << "Network radio keychain read failed:" << result->errorString();
            }
            completed(std::move(password));
        });
    job->start();
#else
    completed({});
#endif
}

void invalidateNetworkRadioLoginRevision(const QString& family)
{
    if (const auto profile = networkRadioProfile(family); profile && s_staged.contains(profile->family)) {
        s_staged[profile->family].savePassword = false;
        s_revisions.insert(profile->family, s_nextRevision++);
    }
}

bool networkRadioPasswordStoreAvailable()
{
#ifdef HAVE_KEYCHAIN
    return true;
#else
    return false;
#endif
}

void stageNetworkRadioLogin(const QString& family, const NetworkRadioLogin& login)
{
    if (const std::optional<NetworkRadioProfile> profile = networkRadioProfile(family)) {
        s_staged.insert(profile->family, login);
        s_revisions.insert(profile->family, s_nextRevision++);
        AppSettings::instance().setSessionCredential(profile->credentialKey, login.password);
    }
}

void populateNetworkRadioParams(RadioConnectRequest& request, const QString& family)
{
    const std::optional<NetworkRadioProfile> profile = networkRadioProfile(family);
    if (!profile) { return; }
    const NetworkRadioLogin login = s_staged.value(profile->family, storedLogin(*profile));
    const quint16 controlPort = request.port ? request.port : profile->controlPort;
    // Credentials belong to one endpoint, including a mapped TCP port.
    if (login.host != request.host || login.controlPort != controlPort) { return; }
    request.params.insert(profile->parameterPrefix + QStringLiteral("username"), login.username);
    request.params.insert(profile->parameterPrefix + QStringLiteral("password"), login.password);
    request.params.insert(profile->parameterPrefix + QStringLiteral("administrator"), login.administrator);
    request.params.insert(profile->parameterPrefix + QStringLiteral("audioPort"), login.audioPort);
    request.params.insert(profile->parameterPrefix + QStringLiteral("variant"), login.variant);
    request.params.insert(QStringLiteral("connection.loginRevision"),
                          QVariant::fromValue(s_revisions.value(profile->family)));
}

void commitNetworkRadioLogin(const QString& family, const QString& host,
                             quint64 revision, QObject* context, const NetworkRadioCredentialStore* store)
{
    const std::optional<NetworkRadioProfile> profile = networkRadioProfile(family);
    if (!profile || !context || !s_staged.contains(profile->family) || revision == 0
        || s_revisions.value(profile->family) != revision) { return; }
    const NetworkRadioLogin login = s_staged.value(profile->family);
    if (login.host != host) { return; }
    AppSettings& settings = AppSettings::instance();
    const QJsonObject existing = QJsonDocument::fromJson(
        settings.value(profile->settingsKey).toString().toUtf8()).object();
    if (existing.value(QStringLiteral("SchemaVersion")).toInt(1) > 1) {
        qCWarning(lcNetworkRadioLogin) << "Network radio settings have a newer schema; refusing to overwrite";
        return;
    }
    QJsonObject document{{"SchemaVersion", 1}, {"Host", login.host}, {"Username", login.username},
        {"ControlPort", login.controlPort}, {"AudioPort", login.audioPort}, {"Administrator", login.administrator},
        {"Variant", login.variant}, {"SavePassword", login.savePassword}};
    settings.setValue(profile->settingsKey, QString::fromUtf8(QJsonDocument(document).toJson(QJsonDocument::Compact)));
    settings.save();
    settings.setSessionCredential(profile->credentialKey, login.password);
    if (!login.savePassword) { return; }
    if (store && store->write) {
        store->write(networkRadioCredentialKey(*profile, login), login.password, context);
        return;
    }
#ifdef HAVE_KEYCHAIN
    auto* job = new QKeychain::WritePasswordJob(QStringLiteral("AetherSDR"), context);
    job->setKey(networkRadioCredentialKey(*profile, login));
    job->setTextData(login.password);
    QObject::connect(job, &QKeychain::Job::finished, context, [](QKeychain::Job* completed) {
        if (completed->error() != QKeychain::NoError) {
            qCWarning(lcNetworkRadioLogin) << "Network radio password was not saved:" << completed->errorString();
        }
    });
    job->start();
#else
    qCWarning(lcNetworkRadioLogin) << "Keychain support is unavailable; network radio password is session-only";
#endif
}
} // namespace AetherSDR
