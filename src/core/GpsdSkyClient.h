#pragma once

#include <QHostAddress>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>

#include <cmath>
#include <optional>

class QTcpSocket;
class QTimer;

namespace AetherSDR {

// One satellite from a gpsd SKY report (gpsd's JSON protocol, "SKY").
struct GpsSatellite {
    int prn{0};
    int gnssId{-1};          // gpsd gnssid: 0 GPS, 1 SBAS, 2 Galileo, 3 BeiDou,
                             // 4 IMES, 5 QZSS, 6 GLONASS, 7 NavIC; -1 not sent
    double elevationDeg{NAN};   // NaN when gpsd doesn't know it
    double azimuthDeg{NAN};
    double snrDbHz{0.0};        // 0: listed but not heard
    bool used{false};           // in the current fix

    bool heard() const { return snrDbHz > 0.0; }
    bool hasSkyPosition() const { return std::isfinite(elevationDeg) && std::isfinite(azimuthDeg); }
};

// The receiver detail gpsd reports beside the satellites, for AetherGPS's
// readouts. Unset values are NaN, 0 or empty. Position is deliberately not
// here: AetherGPS shows the radio's own.
struct GpsdDetail {
    int mode{0};                // TPV: 0 unknown, 1 no fix, 2 2D, 3 3D
    double hdop{NAN};           // SKY dilutions of precision
    double vdop{NAN};
    double pdop{NAN};
    double tdop{NAN};
    double gdop{NAN};
    double ephM{NAN};           // TPV: estimated horizontal error, m (95%)
    double epvM{NAN};           // estimated vertical error, m (95%)
    double eptS{NAN};           // estimated time error, s (95%)
    bool hasPps{false};
    qint64 ppsOffsetNs{0};      // PPS: the radio's clock minus GPS time at the pulse,
                                // within the second (-0.5 s..+0.5 s)
    QString device;             // DEVICES: the receiver's path, driver and speed
    QString driver;
    int bps{0};
    QString ppsDevice;          // the PPS source, when gpsd has one
};

namespace gpsdsky {

// The AetherGPS relay (tools/aether-gps) serves the radio's gpsd here, at
// the radio's LAN address, and through the tailnet shim when it runs.
inline constexpr quint16 kGpsdPort = 2947;

enum class Constellation { Gps, Sbas, Galileo, BeiDou, Imes, Qzss, Glonass, NavIc, Unknown };

// From gnssid when gpsd sends it, else from gpsd's PRN numbering.
Constellation constellation(const GpsSatellite& satellite);
QString constellationName(Constellation c);

// The satellites of a SKY report. nullopt for any other line, and for a SKY
// that carries only DOPs (gpsd 3.2x sends those between full reports).
// Bounded: at most 128 satellites, values outside their ranges dropped.
std::optional<QList<GpsSatellite>> parseSky(const QByteArray& line);
std::optional<QList<GpsSatellite>> parseSky(const QJsonObject& report);

// Folds one gpsd report (SKY, TPV, PPS or DEVICES) into `detail`, range-
// checking every value; other classes and out-of-range values change
// nothing. True when it applied.
bool applyDetail(const QJsonObject& report, GpsdDetail& detail);

// The relay answers on the radio's local network, or through the tailnet
// shim (a tailnet address, TailnetAddress.h). Over SmartLink the remembered
// address may be stale.
bool reachableRadio(bool overWan, const QHostAddress& address);

}  // namespace gpsdsky

// A gpsd client for the AetherGPS relay on the radio: watches SKY and emits
// each full sky view. Read-only: it sends one ?WATCH and nothing else.
// Not SmartSDR wire; the relay is AetherSDR's own radio-side app.
class GpsdSkyClient : public QObject {
    Q_OBJECT

public:
    enum class State {
        Stopped,
        Connecting,
        Streaming,     // connected; satellites arrive with each SKY
        Unavailable,   // nothing answered, or gpsd isn't running; retried
    };
    Q_ENUM(State)

    explicit GpsdSkyClient(QObject* parent = nullptr);

    // Connects, and reconnects every 10 s while started. A new address
    // drops the session in flight.
    void start(const QHostAddress& address);
    void stop();
    State state() const { return m_state; }
    // gpsd's release, from this session's VERSION; empty until it arrives.
    QString gpsdRelease() const { return m_release; }
    // Whether this session reached the relay: an Unavailable after that is
    // the relay's or gpsd's own trouble, not a missing waveform.
    bool relayReached() const { return m_reached; }

signals:
    // The merged sky view, by PRN. gpsd can split one epoch's sky across
    // reports (a FLEX-8600 alternates GPS+GLONASS with GPS+GLONASS+Galileo),
    // so a satellite stays until it is missing from 4 reports in a row.
    void skyUpdated(const QList<AetherSDR::GpsSatellite>& satellites);
    void detailUpdated(const AetherSDR::GpsdDetail& detail);
    // `detail` says why when Unavailable.
    void stateChanged(AetherSDR::GpsdSkyClient::State state, const QString& detail);
    // Connected, but no full SKY for 30 s: what was shown is no longer live.
    void skyStale();

protected:
    // One chunk of the stream, as the socket delivered it. A line longer
    // than 64 KiB is dropped whole, however the reads split it.
    void ingest(const QByteArray& chunk);
    // Bytes held waiting for a newline; never more than the line bound.
    qsizetype pendingBytes() const { return m_buffer.size(); }

private:
    void openSession();
    void closeSession(const QString& reason);
    void setState(State state, const QString& detail = {});
    void handleLine(const QByteArray& line);

    QHostAddress m_address;
    QPointer<QTcpSocket> m_socket;
    QTimer* m_retry{nullptr};
    QTimer* m_stale{nullptr};
    QByteArray m_buffer;
    bool m_overflow{false};   // discarding up to the next newline
    QString m_release;
    struct Tracked {
        GpsSatellite satellite;
        int missed{0};
    };
    QMap<int, Tracked> m_sky;   // by PRN
    GpsdDetail m_detail;
    bool m_reached{false};
    State m_state{State::Stopped};
};

}  // namespace AetherSDR
