#include "core/tnc/HeardList.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace AetherSDR {

using ax25::Address;
using ax25::Frame;
using ax25::FrameType;

namespace {
// A printable, single-line rendering of a UI beacon's info field (control chars
// and CR/LF collapsed to spaces) so MHEARD stays one row per station.
QString beaconText(const QByteArray& info)
{
    QString text = QString::fromLatin1(info).trimmed();
    for (QChar& c : text) {
        if (c < QChar(0x20))
            c = QLatin1Char(' ');
    }
    return text.simplified();
}
} // namespace

HeardList::HeardList(QObject* parent)
    : QObject(parent)
{
}

HeardList::~HeardList() = default;

void HeardList::setPersistencePath(const QString& path)
{
    m_path = path;
    if (!m_path.isEmpty())
        load();
}

void HeardList::record(const Frame& frame)
{
    if (!frame.src.isValid())
        return;

    QString via;
    if (!frame.via.isEmpty()) {
        QStringList v;
        for (const Address& a : frame.via)
            v << a.toString();
        via = v.join(QLatin1Char(','));
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    const bool isBeacon = (frame.type == FrameType::UI) && !frame.info.isEmpty();

    for (Station& s : m_stations) {
        if (s.station == frame.src) {
            s.utc = now;
            s.dest = frame.dest.toString();
            s.via = via;
            ++s.count;
            if (isBeacon) {
                s.lastBeacon = beaconText(frame.info);
                s.beaconUtc = now;
            }
            save();
            emit changed();
            return;
        }
    }

    Station s;
    s.station = frame.src;
    s.dest = frame.dest.toString();
    s.via = via;
    s.utc = now;
    if (isBeacon) {
        s.lastBeacon = beaconText(frame.info);
        s.beaconUtc = now;
    }
    m_stations.append(s);

    if (m_stations.size() > m_max) {
        std::sort(m_stations.begin(), m_stations.end(),
                  [](const Station& a, const Station& b) { return a.utc > b.utc; });
        m_stations.resize(m_max);
    }
    save();
    emit changed();
}

QVector<HeardList::Station> HeardList::stations(int max) const
{
    QVector<Station> sorted = m_stations;
    std::sort(sorted.begin(), sorted.end(),
              [](const Station& a, const Station& b) { return a.utc > b.utc; });
    if (max >= 0 && sorted.size() > max)
        sorted.resize(max);
    return sorted;
}

QStringList HeardList::summary(int n) const
{
    const QVector<Station> sorted = stations(n);
    QStringList out;
    for (const Station& s : sorted) {
        out << QStringLiteral("%1  %2  %3")
                   .arg(s.station.toString().leftJustified(9),
                        s.utc.toString(QStringLiteral("MM/dd HH:mm")),
                        s.via);
    }
    return out;
}

void HeardList::clear()
{
    m_stations.clear();
    save();
    emit changed();
}

void HeardList::load()
{
    m_stations.clear();
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (const QJsonValue& v : root.value(QStringLiteral("heard")).toArray()) {
        const QJsonObject o = v.toObject();
        auto addr = Address::parse(o.value(QStringLiteral("call")).toString());
        if (!addr)
            continue;
        Station s;
        s.station = *addr;
        s.dest = o.value(QStringLiteral("dest")).toString();
        s.via = o.value(QStringLiteral("via")).toString();
        s.lastBeacon = o.value(QStringLiteral("beacon")).toString();
        s.utc = QDateTime::fromString(o.value(QStringLiteral("utc")).toString(), Qt::ISODate);
        s.beaconUtc = QDateTime::fromString(
            o.value(QStringLiteral("beaconUtc")).toString(), Qt::ISODate);
        s.count = o.value(QStringLiteral("count")).toInt(1);
        m_stations.append(s);
    }
}

void HeardList::save() const
{
    if (m_path.isEmpty())
        return;
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QJsonArray arr;
    for (const Station& s : m_stations) {
        QJsonObject o;
        o.insert(QStringLiteral("call"), s.station.toString());
        o.insert(QStringLiteral("dest"), s.dest);
        o.insert(QStringLiteral("via"), s.via);
        o.insert(QStringLiteral("beacon"), s.lastBeacon);
        o.insert(QStringLiteral("utc"), s.utc.toString(Qt::ISODate));
        o.insert(QStringLiteral("beaconUtc"), s.beaconUtc.toString(Qt::ISODate));
        o.insert(QStringLiteral("count"), s.count);
        arr.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("heard"), arr);
    QFile f(m_path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(root).toJson());
}

} // namespace AetherSDR
