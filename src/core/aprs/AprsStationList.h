#pragma once

#include "core/aprs/AprsPacket.h"

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

namespace AetherSDR {

// The APRS station roster behind the station table: every station heard,
// with its most recent position, movement, and free-text data merged across
// packets (a Mic-E position and a separate status report both land on the
// same row). Digipeated duplicates of one transmission are suppressed so
// packet counts reflect originated traffic, not how many copies we heard.
// Follows the HeardList persistence pattern: optional JSON file, writes
// coalesced through a single-shot timer.
class AprsStationList : public QObject {
    Q_OBJECT

public:
    struct Station {
        QString call;            // "N0CALL-9"
        QDateTime firstHeard;    // UTC
        QDateTime lastHeard;     // UTC
        int packets{0};

        bool hasPosition{false};
        double latitude{0.0};
        double longitude{0.0};
        QDateTime positionUtc;
        char symbolTable{'/'};
        char symbolCode{'-'};
        double courseDeg{-1.0};  // < 0 when unknown
        double speedKnots{-1.0}; // < 0 when unknown
        bool hasAltitude{false};
        double altitudeFeet{0.0};

        // Weather station data: comment holds the readable weatherSummary().
        bool isWeather{false};
        int wxCondition{0};      // aprsicons weather condition (cloud/rain/wind)

        QString comment;         // last position comment / weather summary
        QString status;          // last '>' status text
        QString via;             // last digipeater path, comma-joined
        QString lastInfo;        // raw info field of the last packet
        aprs::PacketType lastType{aprs::PacketType::Other};
    };

    explicit AprsStationList(QObject* parent = nullptr);
    ~AprsStationList() override;

    // Empty path (the default) keeps the list in-memory only.
    void setPersistencePath(const QString& path);
    void setMaxStations(int n) { m_max = qBound(10, n, 5000); }

    // Merge a parsed packet into the roster. Returns false when the packet
    // was dropped as a digipeated duplicate.
    bool record(const aprs::Packet& packet);

    // Stations, most-recently-heard first.
    QVector<Station> stations() const;
    std::optional<Station> find(const QString& call) const;

    int size() const { return m_stations.size(); }
    void clear();

signals:
    void changed();

private:
    void load();
    void save() const;
    void scheduleSave();

    QVector<Station> m_stations;
    QString m_path;
    int m_max{500};
    QTimer m_saveCoalesce; // single-shot, ~2 s; restarts on each scheduleSave()

    // Duplicate suppression: a WIDE2 digipeat delivers the same source+info
    // seconds after the direct copy. Remember the last payload per station
    // and drop repeats inside the window.
    static constexpr int kDupeWindowSecs = 30;
};

} // namespace AetherSDR
