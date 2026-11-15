#pragma once

#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVector>

#include <optional>

class QNetworkReply;

namespace AetherSDR {

// TCP port of the shim's link-telemetry endpoint, served on the tailnet only
// (tools/flex-tailnet-shim/telemetry.go: TelemetryPort).
inline constexpr quint16 kTailnetShimTelemetryPort = 48993;

// One relay session's counters, as datagrams leave the radio, before the
// tunnel. A computer can hold several sessions (two AetherSDR instances, or
// AetherSDR and SmartSDR), so each keeps its identity.
struct TailnetRelaySession {
    QString clientUdp;          // "100.x.y.z:port" the radio streams to
    quint16 clientUdpPort{0};   // the client's local VITA-49 port
    quint64 radioPackets{0};
    quint64 radioBreaks{0};     // sequence discontinuities
    quint64 radioGaps{0};       // missed packets
    quint64 sendFailures{0};    // datagrams the shim could not send to the client
};

// What GET /v1/session returns: the radio side's view of this client's
// tailnet link. The shim answers each caller with only its own sessions.
struct TailnetSessionReport {
    QString version;
    QString path;          // "direct", "peer-relay", "relay" or "unknown"
    QString endpoint;      // the client's direct address, when direct
    QString relay;         // DERP region code
    double rttMs{-1.0};    // -1 until the shim's disco ping succeeds
    QString rttVia;
    double rttAgeS{0.0};
    int pathChanges{0};
    double pathSinceS{0.0};
    double toClientKbps{0.0};
    double fromClientKbps{0.0};
    double lastHandshakeS{0.0};
    double shimCpuPct{0.0};
    qint64 shimRssKb{0};
    int mtuClamp{0};
    QVector<TailnetRelaySession> sessions;

    // The session this AetherSDR is streaming on, matched by its local
    // VITA-49 port; null when none matches (not streaming yet, or the
    // report is for another of this computer's clients).
    const TailnetRelaySession* sessionForUdpPort(quint16 port) const;
};

namespace tailnetshim {
std::optional<TailnetSessionReport> parseSessionReport(const QByteArray& json);

// The port of an "address:port" endpoint ("100.64.1.2:4993",
// "[fd7a::1]:4993"); 0 when there is none ("Not bound").
quint16 portOfEndpoint(const QString& endpoint);

// Sequence breaks per 100 packets the tunnel added: what the client saw over
// a window, less what the radio had already lost before the tunnel. Both
// counts are breaks, the unit AetherSDR's stream counters use. Negative
// windows (counter resets, clock skew between the two sides) read as 0.
double tunnelBreakPercent(qint64 clientPackets, qint64 clientBreaks,
                          qint64 radioPackets, qint64 radioBreaks);
}  // namespace tailnetshim

// Polls the shim's /v1/session every 2 s while the radio is reached over the
// tailnet. Anything else (a LAN radio, SmartLink, no radio) leaves it idle.
class TailnetLinkTelemetry : public QObject {
    Q_OBJECT
public:
    explicit TailnetLinkTelemetry(QObject* parent = nullptr);

    // A null address, or one outside the tailnet, stops polling and clears
    // the last report.
    void setRadioAddress(const QHostAddress& address);

    // The last report, if one arrived within the staleness window: frozen
    // numbers from a shim that stopped answering must not read as live.
    std::optional<TailnetSessionReport> current() const;
    // Increments with every report received, so callers can tell a new
    // report from the same one read twice.
    quint64 reportSerial() const { return m_serial; }
    bool isPolling() const { return m_timer.isActive(); }

private:
    void poll();

    QNetworkAccessManager m_nam;
    QTimer m_timer;
    QHostAddress m_address;
    QPointer<QNetworkReply> m_reply;
    std::optional<TailnetSessionReport> m_last;
    qint64 m_lastOkMs{0};
    quint64 m_serial{0};
};

}  // namespace AetherSDR
