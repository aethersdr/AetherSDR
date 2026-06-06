#pragma once

#include "core/tnc/Ax25.h"

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

namespace AetherSDR {

// A station-heard log shared by the PMS mailbox (JHEARD) and the TNC terminal
// (MHEARD): who has been decoded on the air, when, via what path, and the text
// of the last UI beacon they sent. It is RF-agnostic — feed it every decoded
// ax25::Frame via record() — and optionally persists to JSON so the list
// survives restarts. The PMS header always anticipated this being "split out so
// the future APRS/AX.25 digipeater can reuse the same plumbing"; this is it.
class HeardList : public QObject {
    Q_OBJECT

public:
    struct Station {
        ax25::Address station;   // source callsign incl. SSID
        QString dest;            // last destination heard
        QString via;             // last digipeater path (comma-joined), or empty
        QString lastBeacon;      // text of the last UI (beacon) frame, if any
        QDateTime utc;           // last heard (UTC)
        QDateTime beaconUtc;     // when lastBeacon was captured (UTC)
        int count{1};            // total frames heard from this station
    };

    explicit HeardList(QObject* parent = nullptr);
    ~HeardList() override;

    // Point at a JSON file to load now and persist to on change (writes are
    // coalesced through a single-shot QTimer so a beacon burst collapses into
    // one rewrite — see scheduleSave()). Pass an empty path for an
    // in-memory-only list (the default).
    void setPersistencePath(const QString& path);
    void setMaxStations(int n) { m_max = qBound(10, n, 5000); }

    // Record a decoded frame. The src becomes/refreshes a Station entry; a UI
    // frame with non-empty info also updates that station's last-beacon text.
    void record(const ax25::Frame& frame);

    // Stations, most-recently-heard first, capped at `max`.
    QVector<Station> stations(int max = 200) const;

    // Compact "CALL-SSID  MM/dd HH:mm  via" lines (PMS JHEARD compatible).
    QStringList summary(int n) const;

    int size() const { return m_stations.size(); }
    bool isEmpty() const { return m_stations.isEmpty(); }
    void clear();

signals:
    void changed();

private:
    void load();
    void save() const;
    // Schedule a deferred save() if a persistence path is set. Coalesces
    // bursts of record() — a busy APRS channel can decode hundreds of frames
    // per minute, and a synchronous JSON rewrite per frame churns the disk
    // for no observer-visible benefit.
    void scheduleSave();

    QVector<Station> m_stations;
    QString m_path;
    int m_max{200};
    QTimer m_saveCoalesce;  // single-shot, ~2 s; restarts on each scheduleSave()
};

} // namespace AetherSDR
