#pragma once

#include <QHostAddress>
#include <QJsonObject>
#include <QList>
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
    void skyUpdated(const QList<AetherSDR::GpsSatellite>& satellites);
    // `detail` says why when Unavailable.
    void stateChanged(AetherSDR::GpsdSkyClient::State state, const QString& detail);
    // Connected, but no full SKY for 30 s: what was shown is no longer live.
    void skyStale();

protected:
    // One chunk of the stream, as the socket delivered it.
    void ingest(const QByteArray& chunk);

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
    bool m_reached{false};
    State m_state{State::Stopped};
};

}  // namespace AetherSDR
