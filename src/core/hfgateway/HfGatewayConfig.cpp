#include "core/hfgateway/HfGatewayConfig.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>

namespace AetherSDR::HfGateway {

namespace {

const QString kVersion = QStringLiteral("version");
const QString kEngines = QStringLiteral("engines");
const QString kBackend = QStringLiteral("backend");
const QString kId = QStringLiteral("id");
const QString kProfile = QStringLiteral("profile");
const QString kLabel = QStringLiteral("label");
const QString kHost = QStringLiteral("host");
const QString kCommandPort = QStringLiteral("commandPort");
const QString kDataPort = QStringLiteral("dataPort");
const QString kLoopbackOnly = QStringLiteral("loopbackOnly");

constexpr int kMaxLabelChars = 64;

quint16 portOr(const QJsonObject& o, const QString& key, quint16 fallback)
{
    const QJsonValue v = o.value(key);
    if (!v.isDouble()) {
        return fallback;
    }
    const double d = v.toDouble();
    const int p = static_cast<int>(d);
    if (static_cast<double>(p) != d || !isValidPort(p)) {
        return fallback;
    }
    return static_cast<quint16>(p);
}

QString cleanLabel(const QString& text)
{
    QString out;
    for (const QChar c : text) {
        if (c.isPrint()) {
            out.append(c);
        }
    }
    return out.left(kMaxLabelChars).trimmed();
}

}  // namespace

QString toString(Profile profile)
{
    switch (profile) {
    case Profile::VaraCompatible: return QStringLiteral("vara-compatible");
    case Profile::Mercury: return QStringLiteral("mercury");
    case Profile::Data2gHost: return QStringLiteral("data2g-host");
    }
    return QStringLiteral("vara-compatible");
}

Profile profileFromString(const QString& text)
{
    if (text == QLatin1String("mercury")) {
        return Profile::Mercury;
    }
    if (text == QLatin1String("data2g-host")) {
        return Profile::Data2gHost;
    }
    return Profile::VaraCompatible;
}

bool isValidPort(int port)
{
    return port >= 1024 && port <= 65535;
}

bool isValidEngineId(const QString& id)
{
    static const QRegularExpression re(QStringLiteral("^[a-z0-9-]{1,32}$"));
    return re.match(id).hasMatch();
}

bool isValidHost(const QString& host)
{
    if (host.isEmpty() || host.size() > 253) {
        return false;
    }
    for (const QChar c : host) {
        if (c.isSpace() || !c.isPrint()) {
            return false;
        }
    }
    return true;
}

bool isLoopbackHost(const QString& host)
{
    if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    const QHostAddress a(host);
    return !a.isNull() && a.isLoopback();
}

QJsonObject toJson(const Config& config)
{
    QJsonArray engines;
    for (const EngineConfig& e : config.engines) {
        QJsonObject o;
        o.insert(kId, e.id);
        o.insert(kProfile, toString(e.profile));
        o.insert(kLabel, e.label);
        o.insert(kHost, e.host);
        o.insert(kCommandPort, e.commandPort);
        o.insert(kDataPort, e.dataPort);
        engines.append(o);
    }
    QJsonObject backend;
    backend.insert(kCommandPort, config.backend.commandPort);
    backend.insert(kDataPort, config.backend.dataPort);
    backend.insert(kLoopbackOnly, config.backend.loopbackOnly);

    QJsonObject root;
    root.insert(kVersion, Config::kSchemaVersion);
    root.insert(kEngines, engines);
    root.insert(kBackend, backend);
    return root;
}

Config fromJson(const QJsonObject& json)
{
    Config config;
    const BackendConfig defaults;

    QSet<QString> seen;
    for (const QJsonValue& v : json.value(kEngines).toArray()) {
        if (config.engines.size() >= Config::kMaxEngines) {
            break;
        }
        const QJsonObject o = v.toObject();
        EngineConfig e;
        e.id = o.value(kId).toString();
        if (!isValidEngineId(e.id) || seen.contains(e.id)) {
            continue;
        }
        seen.insert(e.id);
        e.profile = profileFromString(o.value(kProfile).toString());
        e.label = cleanLabel(o.value(kLabel).toString());
        const QString host = o.value(kHost).toString();
        if (isValidHost(host)) {
            e.host = host;
        }
        e.commandPort = portOr(o, kCommandPort, e.commandPort);
        const quint16 defaultData = e.commandPort < 65535 ? static_cast<quint16>(e.commandPort + 1) : e.dataPort;
        e.dataPort = portOr(o, kDataPort, defaultData);
        if (e.dataPort == e.commandPort) {
            e.dataPort = defaultData;
        }
        config.engines.append(e);
    }

    const QJsonObject b = json.value(kBackend).toObject();
    config.backend.commandPort = portOr(b, kCommandPort, defaults.commandPort);
    config.backend.dataPort = portOr(b, kDataPort, defaults.dataPort);
    if (config.backend.dataPort == config.backend.commandPort) {
        config.backend = defaults;
    }
    const QJsonValue lb = b.value(kLoopbackOnly);
    config.backend.loopbackOnly = lb.isBool() ? lb.toBool() : defaults.loopbackOnly;
    return config;
}

}  // namespace AetherSDR::HfGateway
