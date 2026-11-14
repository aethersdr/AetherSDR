#include "GpsdSkyClient.h"

#include "TailnetAddress.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QTimer>

namespace AetherSDR {

namespace {
constexpr int kRetryMs = 10000;
constexpr int kConnectTimeoutMs = 5000;
// gpsd sends a full SKY every few seconds while the receiver runs.
constexpr int kStaleMs = 30000;
// gpsd's largest report (GPS_JSON_RESPONSE_MAX) is 10 KB; a SKY with every
// constellation stays well inside this.
constexpr qsizetype kMaxLine = 64 * 1024;
constexpr qsizetype kMaxSatellites = 128;
// SKY reports a satellite may be missing from before it leaves the view.
constexpr int kMissedLimit = 4;

double ranged(const QJsonObject& o, const char* key, double lo, double hi)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isDouble()) {
        return NAN;
    }
    const double d = v.toDouble();
    return d >= lo && d <= hi ? d : NAN;
}

QString printable(const QJsonValue& v, int max)
{
    QString s = v.toString().left(max);
    for (const QChar c : s) {
        if (!c.isPrint()) {
            return {};
        }
    }
    return s;
}
}  // namespace

namespace gpsdsky {

Constellation constellation(const GpsSatellite& s)
{
    switch (s.gnssId) {
    case 0: return Constellation::Gps;
    case 1: return Constellation::Sbas;
    case 2: return Constellation::Galileo;
    case 3: return Constellation::BeiDou;
    case 4: return Constellation::Imes;
    case 5: return Constellation::Qzss;
    case 6: return Constellation::Glonass;
    case 7: return Constellation::NavIc;
    default: break;
    }
    // gpsd's own PRN numbering, for reports without gnssid.
    const int p = s.prn;
    if (p >= 1 && p <= 63) return Constellation::Gps;
    if (p >= 64 && p <= 96) return Constellation::Glonass;
    if (p >= 120 && p <= 158) return Constellation::Sbas;
    if (p >= 193 && p <= 200) return Constellation::Qzss;
    if (p >= 201 && p <= 263) return Constellation::BeiDou;
    if (p >= 301 && p <= 336) return Constellation::Galileo;
    return Constellation::Unknown;
}

QString constellationName(Constellation c)
{
    switch (c) {
    case Constellation::Gps: return QObject::tr("GPS");
    case Constellation::Sbas: return QObject::tr("SBAS");
    case Constellation::Galileo: return QObject::tr("Galileo");
    case Constellation::BeiDou: return QObject::tr("BeiDou");
    case Constellation::Imes: return QObject::tr("IMES");
    case Constellation::Qzss: return QObject::tr("QZSS");
    case Constellation::Glonass: return QObject::tr("GLONASS");
    case Constellation::NavIc: return QObject::tr("NavIC");
    case Constellation::Unknown: break;
    }
    return QObject::tr("Other");
}

std::optional<QList<GpsSatellite>> parseSky(const QByteArray& line)
{
    const QJsonDocument doc = QJsonDocument::fromJson(line.trimmed());
    if (!doc.isObject()) {
        return std::nullopt;
    }
    return parseSky(doc.object());
}

std::optional<QList<GpsSatellite>> parseSky(const QJsonObject& o)
{
    if (o.value(QStringLiteral("class")).toString() != QLatin1String("SKY")
        || !o.value(QStringLiteral("satellites")).isArray()) {
        return std::nullopt;
    }
    QList<GpsSatellite> out;
    for (const QJsonValue& v : o.value(QStringLiteral("satellites")).toArray()) {
        if (out.size() >= kMaxSatellites) {
            break;
        }
        const QJsonObject s = v.toObject();
        const int prn = s.value(QStringLiteral("PRN")).toInt(0);
        if (prn <= 0 || prn > 1000) {
            continue;
        }
        GpsSatellite sat;
        sat.prn = prn;
        const int gnss = s.value(QStringLiteral("gnssid")).toInt(-1);
        sat.gnssId = gnss >= 0 && gnss <= 7 ? gnss : -1;
        const QJsonValue el = s.value(QStringLiteral("el"));
        const QJsonValue az = s.value(QStringLiteral("az"));
        if (el.isDouble() && az.isDouble()) {
            const double e = el.toDouble(), a = az.toDouble();
            // gpsd reports elevation -90..90 and azimuth 0..360. Below the
            // horizon or out of range is not a sky position, and 0/0 is what
            // gpsd 3.22 sends for a satellite whose position it doesn't know
            // (seen on a FLEX-8600), which would sit on the north rim.
            if (e >= 0.0 && e <= 90.0 && a >= 0.0 && a <= 360.0 && (e != 0.0 || a != 0.0)) {
                sat.elevationDeg = e;
                sat.azimuthDeg = a >= 360.0 ? 0.0 : a;
            }
        }
        const double ss = s.value(QStringLiteral("ss")).toDouble(0.0);
        sat.snrDbHz = ss > 0.0 && ss < 100.0 ? ss : 0.0;
        sat.used = s.value(QStringLiteral("used")).toBool(false);
        out.append(sat);
    }
    return out;
}

bool applyDetail(const QJsonObject& o, GpsdDetail& d)
{
    const QString cls = o.value(QStringLiteral("class")).toString();
    if (cls == QLatin1String("SKY")) {
        // A DOP that is absent from this report keeps its last value.
        auto dop = [&](const char* key, double& field) {
            const double v = ranged(o, key, 0.0, 100.0);
            if (std::isfinite(v)) {
                field = v;
            }
        };
        dop("hdop", d.hdop);
        dop("vdop", d.vdop);
        dop("pdop", d.pdop);
        dop("tdop", d.tdop);
        dop("gdop", d.gdop);
        return true;
    }
    if (cls == QLatin1String("TPV")) {
        const int mode = o.value(QStringLiteral("mode")).toInt(-1);
        if (mode < 0 || mode > 3) {
            return false;
        }
        d.mode = mode;
        // Errors gpsd leaves out (no fix) are unknown, not the last value.
        d.ephM = ranged(o, "eph", 0.0, 1.0e6);
        d.epvM = ranged(o, "epv", 0.0, 1.0e6);
        d.eptS = ranged(o, "ept", 0.0, 1.0e3);
        return true;
    }
    if (cls == QLatin1String("PPS")) {
        const QJsonValue rs = o.value(QStringLiteral("real_sec")), rn = o.value(QStringLiteral("real_nsec"));
        const QJsonValue cs = o.value(QStringLiteral("clock_sec")), cn = o.value(QStringLiteral("clock_nsec"));
        if (!rs.isDouble() || !rn.isDouble() || !cs.isDouble() || !cn.isDouble()) {
            return false;
        }
        const qint64 rns = rn.toInteger(-1), cns = cn.toInteger(-1);
        if (rns < 0 || rns >= 1000000000 || cns < 0 || cns >= 1000000000) {
            return false;
        }
        const qint64 secs = cs.toInteger() - rs.toInteger();
        if (secs < -100 || secs > 100) {
            return false;   // the clock is far off: an offset in ns means nothing
        }
        // A pulse marks the start of a second; which second comes from the
        // receiver's NMEA, and gpsd can label it one off (a FLEX-8600 reports
        // clock_sec one behind real_sec with clock_nsec ~100 us). So the offset
        // is only meaningful within the second: fold it into -0.5 s..+0.5 s.
        qint64 offset = (secs * 1000000000 + (cns - rns)) % 1000000000;
        if (offset >= 500000000) {
            offset -= 1000000000;
        } else if (offset < -500000000) {
            offset += 1000000000;
        }
        d.ppsOffsetNs = offset;
        d.hasPps = true;
        return true;
    }
    if (cls == QLatin1String("DEVICES")) {
        d.device.clear();
        d.driver.clear();
        d.bps = 0;
        d.ppsDevice.clear();
        for (const QJsonValue& v : o.value(QStringLiteral("devices")).toArray()) {
            const QJsonObject dev = v.toObject();
            const QString driver = printable(dev.value(QStringLiteral("driver")), 32);
            const QString path = printable(dev.value(QStringLiteral("path")), 64);
            if (driver == QLatin1String("PPS")) {
                d.ppsDevice = path;
            } else if (d.device.isEmpty()) {
                d.device = path;
                d.driver = driver;
                const int bps = dev.value(QStringLiteral("bps")).toInt(0);
                d.bps = bps > 0 && bps <= 10000000 ? bps : 0;
            }
        }
        return true;
    }
    return false;
}

bool reachableRadio(bool overWan, const QHostAddress& address)
{
    if (overWan || address.isNull()) {
        return false;
    }
    // The relay listens on the radio's IPv4 LAN address; the shim publishes
    // it on both of its tailnet addresses.
    const bool lan = address.protocol() == QAbstractSocket::IPv4Protocol
        && (address.isPrivateUse() || address.isLinkLocal());
    return lan || isTailnetAddress(address);
}

}  // namespace gpsdsky

GpsdSkyClient::GpsdSkyClient(QObject* parent)
    : QObject(parent)
    , m_retry(new QTimer(this))
    , m_stale(new QTimer(this))
{
    m_retry->setSingleShot(true);
    m_retry->setInterval(kRetryMs);
    connect(m_retry, &QTimer::timeout, this, &GpsdSkyClient::openSession);
    m_stale->setSingleShot(true);
    m_stale->setInterval(kStaleMs);
    connect(m_stale, &QTimer::timeout, this, &GpsdSkyClient::skyStale);
}

void GpsdSkyClient::start(const QHostAddress& address)
{
    if (m_state != State::Stopped && address == m_address) {
        return;
    }
    if (m_socket) {
        closeSession({});
    }
    m_address = address;
    m_retry->stop();
    openSession();
}

void GpsdSkyClient::stop()
{
    m_retry->stop();
    if (m_socket) {
        closeSession({});
    }
    setState(State::Stopped);
}

void GpsdSkyClient::openSession()
{
    if (m_address.isNull() || m_socket) {
        return;
    }
    m_buffer.clear();
    m_overflow = false;
    m_release.clear();
    m_reached = false;
    m_sky.clear();
    m_detail = {};
    auto* socket = new QTcpSocket(this);
    m_socket = socket;
    setState(State::Connecting);
    connect(socket, &QTcpSocket::connected, this, [this, socket] {
        if (socket != m_socket) {
            return;
        }
        m_reached = true;
        m_stale->start();
        setState(State::Streaming);
        socket->write("?WATCH={\"enable\":true,\"json\":true,\"pps\":true};\n");
    });
    connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
        if (socket == m_socket) {
            ingest(socket->readAll());
        }
    });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
        if (socket == m_socket) {
            closeSession(tr("The radio closed the GPS connection."));
        }
    });
    connect(socket, &QTcpSocket::errorOccurred, this, [this, socket] {
        if (socket == m_socket) {
            closeSession(tr("Nothing answered on the radio's GPS port (%1).").arg(gpsdsky::kGpsdPort));
        }
    });
    QTimer::singleShot(kConnectTimeoutMs, socket, [this, socket] {
        if (socket == m_socket && socket->state() != QAbstractSocket::ConnectedState) {
            closeSession(tr("The radio's GPS port didn't answer."));
        }
    });
    socket->connectToHost(m_address, gpsdsky::kGpsdPort);
}

void GpsdSkyClient::closeSession(const QString& reason)
{
    if (QTcpSocket* socket = m_socket) {
        m_socket = nullptr;
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    m_buffer.clear();
    m_stale->stop();
    if (!reason.isEmpty()) {
        setState(State::Unavailable, reason);
        m_retry->start();
    }
}

void GpsdSkyClient::setState(State state, const QString& detail)
{
    if (state == m_state && detail.isEmpty()) {
        return;
    }
    m_state = state;
    emit stateChanged(state, detail);
}

void GpsdSkyClient::ingest(const QByteArray& chunk)
{
    m_buffer += chunk;
    qsizetype start = 0;
    for (qsizetype nl; (nl = m_buffer.indexOf('\n', start)) >= 0; start = nl + 1) {
        if (m_overflow) {
            m_overflow = false;   // the end of a line too long to keep
            continue;
        }
        // A whole line can exceed the bound without the tail check below
        // ever seeing it, when its newline arrives in the same read.
        if (nl - start <= kMaxLine) {
            handleLine(m_buffer.mid(start, nl - start));
        }
    }
    m_buffer.remove(0, start);
    if (m_buffer.size() > kMaxLine) {
        m_buffer.clear();
        m_overflow = true;
    }
}

void GpsdSkyClient::handleLine(const QByteArray& line)
{
    const QJsonObject o = QJsonDocument::fromJson(line.trimmed()).object();
    if (const auto sky = gpsdsky::parseSky(o)) {
        m_stale->start();
        for (auto it = m_sky.begin(); it != m_sky.end(); ++it) {
            ++it->missed;
        }
        for (const GpsSatellite& s : *sky) {
            m_sky.insert(s.prn, {s, 0});
        }
        m_sky.removeIf([](const auto& entry) { return entry.value().missed >= kMissedLimit; });
        QList<GpsSatellite> merged;
        for (const Tracked& t : std::as_const(m_sky)) {
            if (merged.size() < kMaxSatellites) {
                merged.append(t.satellite);
            }
        }
        emit skyUpdated(merged);
    }
    if (gpsdsky::applyDetail(o, m_detail)) {
        emit detailUpdated(m_detail);
        return;
    }
    const QString cls = o.value(QStringLiteral("class")).toString();
    if (cls == QLatin1String("VERSION")) {
        m_release = o.value(QStringLiteral("release")).toString().left(32);
    } else if (cls == QLatin1String("ERROR") && m_socket) {
        // The relay's "gpsd is not running" arrives this way, then a close.
        const QString message = o.value(QStringLiteral("message")).toString().left(200);
        closeSession(message.isEmpty() ? tr("The radio's GPS service reported an error.")
                                       : message);
    }
}

}  // namespace AetherSDR
