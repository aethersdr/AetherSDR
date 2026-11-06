#include "ConnectionWorkspace.h"
#include "core/AppSettings.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QSet>
#include <QStringList>
#include <QUuid>
#include <algorithm>

namespace AetherSDR {
namespace {
Q_LOGGING_CATEGORY(lcWorkspace, "aether.connection.workspace")
constexpr auto kFeature = "ConnectionWorkspace";

bool fail(QString* error, const QString& message)
{
    if (error) { *error = message; }
    qCWarning(lcWorkspace) << message;
    return false;
}

bool hasSecrets(const QJsonValue& value)
{
    if (value.isArray()) {
        for (const QJsonValue& item : value.toArray()) {
            if (hasSecrets(item)) { return true; }
        }
    } else if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            const QString key = it.key().toLower();
            if (key.contains("password") || key.contains("token") || key.contains("secret")
                || hasSecrets(it.value())) { return true; }
        }
    }
    return false;
}
bool validate(const QJsonObject& document, QString* error)
{
    if (document.value("version").toInt() != ConnectionWorkspace::kVersion || hasSecrets(document)
        || QJsonDocument(document).toJson(QJsonDocument::Compact).size() > 1024 * 1024) {
        return fail(error, QStringLiteral("Invalid connection workspace document."));
    }
    const QJsonArray accounts = document.value("accounts").toArray();
    for (const QJsonValue& value : accounts) {
        const QJsonObject account = value.toObject();
        const QString id = account.value("id").toString();
        const bool validId = id == QLatin1String("legacy")
            || (id.size() == 64 && std::all_of(id.cbegin(), id.cend(), [](QChar c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }));
        if (!validId || account.value("email").toString().size() > 254) {
            return fail(error, QStringLiteral("Invalid account reference."));
        }
    }
    for (const QJsonValue& value : document.value("operators").toArray()) {
        const QJsonObject preferences = value.toObject().value("preferences").toObject();
        for (const QString& key : {QStringLiteral("inputDeviceId"), QStringLiteral("outputDeviceId")}) {
            const QString encoded = preferences.value(key).toString();
            if (encoded.size() > 8192 || !QByteArray::fromBase64Encoding(encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors)) {
                return fail(error, QStringLiteral("Invalid audio device identity."));
            }
        }
        const QStringList layouts{"1", "2v", "2h", "2h1", "12h", "3v", "2x2", "4v", "3h2", "2x3", "4h3", "2x4"};
        if (!layouts.contains(preferences.value("layout").toString())) {
            return fail(error, QStringLiteral("Invalid operator layout."));
        }
    }
    for (const QJsonValue& value : document.value("stations").toArray()) {
        const QJsonObject station = value.toObject();
        if (!station.value("connections").isArray() || station.value("connections").toArray().size() > 256) {
            return fail(error, QStringLiteral("Invalid saved connection collection."));
        }
        QSet<QString> connectionIds;
        for (const QJsonValue& entry : station.value("connections").toArray()) {
            const QJsonObject connection = entry.toObject();
            const QString id = connection.value("id").toString();
            const QString kind = connection.value("kind").toString();
            if (id.isEmpty() || id.size() > 128 || connectionIds.contains(id)
                || (kind != QLatin1String("local") && kind != QLatin1String("manual") && kind != QLatin1String("smartlink"))
                || connection.value("host").toString().size() > 253 || connection.value("serial").toString().size() > 128
                || connection.value("family").toString().size() > 32) {
                return fail(error, QStringLiteral("Invalid saved connection."));
            }
            connectionIds.insert(id);
        }
    }
    for (const QString& key : {QStringLiteral("stations"), QStringLiteral("operators"), QStringLiteral("accounts")}) {
        const QJsonArray records = document.value(key).toArray();
        if (!document.value(key).isArray() || records.size() > 256
            || (records.isEmpty() && key != QLatin1String("accounts"))) {
            return fail(error, QStringLiteral("Invalid or empty profile collection."));
        }
        QSet<QString> ids;
        QSet<QString> names;
        for (const QJsonValue& value : records) {
            const QJsonObject record = value.toObject();
            const QString id = record.value("id").toString();
            const QString name = record.value("name").toString().trimmed();
            if (id.isEmpty() || id.size() > 128 || ids.contains(id) || name.isEmpty()
                || name.size() > 128 || names.contains(name.toCaseFolded())) {
                return fail(error, QStringLiteral("Profile names and identities must be unique."));
            }
            ids.insert(id);
            names.insert(name.toCaseFolded());
        }
        const QString selectionKey = key == QLatin1String("stations") ? QStringLiteral("selectedStation")
            : key == QLatin1String("operators") ? QStringLiteral("selectedOperator") : QStringLiteral("selectedAccount");
        if (!records.isEmpty() && !ids.contains(document.value(selectionKey).toString())) {
            return fail(error, QStringLiteral("Selected profile is missing."));
        }
    }
    return true;
}
} // namespace

QString ConnectionWorkspace::newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QJsonObject ConnectionWorkspace::find(const QJsonArray& records, const QString& id)
{
    for (const QJsonValue& value : records) {
        const QJsonObject record = value.toObject();
        if (record.value("id").toString() == id) { return record; }
    }
    return {};
}

bool ConnectionWorkspace::uniqueName(const QJsonArray& records, const QString& name,
                                      const QString& exceptId)
{
    if (name.trimmed().isEmpty() || name.size() > 128) { return false; }
    for (const QJsonValue& value : records) {
        const QJsonObject record = value.toObject();
        if (record.value("id").toString() != exceptId
            && record.value("name").toString().compare(name.trimmed(), Qt::CaseInsensitive) == 0) {
            return false;
        }
    }
    return true;
}

QJsonObject ConnectionWorkspace::defaultOperatorPreferences()
{
    return {{"inputDeviceId", ""}, {"outputDeviceId", ""}, {"layout", "1"}};
}

QJsonObject ConnectionWorkspace::captureOperatorPreferences()
{
    const AppSettings& settings = AppSettings::instance();
    return {{"inputDeviceId", QString::fromLatin1(settings.value("AudioInputDeviceId").toByteArray().toBase64())},
            {"outputDeviceId", QString::fromLatin1(settings.value("AudioOutputDeviceId").toByteArray().toBase64())},
            {"layout", settings.value("PanadapterLayout", "1").toString()}};
}

QString ConnectionWorkspace::accountId(const QString& email)
{
    return QString::fromLatin1(QCryptographicHash::hash(
        email.trimmed().toCaseFolded().toUtf8(), QCryptographicHash::Sha256).toHex());
}

QString ConnectionWorkspace::credentialKey(const QString& id)
{
    // Imported accounts retain the existing key; migration never copies or
    // deletes a working credential before the replacement is established.
    return id.isEmpty() || id == QLatin1String("legacy")
        ? QStringLiteral("smartlink_refresh_token")
        : QStringLiteral("smartlink/%1/refresh_token").arg(id);
}

QString ConnectionWorkspace::connectionCredentialId(const QString& host, const QString& username)
{
    return QString::fromLatin1(QCryptographicHash::hash(
        (host.trimmed().toLower() + QLatin1Char('\n') + username).toUtf8(), QCryptographicHash::Sha256).toHex());
}

QJsonObject ConnectionWorkspace::load()
{
    AppSettings& settings = AppSettings::instance();
    int version = 0;
    AppSettings::FeatureReadStatus status;
    QJsonObject document = settings.radioFeatureExact("local", "", kFeature, &version, &status);
    if (status != AppSettings::FeatureReadStatus::Missing) {
        if (status != AppSettings::FeatureReadStatus::Present || version != kVersion) {
            qCWarning(lcWorkspace) << "Connection workspace unavailable or newer than this build";
            return {};
        }
        if (!validate(document, nullptr)) { return {}; }
        QJsonArray operators = document.value("operators").toArray();
        if (uniqueName(operators, "Radio operator")) {
            for (qsizetype i = 0; i < operators.size(); ++i) {
                QJsonObject profile = operators[i].toObject();
                if (profile.value("name").toString() == QLatin1String("Existing preferences")) {
                    profile["name"] = "Radio operator";
                    operators[i] = profile;
                    document["operators"] = operators;
                    if (!save(document)) { return {}; }
                    break;
                }
            }
        }
        if (!document.value("starterStationsCreated").toBool()) {
            QJsonArray stations = document.value("stations").toArray();
            if (uniqueName(stations, "Home")) {
                for (qsizetype i = 0; i < stations.size(); ++i) {
                    QJsonObject station = stations[i].toObject();
                    if (station.value("name").toString() == QLatin1String("Existing setup")) {
                        station["name"] = "Home";
                        stations[i] = station;
                        break;
                    }
                }
            }
            for (const QString& name : {QStringLiteral("Home"), QStringLiteral("POTA"), QStringLiteral("Club"), QStringLiteral("RV")}) {
                if (uniqueName(stations, name)) {
                    stations.append(QJsonObject{{"id", newId()}, {"name", name}, {"connections", QJsonArray()}});
                }
            }
            document["stations"] = stations;
            document["starterStationsCreated"] = true;
            if (!save(document)) { return {}; }
        }
        return document;
    }

    const QString stationId = newId();
    const QString operatorId = newId();
    QJsonArray connections;
    const QJsonObject routed = QJsonDocument::fromJson(
        settings.value("RoutedProfilesJson", "{}").toString().toUtf8()).object();
    const QJsonObject legacyIcom = QJsonDocument::fromJson(settings.value("Icom").toString().toUtf8()).object();
    QSet<QString> imported;
    auto importAddress = [&](const QString& host, const QJsonObject& old) {
        if (host.trimmed().isEmpty() || imported.contains(host)) { return; }
        imported.insert(host);
        const QJsonObject identity = old.value("identity").toObject();
        const QString civSelection = legacyIcom.value("civSelection").toString();
        const int civAddress = legacyIcom.value("civAddress").toInt(0xa4);
        const QString civ = civSelection == QLatin1String("model") ? QStringLiteral("%1").arg(civAddress, 2, 16, QLatin1Char('0')).toUpper()
            : civSelection == QLatin1String("custom") || (civSelection.isEmpty() && civAddress != 0xa4)
                ? QStringLiteral("__custom__") : QStringLiteral("__auto__");
        connections.append(QJsonObject{{"id", newId()}, {"name", host.left(110)}, {"kind", "manual"},
            {"family", identity.value("family").toString("flex")}, {"host", host},
            {"username", legacyIcom.value("username").toString()}, {"civ", civ},
            {"customCiv", QString::number(civAddress, 16)},
            {"legacyCredentials", host == legacyIcom.value("lastHost").toString() || host == settings.value("LastRoutedRadioIp").toString()},
            {"route", old}, {"lowBandwidth", settings.value("LowBandwidthRemotePreferred", "False").toString() == "True"}});
    };
    for (auto it = routed.begin(); it != routed.end(); ++it) { importAddress(it.key(), it.value().toObject()); }
    for (const QJsonValue& host : QJsonDocument::fromJson(settings
             .value("RecentConnectByIpAddresses", "[]").toString().toUtf8()).array()) {
        importAddress(host.toString(), {});
    }
    importAddress(settings.value("LastRoutedRadioIp").toString(), {});
    const QString serial = settings.value("LastConnectedRadioSerial").toString();
    if (!serial.isEmpty() && settings.value("LastRoutedRadioIp").toString().isEmpty()) {
        connections.append(QJsonObject{{"id", newId()}, {"name", "Last radio"},
            {"kind", "local"}, {"serial", serial}, {"family", ""}});
    }

    QJsonArray accounts;
    const QString email = QString::fromUtf8(QByteArray::fromBase64(
        settings.value("SmartLinkEmail").toString().toUtf8()));
    // A legacy refresh token can exist without an email; keep its reference.
    accounts.append(QJsonObject{{"id", "legacy"}, {"name", email.isEmpty() ? QStringLiteral("Existing account") : email.left(128)},
                                 {"email", email}});
    document = {{"version", kVersion}, {"selectedStation", stationId}, {"selectedOperator", operatorId},
        {"selectedAccount", "legacy"}, {"accounts", accounts},
        {"starterStationsCreated", true},
        {"stations", QJsonArray{QJsonObject{{"id", stationId}, {"name", "Home"}, {"connections", connections}},
            QJsonObject{{"id", newId()}, {"name", "POTA"}, {"connections", QJsonArray()}},
            QJsonObject{{"id", newId()}, {"name", "Club"}, {"connections", QJsonArray()}},
            QJsonObject{{"id", newId()}, {"name", "RV"}, {"connections", QJsonArray()}}}},
        {"operators", QJsonArray{QJsonObject{{"id", operatorId}, {"name", "Radio operator"},
                                             {"preferences", captureOperatorPreferences()}}}}};
    return save(document) ? document : QJsonObject{};
}

bool ConnectionWorkspace::save(const QJsonObject& document, QString* error)
{
    AppSettings& settings = AppSettings::instance();
    int version = 0;
    AppSettings::FeatureReadStatus status;
    settings.radioFeatureExact("local", "", kFeature, &version, &status);
    if (status == AppSettings::FeatureReadStatus::Unavailable
        || status == AppSettings::FeatureReadStatus::Corrupt || version > kVersion) {
        return fail(error, QStringLiteral("Connection workspace is unavailable or uses a newer schema."));
    }
    if (!validate(document, error)) { return false; }
    if (!settings.setRadioFeature("local", "", kFeature, kVersion, document)) {
        return fail(error, QStringLiteral("Connection profiles could not be saved. Check the settings store."));
    }
    return true;
}

} // namespace AetherSDR
