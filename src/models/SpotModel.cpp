#include "SpotModel.h"
#include <QDateTime>

namespace MasterSDR {

void SpotModel::applySpotStatus(int index, const QMap<QString, QString>& kvs)
{
    bool isNew = !m_spots.contains(index);
    auto& spot = m_spots[index];
    spot.index = index;
    if (isNew)
        spot.addedMs = QDateTime::currentMSecsSinceEpoch();

    for (auto it = kvs.constBegin(); it != kvs.constEnd(); ++it) {
        const QString& key = it.key();
        const QString& val = it.value();

        if (key == "callsign")
            spot.callsign = QString(val).replace(QChar(0x7f), ' ');
        else if (key == "rx_freq")
            spot.rxFreqMhz = val.toDouble();
        else if (key == "tx_freq")
            spot.txFreqMhz = val.toDouble();
        else if (key == "mode")
            spot.mode = val;
        else if (key == "color")
            spot.color = val;
        else if (key == "background_color")
            spot.backgroundColor = val;
        else if (key == "source")
            spot.source = val;
        else if (key == "spotter_callsign")
            spot.spotterCallsign = val;
        else if (key == "comment")
            spot.comment = QString(val).replace(QChar(0x7f), ' ');
        else if (key == "timestamp") {
            bool ok;
            qint64 ts = val.toLongLong(&ok);
            if (ok)
                spot.timestamp = QDateTime::fromSecsSinceEpoch(ts, Qt::UTC);
        }
        else if (key == "lifetime_seconds")
            spot.lifetimeSeconds = val.toInt();
        else if (key == "priority")
            spot.priority = val.toInt();
    }

    if (isNew)
        emit spotAdded(spot);
    else
        emit spotUpdated(spot);
}

void SpotModel::removeSpot(int index)
{
    if (m_spots.remove(index))
        emit spotRemoved(index);
}

void SpotModel::clear()
{
    m_spots.clear();
    emit spotsCleared();
}

void SpotModel::refresh()
{
    emit spotsRefreshed();
}

} // namespace MasterSDR
