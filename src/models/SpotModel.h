#pragma once

#include <QObject>
#include <QMap>
#include <QString>
#include <QDateTime>

namespace AetherSDR {

struct SpotData {
    int index{-1};
    QString callsign;
    double rxFreqMhz{0.0};
    double txFreqMhz{0.0};
    QString mode;
    QString color;          // #AARRGGBB
    QString backgroundColor;
    QString source;
    QString spotterCallsign;
    QString comment;
    QDateTime timestamp;
    int lifetimeSeconds{1800};  // default 30 min
    int priority{0};
    qint64 addedMs{0};         // local wall-clock when added
};

class SpotModel : public QObject {
    Q_OBJECT
public:
    explicit SpotModel(QObject* parent = nullptr) : QObject(parent) {}

    const QMap<int, SpotData>& spots() const { return m_spots; }

    void applySpotStatus(int index, const QMap<QString, QString>& kvs);
    void removeSpot(int index);
    void clear();
    void refresh();

signals:
    void spotAdded(const SpotData& spot);
    void spotUpdated(const SpotData& spot);
    void spotRemoved(int index);
    void spotsCleared();
    void spotsRefreshed();
    void spotTriggered(int index, const QString& panId);

private:
    QMap<int, SpotData> m_spots;
};

} // namespace AetherSDR
