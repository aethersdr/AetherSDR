#pragma once

#include "core/tnc/Ax25.h"

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

namespace AetherSDR {

// Timed APRS position beacon. Position comes from the radio's onboard GPS
// (FLEX-8000-class / Aurora GPSDO — the owner pushes updates through
// setGpsPosition()) with a manual lat/lon fallback for radios without GPS
// hardware. Encoded frames go out through transmitFrame() into the shared
// modem TX queue, the same contract PmsMailbox uses.
class AprsBeacon : public QObject {
    Q_OBJECT

public:
    explicit AprsBeacon(QObject* parent = nullptr);

    void setMyAddress(const ax25::Address& addr) { m_myAddress = addr; }
    void setPath(const QVector<ax25::Address>& path) { m_path = path; }
    void setStatusText(const QString& text) { m_statusText = text; }
    void setSymbol(char table, char code) { m_symbolTable = table; m_symbolCode = code; }

    // Arms/stops the interval timer. Never transmits immediately — use
    // sendNow() for that.
    void setEnabled(bool on);
    bool isEnabled() const { return m_enabled; }
    void setIntervalMinutes(int minutes);
    int intervalMinutes() const { return m_intervalMin; }

    // GPS fix from RadioModel::gpsStatusChanged. `valid` false (no lock / no
    // GPS hardware) keeps the previous source; the beacon falls back to the
    // manual position when no GPS fix was ever delivered.
    void setGpsPosition(double lat, double lon, bool valid);
    void setManualPosition(double lat, double lon, bool valid);

    // The position the next beacon would use. Returns false when neither a
    // GPS fix nor a manual position is available.
    bool currentPosition(double& lat, double& lon) const;
    bool usingGps() const { return m_gpsValid; }

    // Fire one beacon immediately (also restarts the interval timer so a
    // manual send doesn't double up with an imminent timed one). Returns
    // false when no callsign or no position is configured.
    bool sendNow();

signals:
    void transmitFrame(const QByteArray& rawAx25NoFcs);
    void activity(const QString& line);

private:
    ax25::Address m_myAddress;
    QVector<ax25::Address> m_path;
    QString m_statusText;
    char m_symbolTable{'/'};
    char m_symbolCode{'-'};

    bool m_enabled{false};
    int m_intervalMin{30};
    QTimer m_timer;

    bool m_gpsValid{false};
    double m_gpsLat{0.0};
    double m_gpsLon{0.0};
    bool m_manualValid{false};
    double m_manualLat{0.0};
    double m_manualLon{0.0};
};

} // namespace AetherSDR
