#include "core/aprs/AprsStationList.h"

#include "core/LogManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>

#include <algorithm>

namespace AetherSDR {

AprsStationList::AprsStationList(QObject* parent)
    : QObject(parent)
{
    m_saveCoalesce.setSingleShot(true);
    m_saveCoalesce.setInterval(2000); // fold a digi burst into one rewrite
    connect(&m_saveCoalesce, &QTimer::timeout, this, [this] { save(); });
}

AprsStationList::~AprsStationList()
{
    if (m_saveCoalesce.isActive()) {
        m_saveCoalesce.stop();
        save();
    }
}

void AprsStationList::setPersistencePath(const QString& path)
{
    m_path = path;
    if (!m_path.isEmpty())
        load();
}

bool AprsStationList::record(const aprs::Packet& packet)
{
    if (packet.source.isEmpty())
        return false;

    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QString via = packet.path.join(QLatin1Char(','));

    Station* hit = nullptr;
    for (Station& s : m_stations) {
        if (s.call.compare(packet.source, Qt::CaseInsensitive) == 0) {
            hit = &s;
            break;
        }
    }

    // Digipeated duplicate: the same transmission relayed by another hop
    // arrives with identical source + info within seconds of the original.
    if (hit && hit->lastInfo == packet.infoText
        && hit->lastHeard.secsTo(now) <= kDupeWindowSecs)
        return false;

    if (!hit) {
        Station s;
        s.call = packet.source;
        s.firstHeard = now;
        m_stations.append(s);
        hit = &m_stations.last();
    }

    hit->lastHeard = now;
    hit->packets += 1;
    hit->via = via;
    hit->lastInfo = packet.infoText;
    hit->lastType = packet.type;

    if (packet.hasPosition) {
        hit->hasPosition = true;
        hit->latitude = packet.latitude;
        hit->longitude = packet.longitude;
        hit->positionUtc = now;
        hit->symbolTable = packet.symbolTable;
        hit->symbolCode = packet.symbolCode;
        hit->courseDeg = packet.courseDeg;
        hit->speedKnots = packet.speedKnots;
        if (packet.hasAltitude) {
            hit->hasAltitude = true;
            hit->altitudeFeet = packet.altitudeFeet;
        }
    }
    // Free text merges instead of overwriting: a status report shouldn't
    // blank the comment a position packet carried a minute earlier.
    if (packet.type == aprs::PacketType::Status) {
        hit->status = packet.comment;
    } else if (packet.type == aprs::PacketType::Weather) {
        hit->isWeather = true;
        if (!packet.comment.isEmpty())
            hit->comment = packet.comment; // pre-formatted weatherSummary()
        // Condition drives the wireframe icon: rain beats wind beats cloud.
        if (packet.weather.rainHourIn > 0.0 || packet.weather.rain24hIn > 0.0)
            hit->wxCondition = 1;
        else if (packet.weather.gustMph >= 20.0 || packet.weather.windMph >= 15.0)
            hit->wxCondition = 2;
        else
            hit->wxCondition = 0;
    } else if (!packet.comment.isEmpty()
               && packet.type == aprs::PacketType::Position) {
        hit->comment = packet.comment;
    }

    if (m_stations.size() > m_max) {
        std::sort(m_stations.begin(), m_stations.end(),
                  [](const Station& a, const Station& b) {
                      return a.lastHeard > b.lastHeard;
                  });
        m_stations.resize(m_max);
    }
    scheduleSave();
    emit changed();
    return true;
}

QVector<AprsStationList::Station> AprsStationList::stations() const
{
    QVector<Station> sorted = m_stations;
    std::sort(sorted.begin(), sorted.end(),
              [](const Station& a, const Station& b) {
                  return a.lastHeard > b.lastHeard;
              });
    return sorted;
}

std::optional<AprsStationList::Station> AprsStationList::find(const QString& call) const
{
    for (const Station& s : m_stations) {
        if (s.call.compare(call, Qt::CaseInsensitive) == 0)
            return s;
    }
    return std::nullopt;
}

void AprsStationList::clear()
{
    m_stations.clear();
    save();
    emit changed();
}

void AprsStationList::scheduleSave()
{
    if (m_path.isEmpty())
        return;
    m_saveCoalesce.start();
}

void AprsStationList::load()
{
    m_stations.clear();
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (const QJsonValue& v : root.value(QStringLiteral("stations")).toArray()) {
        const QJsonObject o = v.toObject();
        Station s;
        s.call = o.value(QStringLiteral("call")).toString();
        if (s.call.isEmpty())
            continue;
        s.firstHeard = QDateTime::fromString(
            o.value(QStringLiteral("firstHeard")).toString(), Qt::ISODate);
        s.lastHeard = QDateTime::fromString(
            o.value(QStringLiteral("lastHeard")).toString(), Qt::ISODate);
        s.packets = o.value(QStringLiteral("packets")).toInt(1);
        s.hasPosition = o.value(QStringLiteral("hasPosition")).toBool();
        s.latitude = o.value(QStringLiteral("lat")).toDouble();
        s.longitude = o.value(QStringLiteral("lon")).toDouble();
        s.positionUtc = QDateTime::fromString(
            o.value(QStringLiteral("positionUtc")).toString(), Qt::ISODate);
        const QString table = o.value(QStringLiteral("symbolTable")).toString("/");
        const QString code = o.value(QStringLiteral("symbolCode")).toString("-");
        s.symbolTable = table.isEmpty() ? '/' : table.at(0).toLatin1();
        s.symbolCode = code.isEmpty() ? '-' : code.at(0).toLatin1();
        s.courseDeg = o.value(QStringLiteral("courseDeg")).toDouble(-1.0);
        s.speedKnots = o.value(QStringLiteral("speedKnots")).toDouble(-1.0);
        s.hasAltitude = o.value(QStringLiteral("hasAltitude")).toBool();
        s.altitudeFeet = o.value(QStringLiteral("altitudeFeet")).toDouble();
        s.isWeather = o.value(QStringLiteral("isWeather")).toBool();
        s.wxCondition = o.value(QStringLiteral("wxCondition")).toInt();
        s.comment = o.value(QStringLiteral("comment")).toString();
        s.status = o.value(QStringLiteral("status")).toString();
        s.via = o.value(QStringLiteral("via")).toString();
        s.lastInfo = o.value(QStringLiteral("lastInfo")).toString();
        m_stations.append(s);
    }
}

void AprsStationList::save() const
{
    if (m_path.isEmpty())
        return;
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QJsonArray arr;
    for (const Station& s : m_stations) {
        QJsonObject o;
        o.insert(QStringLiteral("call"), s.call);
        o.insert(QStringLiteral("firstHeard"), s.firstHeard.toString(Qt::ISODate));
        o.insert(QStringLiteral("lastHeard"), s.lastHeard.toString(Qt::ISODate));
        o.insert(QStringLiteral("packets"), s.packets);
        o.insert(QStringLiteral("hasPosition"), s.hasPosition);
        o.insert(QStringLiteral("lat"), s.latitude);
        o.insert(QStringLiteral("lon"), s.longitude);
        o.insert(QStringLiteral("positionUtc"), s.positionUtc.toString(Qt::ISODate));
        o.insert(QStringLiteral("symbolTable"), QString(QLatin1Char(s.symbolTable)));
        o.insert(QStringLiteral("symbolCode"), QString(QLatin1Char(s.symbolCode)));
        o.insert(QStringLiteral("courseDeg"), s.courseDeg);
        o.insert(QStringLiteral("speedKnots"), s.speedKnots);
        o.insert(QStringLiteral("hasAltitude"), s.hasAltitude);
        o.insert(QStringLiteral("altitudeFeet"), s.altitudeFeet);
        o.insert(QStringLiteral("isWeather"), s.isWeather);
        o.insert(QStringLiteral("wxCondition"), s.wxCondition);
        o.insert(QStringLiteral("comment"), s.comment);
        o.insert(QStringLiteral("status"), s.status);
        o.insert(QStringLiteral("via"), s.via);
        o.insert(QStringLiteral("lastInfo"), s.lastInfo);
        arr.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("stations"), arr);
    // QSaveFile writes to a temp file and atomically renames on commit() so an
    // unclean shutdown mid-write can't truncate or corrupt the roster
    // (Constitution Principle XIV — Persist Atomically).
    QSaveFile f(m_path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qCWarning(lcAx25).noquote()
            << "AprsStationList: could not write" << m_path << "—" << f.errorString();
        return;
    }
    f.write(QJsonDocument(root).toJson());
    if (!f.commit())
        qCWarning(lcAx25).noquote()
            << "AprsStationList: could not commit" << m_path << "—" << f.errorString();
}

} // namespace AetherSDR
