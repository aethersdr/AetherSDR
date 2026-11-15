#include "RfkitProtocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

#include <cmath>
#include <limits>

namespace AetherSDR {
namespace Rfkit {

namespace {

std::optional<QJsonObject> parseObject(const QByteArray& body)
{
    if (body.isEmpty() || body.size() > kMaxBodyBytes) {
        return std::nullopt;
    }
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return std::nullopt;
    }
    return doc.object();
}

bool asInt(const QJsonValue& v, int& out)
{
    if (!v.isDouble()) {
        return false;
    }
    const double d = v.toDouble();
    if (!std::isfinite(d) || d < static_cast<double>(std::numeric_limits<int>::min())
        || d > static_cast<double>(std::numeric_limits<int>::max())) {
        return false;
    }
    out = static_cast<int>(std::lround(d));
    return true;
}

bool asFloat(const QJsonValue& v, float& out)
{
    if (!v.isDouble()) {
        return false;
    }
    const double d = v.toDouble();
    // A float that overflows to infinity would drive a gauge to nonsense.
    if (!std::isfinite(d) || std::fabs(d) > 1.0e9) {
        return false;
    }
    out = static_cast<float>(d);
    return true;
}

QString stringOr(const QJsonValue& v, const QString& fallback = QString())
{
    return v.isString() ? v.toString() : fallback;
}

// {"value": <int>, "unit": "..."}. The value is required; the unit is
// descriptive and defaults to empty.
std::optional<IntValue> parseIntValue(const QJsonValue& v)
{
    if (!v.isObject()) {
        return std::nullopt;
    }
    const QJsonObject o = v.toObject();
    IntValue out;
    if (!asInt(o.value(QLatin1String("value")), out.value)) {
        return std::nullopt;
    }
    out.unit = stringOr(o.value(QLatin1String("unit")));
    return out;
}

std::optional<FloatValue> parseFloatValue(const QJsonValue& v)
{
    if (!v.isObject()) {
        return std::nullopt;
    }
    const QJsonObject o = v.toObject();
    FloatValue out;
    if (!asFloat(o.value(QLatin1String("value")), out.value)) {
        return std::nullopt;
    }
    out.unit = stringOr(o.value(QLatin1String("unit")));
    return out;
}

// max_value is the device's reported limit (not a meter range); absent or
// unusable means 0, "not reported". The applet uses fixed scales and ignores it.
std::optional<IntWithMax> parseIntWithMax(const QJsonValue& v)
{
    if (!v.isObject()) {
        return std::nullopt;
    }
    const QJsonObject o = v.toObject();
    IntWithMax out;
    if (!asInt(o.value(QLatin1String("value")), out.value)) {
        return std::nullopt;
    }
    int max = 0;
    if (asInt(o.value(QLatin1String("max_value")), max) && max > 0) {
        out.maxValue = max;
    }
    out.unit = stringOr(o.value(QLatin1String("unit")));
    return out;
}

std::optional<FloatWithMax> parseFloatWithMax(const QJsonValue& v)
{
    if (!v.isObject()) {
        return std::nullopt;
    }
    const QJsonObject o = v.toObject();
    FloatWithMax out;
    if (!asFloat(o.value(QLatin1String("value")), out.value)) {
        return std::nullopt;
    }
    float max = 0.0f;
    if (asFloat(o.value(QLatin1String("max_value")), max) && max > 0.0f) {
        out.maxValue = max;
    }
    out.unit = stringOr(o.value(QLatin1String("unit")));
    return out;
}

std::optional<Antenna> antennaFromObject(const QJsonObject& o)
{
    const QString type = o.value(QLatin1String("type")).toString();
    Antenna antenna;
    if (type == QLatin1String("EXTERNAL")) {
        // One external selection. Any number on the entry is the external
        // switch's port (readOnly in the API document), not part of what is
        // selected; parseAntennas() keeps it separately.
        antenna.type = AntennaType::External;
        antenna.number = kExternalAntennaNumber;
        return antenna;
    }
    int number = 0;
    if (type != QLatin1String("INTERNAL") || !asInt(o.value(QLatin1String("number")), number)
        || number < kMinInternalAntenna || number > kMaxInternalAntenna) {
        return std::nullopt;
    }
    antenna.type = AntennaType::Internal;
    antenna.number = number;
    return antenna;
}

bool antennaInRange(const Antenna& a)
{
    if (a.type == AntennaType::Internal) {
        return a.number >= kMinInternalAntenna && a.number <= kMaxInternalAntenna;
    }
    return a.number == kExternalAntennaNumber;
}

TunerMode tunerModeFromValue(const QJsonValue& v)
{
    // The API document's enum lists the literal `false` beside the names (a
    // generator artifact for a YAML "off"); accept it as a boolean or a string.
    if (v.isBool()) {
        return v.toBool() ? TunerMode::Unknown : TunerMode::Off;
    }
    const QString s = v.toString();
    if (s == QLatin1String("BYPASS"))      { return TunerMode::Bypass; }
    if (s == QLatin1String("MANUAL"))      { return TunerMode::Manual; }
    if (s == QLatin1String("AUTO_TUNING")) { return TunerMode::AutoTuning; }
    if (s == QLatin1String("AUTO"))        { return TunerMode::Auto; }
    if (s == QLatin1String("false"))       { return TunerMode::Off; }
    return TunerMode::Unknown;
}

TunerSetup tunerSetupFromString(const QString& s)
{
    if (s == QLatin1String("NOT TUNED")) { return TunerSetup::NotTuned; }
    if (s == QLatin1String("BYPASS"))    { return TunerSetup::Bypass; }
    if (s == QLatin1String("CL"))        { return TunerSetup::CL; }
    if (s == QLatin1String("LC"))        { return TunerSetup::LC; }
    return TunerSetup::Unknown;
}

}  // namespace

// ── Parsers ──────────────────────────────────────────────────────────────

std::optional<Info> parseInfo(const QByteArray& body)
{
    const auto obj = parseObject(body);
    if (!obj) {
        return std::nullopt;
    }
    const QJsonValue device = obj->value(QLatin1String("device"));
    if (!device.isString()) {
        return std::nullopt;
    }
    Info info;
    info.device = device.toString();
    info.customName = stringOr(obj->value(QLatin1String("custom_device_name")));
    const QJsonValue sw = obj->value(QLatin1String("software_version"));
    if (sw.isObject()) {
        const QJsonObject s = sw.toObject();
        asInt(s.value(QLatin1String("GUI")), info.guiVersion);
        asInt(s.value(QLatin1String("controller")), info.controllerVersion);
    }
    return info;
}

std::optional<Power> parsePower(const QByteArray& body)
{
    const auto obj = parseObject(body);
    if (!obj) {
        return std::nullopt;
    }
    const auto temperature = parseFloatValue(obj->value(QLatin1String("temperature")));
    const auto voltage = parseFloatValue(obj->value(QLatin1String("voltage")));
    const auto current = parseFloatValue(obj->value(QLatin1String("current")));
    const auto forward = parseIntWithMax(obj->value(QLatin1String("forward")));
    const auto reflected = parseIntWithMax(obj->value(QLatin1String("reflected")));
    const auto swr = parseFloatWithMax(obj->value(QLatin1String("swr")));
    if (!temperature || !voltage || !current || !forward || !reflected || !swr) {
        return std::nullopt;
    }
    Power power;
    power.temperature = *temperature;
    power.voltage = *voltage;
    power.current = *current;
    power.forward = *forward;
    power.reflected = *reflected;
    power.swr = *swr;
    return power;
}

std::optional<Data> parseData(const QByteArray& body)
{
    const auto obj = parseObject(body);
    if (!obj) {
        return std::nullopt;
    }
    const auto band = parseIntValue(obj->value(QLatin1String("band")));
    const auto frequency = parseIntValue(obj->value(QLatin1String("frequency")));
    if (!band || !frequency) {
        return std::nullopt;
    }
    Data data;
    data.band = *band;
    data.frequency = *frequency;
    data.status = stringOr(obj->value(QLatin1String("status")));
    return data;
}

std::optional<Tuner> parseTuner(const QByteArray& body)
{
    const auto obj = parseObject(body);
    if (!obj || !obj->contains(QLatin1String("mode"))) {
        return std::nullopt;
    }
    Tuner tuner;
    tuner.mode = tunerModeFromValue(obj->value(QLatin1String("mode")));
    tuner.setup = tunerSetupFromString(stringOr(obj->value(QLatin1String("setup"))));
    tuner.l = parseIntValue(obj->value(QLatin1String("L")));
    tuner.c = parseIntValue(obj->value(QLatin1String("C")));
    tuner.tunedFrequency = parseIntValue(obj->value(QLatin1String("tuned_frequency")));
    tuner.segmentSize = parseIntValue(obj->value(QLatin1String("segment_size")));
    return tuner;
}

std::optional<OperateMode> parseOperateMode(const QByteArray& body)
{
    const auto obj = parseObject(body);
    if (!obj) {
        return std::nullopt;
    }
    const QString mode = obj->value(QLatin1String("operate_mode")).toString();
    if (mode == QLatin1String("OPERATE")) {
        return OperateMode::Operate;
    }
    if (mode == QLatin1String("STANDBY")) {
        return OperateMode::Standby;
    }
    return std::nullopt;
}

std::optional<InterfaceState> parseInterface(const QByteArray& body)
{
    const auto obj = parseObject(body);
    if (!obj) {
        return std::nullopt;
    }
    const QString name = obj->value(QLatin1String("operational_interface")).toString();
    InterfaceState state;
    if (name == QLatin1String("UNIV")) {
        state.active = OperationalInterface::Univ;
    } else if (name == QLatin1String("CAT")) {
        state.active = OperationalInterface::Cat;
    } else if (name == QLatin1String("UDP")) {
        state.active = OperationalInterface::Udp;
    } else if (name == QLatin1String("TCI")) {
        state.active = OperationalInterface::Tci;
    } else {
        return std::nullopt;
    }
    state.error = stringOr(obj->value(QLatin1String("error")));
    return state;
}

std::optional<Antenna> parseAntenna(const QByteArray& body)
{
    const auto obj = parseObject(body);
    if (!obj) {
        return std::nullopt;
    }
    return antennaFromObject(*obj);
}

std::optional<QVector<AntennaWithState>> parseAntennas(const QByteArray& body)
{
    if (body.isEmpty() || body.size() > kMaxBodyBytes) {
        return std::nullopt;
    }
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError) {
        return std::nullopt;
    }
    // The document describes a bare array, but a real RF2K-S (firmware G200C267)
    // answers {"antennas":[...]}. Accept both: the list is the same either way.
    QJsonArray entries;
    if (doc.isArray()) {
        entries = doc.array();
    } else if (doc.isObject() && doc.object().value(QLatin1String("antennas")).isArray()) {
        entries = doc.object().value(QLatin1String("antennas")).toArray();
    } else {
        return std::nullopt;
    }
    QVector<AntennaWithState> out;
    for (const QJsonValue& entry : entries) {
        if (!entry.isObject()) {
            continue;
        }
        const QJsonObject o = entry.toObject();
        const auto antenna = antennaFromObject(o);
        if (!antenna) {
            continue;
        }
        const QString state = o.value(QLatin1String("state")).toString();
        AntennaWithState item;
        item.antenna = *antenna;
        if (antenna->type == AntennaType::External) {
            int port = 0;
            if (asInt(o.value(QLatin1String("number")), port)
                && port >= kMinExternalPort && port <= kMaxExternalPort) {
                item.externalPort = port;
            }
        }
        if (state == QLatin1String("ACTIVE")) {
            item.state = AntennaState::Active;
        } else if (state == QLatin1String("AVAILABLE")) {
            item.state = AntennaState::Available;
        } else if (state == QLatin1String("DISABLED")) {
            item.state = AntennaState::Disabled;
        } else {
            continue;
        }
        out.append(item);
    }
    return out;
}

std::optional<ApiError> parseApiError(const QByteArray& body)
{
    const auto obj = parseObject(body);
    if (!obj) {
        return std::nullopt;
    }
    ApiError error;
    error.status = stringOr(obj->value(QLatin1String("status")));
    error.reason = stringOr(obj->value(QLatin1String("reason")));
    return error;
}

// ── Request bodies ───────────────────────────────────────────────────────

QByteArray buildOperateModeBody(OperateMode mode)
{
    QJsonObject o;
    o.insert(QLatin1String("operate_mode"),
             mode == OperateMode::Operate ? QStringLiteral("OPERATE") : QStringLiteral("STANDBY"));
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QByteArray buildAntennaBody(const Antenna& antenna)
{
    if (!antennaInRange(antenna)) {
        return {};
    }
    QJsonObject o;
    o.insert(QLatin1String("type"),
             antenna.type == AntennaType::Internal ? QStringLiteral("INTERNAL")
                                                   : QStringLiteral("EXTERNAL"));
    if (antenna.type == AntennaType::Internal) {
        o.insert(QLatin1String("number"), antenna.number);
    }
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

// ── Presentation helpers ─────────────────────────────────────────────────

float temperatureCelsius(const FloatValue& temperature)
{
    // "°F", "F" and "degF" all contain an F and never a C; "°C" is the
    // expected case and passes through unchanged.
    const QString unit = temperature.unit.toUpper();
    if (unit.contains(QLatin1Char('F')) && !unit.contains(QLatin1Char('C'))) {
        return (temperature.value - 32.0f) * 5.0f / 9.0f;
    }
    return temperature.value;
}

QString formatFrequency(const IntValue& frequency)
{
    const QString unit = frequency.unit.trimmed().toLower();
    double mhz = 0.0;
    if (unit == QLatin1String("hz")) {
        mhz = frequency.value / 1.0e6;
    } else if (unit == QLatin1String("khz")) {
        mhz = frequency.value / 1.0e3;
    } else if (unit == QLatin1String("mhz")) {
        mhz = frequency.value;
    } else {
        // An unrecognized unit is shown as given rather than mis-scaled.
        return frequency.unit.isEmpty()
            ? QString::number(frequency.value)
            : QStringLiteral("%1 %2").arg(frequency.value).arg(frequency.unit);
    }
    return QStringLiteral("%1 MHz").arg(mhz, 0, 'f', 3);
}

QString formatBand(const IntValue& band)
{
    return band.unit.isEmpty()
        ? QString::number(band.value)
        : QStringLiteral("%1 %2").arg(band.value).arg(band.unit);
}

QString antennaLabel(const Antenna& antenna)
{
    return antenna.type == AntennaType::Internal
        ? QString::number(antenna.number)
        : QStringLiteral("EXT");
}

QString tunerModeLabel(TunerMode mode)
{
    switch (mode) {
    case TunerMode::Bypass:     return QStringLiteral("BYPASS");
    case TunerMode::Manual:     return QStringLiteral("MANUAL");
    case TunerMode::AutoTuning: return QStringLiteral("TUNING");
    case TunerMode::Auto:       return QStringLiteral("AUTO");
    case TunerMode::Off:        return QStringLiteral("OFF");
    case TunerMode::Unknown:    break;
    }
    return QStringLiteral("—");
}

QString tunerSetupLabel(TunerSetup setup)
{
    switch (setup) {
    case TunerSetup::NotTuned: return QStringLiteral("NOT TUNED");
    case TunerSetup::Bypass:   return QStringLiteral("BYPASS");
    case TunerSetup::CL:       return QStringLiteral("CL");
    case TunerSetup::LC:       return QStringLiteral("LC");
    case TunerSetup::Unknown:  break;
    }
    return QStringLiteral("—");
}

QString interfaceLabel(OperationalInterface iface)
{
    switch (iface) {
    case OperationalInterface::Univ: return QStringLiteral("UNIV");
    case OperationalInterface::Cat:  return QStringLiteral("CAT");
    case OperationalInterface::Udp:  return QStringLiteral("UDP");
    case OperationalInterface::Tci:  return QStringLiteral("TCI");
    }
    return QStringLiteral("—");
}

}  // namespace Rfkit
}  // namespace AetherSDR
