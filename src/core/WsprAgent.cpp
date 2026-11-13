#include "WsprAgent.h"

#include "DxSpot.h"
#include "MaidenheadLocator.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QRegularExpression>
#include <QSaveFile>

#include <cmath>

namespace AetherSDR {

bool WsprConfig::operator==(const WsprConfig& o) const
{
    for (int p = 0; p < PeriodCount; ++p) {
        if (hop[p] != o.hop[p]) {
            return false;
        }
    }
    return callsign == o.callsign && grid == o.grid && band == o.band && hopping == o.hopping
        && grayMinutes == o.grayMinutes && depth == o.depth && rxant == o.rxant
        && upload == o.upload && daxChannel == o.daxChannel;
}

namespace wspr {

namespace {

// Bounds for what the radio sends; anything outside them is not a WSPR
// decode and is dropped (Principle VII).
constexpr int kMaxSpots = 5000;
constexpr int kMaxCycles = 200;
constexpr qint64 kMinHz = 100000;
constexpr qint64 kMaxHz = 500000000;

bool validCall(const QString& s)
{
    static const QRegularExpression re(QStringLiteral("^[A-Z0-9/.-]{0,15}$"));
    return re.match(s).hasMatch();
}

bool validGrid(const QString& s)
{
    static const QRegularExpression re(QStringLiteral("^([A-Ra-r]{2}[0-9]{2}([A-Xa-x]{2})?)?$"));
    return re.match(s).hasMatch();
}

bool validBand(const QString& s) { return s.isEmpty() || bands().contains(s); }

QString boundedString(const QJsonValue& v, int max)
{
    const QString s = v.toString();
    return s.size() <= max ? s : QString();
}

bool inRange(const QJsonValue& v, double lo, double hi)
{
    return v.isDouble() && v.toDouble() >= lo && v.toDouble() <= hi;
}

std::optional<WsprSpot> parseSpot(const QJsonObject& o)
{
    if (!inRange(o.value(QLatin1String("t")), 0, 1e11) || !inRange(o.value(QLatin1String("snr")), -60, 60)
        || !inRange(o.value(QLatin1String("dt")), -20, 20)
        || !inRange(o.value(QLatin1String("hz")), kMinHz, kMaxHz)
        || !inRange(o.value(QLatin1String("drift")), -20, 20)
        || !inRange(o.value(QLatin1String("dbm")), -30, 70)) {
        return std::nullopt;
    }
    WsprSpot s;
    s.cycleStart = o.value(QLatin1String("t")).toInteger();
    s.band = o.value(QLatin1String("band")).toString();
    s.snr = o.value(QLatin1String("snr")).toInt();
    s.dt = o.value(QLatin1String("dt")).toDouble();
    s.frequencyHz = o.value(QLatin1String("hz")).toInteger();
    s.drift = o.value(QLatin1String("drift")).toInt();
    s.call = o.value(QLatin1String("call")).toString();
    s.grid = o.value(QLatin1String("grid")).toString();
    s.dbm = o.value(QLatin1String("dbm")).toInt();
    if (!validBand(s.band) || !validCall(s.call) || !validGrid(s.grid)) {
        return std::nullopt;
    }
    return s;
}

std::optional<WsprCycle> parseCycle(const QJsonObject& o)
{
    if (!inRange(o.value(QLatin1String("t")), 0, 1e11)
        || !inRange(o.value(QLatin1String("dial_hz")), kMinHz, kMaxHz)
        || !inRange(o.value(QLatin1String("spots")), 0, 1000)
        || !inRange(o.value(QLatin1String("decode_ms")), 0, 600000)
        || !inRange(o.value(QLatin1String("audio_pct")), 0, 100)) {
        return std::nullopt;
    }
    WsprCycle c;
    c.start = o.value(QLatin1String("t")).toInteger();
    c.band = o.value(QLatin1String("band")).toString();
    c.dialHz = o.value(QLatin1String("dial_hz")).toInteger();
    c.spots = o.value(QLatin1String("spots")).toInt();
    c.decodeMs = o.value(QLatin1String("decode_ms")).toInt();
    c.audioPct = o.value(QLatin1String("audio_pct")).toInt();
    const QJsonValue lost = o.value(QLatin1String("lost_packets"));
    c.lostPackets = inRange(lost, 0, 1e7) ? lost.toInt() : 0;
    const QJsonValue dbfs = o.value(QLatin1String("audio_dbfs"));
    c.audioDbfs = inRange(dbfs, -250, 10) ? dbfs.toDouble() : 0.0;
    if (!validBand(c.band)) {
        return std::nullopt;
    }
    return c;
}

std::optional<QJsonObject> object(const QByteArray& json)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return std::nullopt;
    }
    return doc.object();
}

bool matchesPin(const QByteArray& bytes, const WsprPackage::Pin& pin)
{
    return bytes.size() == pin.size
        && QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() == QByteArray(pin.sha256);
}

}  // namespace

const QStringList& bands()
{
    static const QStringList list{
        QStringLiteral("2200m"), QStringLiteral("630m"), QStringLiteral("160m"), QStringLiteral("80m"),
        QStringLiteral("60m"),   QStringLiteral("40m"),  QStringLiteral("30m"),  QStringLiteral("20m"),
        QStringLiteral("17m"),   QStringLiteral("15m"),  QStringLiteral("12m"),  QStringLiteral("10m"),
        QStringLiteral("6m"),    QStringLiteral("2m")};
    return list;
}

const QStringList& coordinatedBands()
{
    static const QStringList list{
        QStringLiteral("160m"), QStringLiteral("80m"), QStringLiteral("60m"), QStringLiteral("40m"),
        QStringLiteral("30m"),  QStringLiteral("20m"), QStringLiteral("17m"), QStringLiteral("15m"),
        QStringLiteral("12m"),  QStringLiteral("10m")};
    return list;
}

const QStringList& depths()
{
    static const QStringList list{QStringLiteral("standard"), QStringLiteral("fast"),
                                  QStringLiteral("normal"), QStringLiteral("deep")};
    return list;
}

QString periodKey(WsprConfig::Period period)
{
    switch (period) {
    case WsprConfig::Sunrise: return QStringLiteral("hop_sunrise");
    case WsprConfig::Day: return QStringLiteral("hop_day");
    case WsprConfig::Sunset: return QStringLiteral("hop_sunset");
    case WsprConfig::Night: return QStringLiteral("hop_night");
    case WsprConfig::PeriodCount: break;
    }
    return {};
}

std::optional<WsprStatus> parseStatus(const QByteArray& json)
{
    const auto root = object(json);
    if (!root || root->value(QLatin1String("agent")).toString() != QLatin1String("AetherWSPR")) {
        return std::nullopt;
    }
    const QJsonObject& o = *root;
    WsprStatus st;
    st.version = boundedString(o.value(QLatin1String("version")), 32);
    st.state = boundedString(o.value(QLatin1String("state")), 32);
    st.clockSynced = o.value(QLatin1String("clock_synced")).toBool();
    st.uptimeS = inRange(o.value(QLatin1String("uptime_s")), 0, 1e10)
        ? o.value(QLatin1String("uptime_s")).toInteger() : 0;
    // The station's own call and grid come from the radio, whatever it was
    // told: one it can't show mustn't cost every decode in the reply.
    st.callsign = o.value(QLatin1String("callsign")).toString().toUpper();
    if (!validCall(st.callsign)) {
        st.callsign.clear();
    }
    st.grid = o.value(QLatin1String("grid")).toString();
    if (!validGrid(st.grid)) {
        st.grid.clear();
    }
    st.band = o.value(QLatin1String("band")).toString();
    st.dialHz = inRange(o.value(QLatin1String("dial_hz")), 0, kMaxHz)
        ? o.value(QLatin1String("dial_hz")).toInteger() : 0;
    st.hopPeriod = boundedString(o.value(QLatin1String("hop_period")), 16);
    st.depth = o.value(QLatin1String("depth")).toString();
    st.slice = o.value(QLatin1String("slice")).toBool();
    st.sliceIndex = st.slice && inRange(o.value(QLatin1String("slice_index")), 0, 63)
        ? o.value(QLatin1String("slice_index")).toInt(-1) : -1;
    st.audio = o.value(QLatin1String("audio")).toBool();
    if (!validBand(st.band)
        || (!st.depth.isEmpty() && !depths().contains(st.depth))) {
        return std::nullopt;
    }
    const QJsonObject up = o.value(QLatin1String("upload")).toObject();
    st.upload.enabled = up.value(QLatin1String("enabled")).toBool();
    st.upload.sent = inRange(up.value(QLatin1String("sent")), 0, 1e12) ? up.value(QLatin1String("sent")).toInteger() : 0;
    st.upload.rejected = inRange(up.value(QLatin1String("rejected")), 0, 1e12)
        ? up.value(QLatin1String("rejected")).toInteger() : 0;
    st.upload.queued = inRange(up.value(QLatin1String("queued")), 0, 1e7) ? up.value(QLatin1String("queued")).toInt() : 0;
    st.upload.lastError = boundedString(up.value(QLatin1String("last_error")), 120);

    const QJsonArray cycles = o.value(QLatin1String("cycles")).toArray();
    const QJsonArray spots = o.value(QLatin1String("spots")).toArray();
    if (cycles.size() > kMaxCycles || spots.size() > kMaxSpots) {
        return std::nullopt;
    }
    for (const QJsonValue& v : cycles) {
        if (auto c = parseCycle(v.toObject())) {
            st.cycles.append(*c);
        }
    }
    for (const QJsonValue& v : spots) {
        if (auto s = parseSpot(v.toObject())) {
            st.spots.append(*s);
        }
    }
    return st;
}

std::optional<WsprConfig> parseConfig(const QByteArray& json, bool* claimed)
{
    const auto root = object(json);
    if (!root || !root->value(QLatin1String("config")).isObject()) {
        return std::nullopt;
    }
    if (claimed) {
        *claimed = root->value(QLatin1String("claimed")).toBool();
    }
    const QJsonObject o = root->value(QLatin1String("config")).toObject();
    WsprConfig c;
    c.callsign = o.value(QLatin1String("callsign")).toString();
    c.grid = o.value(QLatin1String("grid")).toString();
    c.band = o.value(QLatin1String("band")).toString();
    c.hopping = o.value(QLatin1String("hopping")).toBool();
    c.depth = o.value(QLatin1String("depth")).toString();
    c.rxant = o.value(QLatin1String("rxant")).toString();
    c.upload = o.value(QLatin1String("upload")).toBool();
    if (!inRange(o.value(QLatin1String("gray_minutes")), 1, 120)
        || !inRange(o.value(QLatin1String("dax_channel")), 1, 8)) {
        return std::nullopt;
    }
    c.grayMinutes = o.value(QLatin1String("gray_minutes")).toInt();
    c.daxChannel = o.value(QLatin1String("dax_channel")).toInt();
    static const QRegularExpression ant(QStringLiteral("^[A-Za-z0-9_]{0,11}$"));
    if (!validCall(c.callsign) || !validGrid(c.grid) || !bands().contains(c.band)
        || !depths().contains(c.depth) || !ant.match(c.rxant).hasMatch()) {
        return std::nullopt;
    }
    for (int p = 0; p < WsprConfig::PeriodCount; ++p) {
        const QJsonArray list = o.value(periodKey(static_cast<WsprConfig::Period>(p))).toArray();
        for (const QJsonValue& v : list) {
            const QString b = v.toString();
            if (!bands().contains(b)) {
                return std::nullopt;
            }
            if (!c.hop[p].contains(b)) {
                c.hop[p].append(b);
            }
        }
    }
    return c;
}

QString adminToken(const QByteArray& json)
{
    const auto root = object(json);
    if (!root) {
        return {};
    }
    static const QRegularExpression re(QStringLiteral("^[0-9a-f]{32}$"));
    const QString t = root->value(QLatin1String("admin_token")).toString();
    return re.match(t).hasMatch() ? t : QString();
}

QByteArray configJson(const WsprConfig& c)
{
    QJsonObject o;
    o.insert(QStringLiteral("callsign"), c.callsign);
    o.insert(QStringLiteral("grid"), c.grid);
    o.insert(QStringLiteral("band"), c.band);
    o.insert(QStringLiteral("hopping"), c.hopping);
    o.insert(QStringLiteral("gray_minutes"), c.grayMinutes);
    for (int p = 0; p < WsprConfig::PeriodCount; ++p) {
        QJsonArray list;
        // In band order, whatever order they were ticked in.
        for (const QString& b : bands()) {
            if (c.hop[p].contains(b)) {
                list.append(b);
            }
        }
        o.insert(periodKey(static_cast<WsprConfig::Period>(p)), list);
    }
    o.insert(QStringLiteral("depth"), c.depth);
    o.insert(QStringLiteral("rxant"), c.rxant);
    o.insert(QStringLiteral("upload"), c.upload);
    o.insert(QStringLiteral("dax_channel"), c.daxChannel);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

DxSpot toDxSpot(const WsprSpot& spot, const QString& receiver)
{
    DxSpot d;
    d.spotterCall = receiver;
    d.freqMhz = static_cast<double>(spot.frequencyHz) / 1e6;
    d.dxCall = spot.call;
    d.comment = QStringLiteral("WSPR %1 dB %2 dBm").arg(spot.snr).arg(spot.dbm);
    d.utcTime = QDateTime::fromSecsSinceEpoch(spot.cycleStart, QTimeZone::UTC).time();
    d.source = QStringLiteral("WSPR");
    d.snr = spot.snr;
    d.hasSnr = true;
    d.dxGrid = spot.grid.left(4);
    return d;
}

QList<WsprSpot> spotsSince(const QList<WsprSpot>& spots, qint64 since, qint64* next)
{
    QList<WsprSpot> fresh;
    qint64 newest = since - 1;
    for (const WsprSpot& s : spots) {
        if (s.cycleStart >= since) {
            fresh.append(s);
            newest = qMax(newest, s.cycleStart);
        }
    }
    if (next) {
        *next = newest + 1;
    }
    return fresh;
}

WsprSpotHubSettings parseSpotHubSettings(const QString& json)
{
    WsprSpotHubSettings s;
    const auto o = object(json.toUtf8());
    if (!o) {
        return s;
    }
    s.autoStart = o->value(QLatin1String("autoStart")).toBool(s.autoStart);
    static const QRegularExpression hex(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
    const QString color = o->value(QLatin1String("color")).toString();
    if (hex.match(color).hasMatch()) {
        s.color = color;
    }
    if (inRange(o->value(QLatin1String("lifetimeMinutes")), 1, 1440)) {
        s.lifetimeMinutes = o->value(QLatin1String("lifetimeMinutes")).toInt();
    }
    return s;
}

QString serializeSpotHubSettings(const WsprSpotHubSettings& s)
{
    QJsonObject o;
    o.insert(QStringLiteral("autoStart"), s.autoStart);
    o.insert(QStringLiteral("color"), s.color);
    o.insert(QStringLiteral("lifetimeMinutes"), s.lifetimeMinutes);
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

int distanceKm(const QString& fromGrid, const QString& toGrid)
{
    double km = 0.0;
    double bearing = 0.0;
    if (fromGrid.size() < 4 || toGrid.size() < 4
        || !MaidenheadLocator::gridDistance(fromGrid, toGrid, km, bearing)) {
        return -1;
    }
    return static_cast<int>(std::lround(km));
}

bool reachableRadio(bool overWan, const QHostAddress& address)
{
    return !overWan && reachableAddress(address);
}

bool reachableAddress(const QHostAddress& address)
{
    if (address.isNull() || address.protocol() != QAbstractSocket::IPv4Protocol) {
        return false;
    }
    static const QPair<QHostAddress, int> tailnet = QHostAddress::parseSubnet(QStringLiteral("100.64.0.0/10"));
    return address.isPrivateUse() || address.isLinkLocal() || address.isInSubnet(tailnet);
}

QString writeBundledPackage(const WsprPackage::Pin& pin, const QString& dir, QString* error)
{
    const QString target = QDir(dir).filePath(QString::fromLatin1(pin.fileName));
    QFile existing(target);
    if (existing.open(QIODevice::ReadOnly) && existing.size() == pin.size && matchesPin(existing.readAll(), pin)) {
        return target;
    }
    existing.close();
    QFile bundled(QString::fromLatin1(pin.resource));
    const QByteArray bytes = bundled.open(QIODevice::ReadOnly) ? bundled.read(pin.size + 1) : QByteArray();
    if (!matchesPin(bytes, pin)) {
        if (error) {
            *error = QObject::tr("This AetherSDR's WSPR receiver is damaged, so nothing was installed.");
        }
        return {};
    }
    QSaveFile out(target);
    if (!QDir().mkpath(dir) || !out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size()
        || !out.commit()) {
        if (error) {
            *error = QObject::tr("Can't write the WSPR receiver to %1.").arg(QDir::toNativeSeparators(target));
        }
        return {};
    }
    return target;
}

bool updateAvailable(const QString& installed)
{
    static const QRegularExpression re(QStringLiteral("^(\\d{1,6})\\.(\\d{1,6})\\.(\\d{1,6})"));
    const QRegularExpressionMatch have = re.match(installed);
    const QRegularExpressionMatch bundled = re.match(QString::fromLatin1(WsprPackage::kVersion));
    if (!have.hasMatch() || !bundled.hasMatch()) {
        return false;
    }
    for (int i = 1; i <= 3; ++i) {
        const int a = have.captured(i).toInt();
        const int b = bundled.captured(i).toInt();
        if (a != b) {
            return a < b;
        }
    }
    return false;
}

}  // namespace wspr
}  // namespace AetherSDR
