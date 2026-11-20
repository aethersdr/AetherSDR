#include "Ft8Agent.h"

#include "DxSpot.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTimeZone>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace AetherSDR {
namespace ft8 {

namespace {

// Bounds for what the agent sends; anything outside them is not an FT8
// decode and is dropped (Principle VII). A 15-second cycle produces far more
// cycles per hour than WSPR's two-minute one, so the cycle bound is higher
// even though the agent itself keeps 240.
constexpr int kMaxSpots = 5000;
constexpr int kMaxCycles = 500;
constexpr qint64 kMinHz = 100000;
constexpr qint64 kMaxHz = 500000000;
constexpr int kMaxStringChars = 64;

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

std::optional<QJsonObject> object(const QByteArray& json)
{
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return std::nullopt;
    }
    return doc.object();
}

bool inRange(const QJsonValue& v, double lo, double hi)
{
    if (!v.isDouble()) {
        return false;
    }
    const double d = v.toDouble();
    return d >= lo && d <= hi;
}

QString boundedString(const QJsonValue& v, int max)
{
    const QString s = v.toString();
    return s.size() > max ? s.left(max) : s;
}

}  // namespace

const QStringList& bands()
{
    // ⚠ The standard FT8 dial frequencies, which are NOT WSPR's. A band name
    // therefore means a different frequency in the two agents: 20 m is
    // 14.074 here and 14.0956 in AetherWSPR.
    static const QStringList list{
        QStringLiteral("2200m"), QStringLiteral("630m"), QStringLiteral("160m"),
        QStringLiteral("80m"),   QStringLiteral("60m"),  QStringLiteral("40m"),
        QStringLiteral("30m"),   QStringLiteral("20m"),  QStringLiteral("17m"),
        QStringLiteral("15m"),   QStringLiteral("12m"),  QStringLiteral("10m"),
        QStringLiteral("6m"),    QStringLiteral("2m"),
    };
    return list;
}

std::optional<Ft8Status> parseStatus(const QByteArray& json)
{
    const auto root = object(json);
    if (!root) {
        return std::nullopt;
    }
    // Refuse another agent's status outright rather than reading fields out
    // of it: AetherWSPR answers the same shape on 48995.
    if (root->value(QLatin1String("agent")).toString() != QLatin1String(kFt8WaveformName)) {
        return std::nullopt;
    }

    Ft8Status s;
    s.version = boundedString(root->value(QLatin1String("version")), kMaxStringChars);
    s.state = boundedString(root->value(QLatin1String("state")), kMaxStringChars);
    s.clockSynced = root->value(QLatin1String("clock_synced")).toBool();
    s.clockSource = boundedString(root->value(QLatin1String("clock_source")), kMaxStringChars);
    s.uptimeS = static_cast<qint64>(root->value(QLatin1String("uptime_s")).toDouble());
    s.callsign = boundedString(root->value(QLatin1String("callsign")), kMaxStringChars);
    s.grid = boundedString(root->value(QLatin1String("grid")), kMaxStringChars);
    s.band = boundedString(root->value(QLatin1String("band")), kMaxStringChars);
    s.dialHz = static_cast<qint64>(root->value(QLatin1String("dial_hz")).toDouble());
    s.slice = root->value(QLatin1String("slice")).toBool();
    s.audio = root->value(QLatin1String("audio")).toBool();

    const QJsonArray cycles = root->value(QLatin1String("cycles")).toArray();
    for (const QJsonValue& v : cycles) {
        if (s.cycles.size() >= kMaxCycles) {
            break;
        }
        const QJsonObject o = v.toObject();
        Ft8Cycle c;
        c.start = static_cast<qint64>(o.value(QLatin1String("t")).toDouble());
        c.band = boundedString(o.value(QLatin1String("band")), kMaxStringChars);
        c.dialHz = static_cast<qint64>(o.value(QLatin1String("dial_hz")).toDouble());
        c.spots = o.value(QLatin1String("spots")).toInt();
        c.decodeMs = o.value(QLatin1String("decode_ms")).toInt();
        c.audioPct = o.value(QLatin1String("audio_pct")).toInt();
        c.lostPackets = o.value(QLatin1String("lost_packets")).toInt();
        c.audioDbfs = o.value(QLatin1String("audio_dbfs")).toDouble();
        if (c.start <= 0 || c.decodeMs < 0) {
            continue;   // one unreadable cycle is dropped, not the status
        }
        s.cycles.append(c);
    }

    const QJsonArray spots = root->value(QLatin1String("spots")).toArray();
    for (const QJsonValue& v : spots) {
        if (s.spots.size() >= kMaxSpots) {
            break;
        }
        const QJsonObject o = v.toObject();
        Ft8Spot sp;
        sp.cycleStart = static_cast<qint64>(o.value(QLatin1String("t")).toDouble());
        sp.band = boundedString(o.value(QLatin1String("band")), kMaxStringChars);
        sp.score = o.value(QLatin1String("score")).toInt();
        sp.dt = o.value(QLatin1String("dt")).toDouble();
        sp.frequencyHz = static_cast<qint64>(o.value(QLatin1String("hz")).toDouble());
        sp.call = boundedString(o.value(QLatin1String("call")), kMaxStringChars);
        sp.grid = boundedString(o.value(QLatin1String("grid")), kMaxStringChars);
        sp.message = boundedString(o.value(QLatin1String("msg")), kMaxStringChars);
        if (sp.cycleStart <= 0 || sp.frequencyHz < kMinHz || sp.frequencyHz > kMaxHz
            || !validCall(sp.call) || !validGrid(sp.grid)) {
            continue;   // one bad spot is dropped
        }
        s.spots.append(sp);
    }
    return s;
}

std::optional<Ft8Config> parseConfig(const QByteArray& json, bool* claimed)
{
    const auto root = object(json);
    if (!root || !root->value(QLatin1String("config")).isObject()) {
        return std::nullopt;
    }
    if (claimed) {
        *claimed = root->value(QLatin1String("claimed")).toBool();
    }
    const QJsonObject o = root->value(QLatin1String("config")).toObject();
    Ft8Config c;
    c.callsign = o.value(QLatin1String("callsign")).toString();
    c.grid = o.value(QLatin1String("grid")).toString();
    c.band = o.value(QLatin1String("band")).toString();
    c.rxant = o.value(QLatin1String("rxant")).toString();
    if (!inRange(o.value(QLatin1String("dax_channel")), 1, 8)) {
        return std::nullopt;
    }
    c.daxChannel = o.value(QLatin1String("dax_channel")).toInt();
    static const QRegularExpression ant(QStringLiteral("^[A-Za-z0-9_]{0,11}$"));
    if (!validCall(c.callsign) || !validGrid(c.grid) || !bands().contains(c.band)
        || !ant.match(c.rxant).hasMatch()) {
        return std::nullopt;
    }
    return c;
}

QString adminToken(const QByteArray& json)
{
    const auto root = object(json);
    if (!root) {
        return {};
    }
    // The agent issues a 32-character hex token on the first POST only.
    static const QRegularExpression re(QStringLiteral("^[0-9a-f]{8,64}$"));
    const QString t = root->value(QLatin1String("admin_token")).toString();
    return re.match(t).hasMatch() ? t : QString();
}

QByteArray configJson(const Ft8Config& config)
{
    QJsonObject o;
    o.insert(QStringLiteral("callsign"), config.callsign);
    o.insert(QStringLiteral("grid"), config.grid);
    o.insert(QStringLiteral("band"), config.band);
    o.insert(QStringLiteral("rxant"), config.rxant);
    o.insert(QStringLiteral("dax_channel"), config.daxChannel);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
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

QString writeBundledPackage(const Ft8Package::Pin& pin, const QString& dir, QString* error)
{
    const auto matches = [&pin](const QByteArray& bytes) {
        return bytes.size() == pin.size
            && QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() == QByteArray(pin.sha256);
    };
    const QString target = QDir(dir).filePath(QString::fromLatin1(pin.fileName));
    QFile existing(target);
    if (existing.open(QIODevice::ReadOnly) && existing.size() == pin.size && matches(existing.readAll())) {
        return target;   // already written and intact
    }
    existing.close();
    QFile bundled(QString::fromLatin1(pin.resource));
    const QByteArray bytes = bundled.open(QIODevice::ReadOnly) ? bundled.read(pin.size + 1) : QByteArray();
    if (!matches(bytes)) {
        if (error) {
            *error = QObject::tr("This AetherSDR's FT8 receiver is damaged, so nothing was installed.");
        }
        return {};
    }
    if (!QDir().mkpath(dir)) {
        if (error) {
            *error = QObject::tr("The FT8 receiver could not be unpacked for installing.");
        }
        return {};
    }
    QFile out(target);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(bytes) != bytes.size()) {
        if (error) {
            *error = QObject::tr("The FT8 receiver could not be unpacked for installing.");
        }
        return {};
    }
    out.close();
    return target;
}

DxSpot toDxSpot(const Ft8Spot& spot, const QString& receiver)
{
    DxSpot d;
    d.spotterCall = receiver;
    d.freqMhz = static_cast<double>(spot.frequencyHz) / 1e6;
    d.dxCall = spot.call;
    // ⚠ "score", not "dB". ft8_lib's candidate score is a correlation
    // strength; calling it an SNR here would be read as dB by everything
    // downstream. wspr::toDxSpot sets hasSnr = true because a WSPR spot
    // carries a real dB figure. This one does not, so it does not.
    d.comment = QStringLiteral("FT8 score %1").arg(spot.score);
    if (!spot.message.isEmpty()) {
        d.comment += QStringLiteral(" — ") + spot.message;
    }
    d.utcTime = QDateTime::fromSecsSinceEpoch(spot.cycleStart, QTimeZone::UTC).time();
    d.source = QStringLiteral("FT8");
    d.hasSnr = false;
    d.dxGrid = spot.grid.left(4);
    return d;
}

QList<Ft8Spot> spotsSince(const QList<Ft8Spot>& spots, qint64 since, qint64* next)
{
    QList<Ft8Spot> fresh;
    qint64 newest = since - 1;
    for (const Ft8Spot& s : spots) {
        if (s.cycleStart >= since) {
            fresh.append(s);
            newest = qMax(newest, s.cycleStart);
        }
    }
    if (next) {
        // The NEXT cycle, so the newest is not sent twice. Written even when
        // nothing was fresh (newest stays since-1, so *next stays since), as
        // wspr::spotsSince does: a caller that only advances on a hit would
        // stall if it ever read a status with no new decodes.
        *next = newest + 1;
    }
    return fresh;
}

}  // namespace ft8
}  // namespace AetherSDR
