#pragma once

#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <optional>

class QNetworkReply;

namespace AetherSDR {

// TCP port of the shim's link-telemetry endpoint, served on the tailnet only
// (tools/flex-tailnet-shim/telemetry.go: TelemetryPort).
inline constexpr quint16 kTailnetShimTelemetryPort = 48993;

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
    int sessions{0};
    // Summed over this client's sessions and streams, counted as datagrams
    // leave the radio, before the tunnel.
    quint64 radioPackets{0};
    quint64 radioBreaks{0};     // sequence discontinuities
    quint64 radioGaps{0};       // missed packets
    quint64 sendFailures{0};    // datagrams the shim could not send to the client
};

namespace tailnetshim {
std::optional<TailnetSessionReport> parseSessionReport(const QByteArray& json);

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
