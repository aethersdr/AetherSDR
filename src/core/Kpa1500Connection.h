#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>

#include <functional>
#include <optional>
#include <vector>

#include "Kpa1500Protocol.h"

namespace AetherSDR {

// Peripheral transport for an Elecraft KPA1500 amplifier (#4097).
//
// A standalone Ethernet device with no FlexRadio awareness at all — the
// Flex radio neither discovers it nor proxies its telemetry the way it does
// for a PGXL/TGXL — so this is a peripheral(kpa1500) accessory alongside
// AcomConnection/SpeConnection/VkampConnection, not an IRadioBackend
// implementor. See docs/architecture/kpa1500-amplifier-design.md.
//
// Two transports to the amp's command port (default 1500), selected with
// setTransport() before connectNetwork():
//
//   TCP — a stream connection; link state is the socket's own state.
//   UDP — one `^CMD;` frame per datagram to the amp's UDP server, which
//         accepts the identical command set. There is no connection, so the
//         link is declared up on the first well-formed reply and down after
//         kUdpSilenceMs without one. Because a datagram can be lost, every
//         control command is confirmed by readback: it is resent with a
//         query until the amp reports the requested state, and after
//         kMaxControlAttempts without that, controlUnconfirmed() fires.
//         Network keying is refused on UDP (see key()).
//
// The amp does not broadcast spontaneously: every reading below arrives
// because this class asked for it. m_pollTimer walks the query set; see
// fastPollCommands()/slowPollCommands() for why it is split in two.
// Design note: docs/architecture/kpa1500-amplifier-design.md §3.2.
class Kpa1500Connection : public QObject {
    Q_OBJECT

public:
    explicit Kpa1500Connection(QObject* parent = nullptr);
    ~Kpa1500Connection() override;

    bool isConnected() const { return m_connected; }
    QString description() const;  // "192.168.1.60:1500 (UDP)", for status display

    // Takes effect on the next connectNetwork(), including auto-reconnects.
    void setTransport(Kpa1500::Transport transport) { m_transport = transport; }
    Kpa1500::Transport transport() const { return m_transport; }

    void connectNetwork(const QString& host, quint16 port = Kpa1500::kDefaultPort);
    void disconnect();
    void setAutoReconnect(bool on) { m_autoReconnect = on; }

    // Control (host -> amp). Every one is a no-op, logged, when not
    // connected — the amp is the authority on its own state, so nothing
    // here latches an optimistic value; the applet repaints when the poll
    // reply lands (constitution Principle II applied to a peripheral).
    void setOperate(bool operate);
    void startTune();   // the exciter must supply RF for the tune to finish
    void cancelTune();
    void setAtuInline(bool inLine);
    void selectAntenna(int port);  // ANT1/ANT2; out of range is a no-op, logged
    void clearFault();
    void setFanMinimum(int level);  // 0-5; out of range is a no-op, logged
    void setPower(bool on);         // ^ON1 wakes a sleeping amp; ^ON0 puts it to sleep

    // ── TX frequency tracking (^FR) ──────────────────────────────────────
    //
    // Feeds the amp the TX slice's frequency so it changes band and recalls
    // ATU memories without a transmission. Rapid tuning is coalesced to one
    // ^FR per kTrackIntervalMs, and nothing is sent while `transmitting`:
    // a band change under RF would switch the amp's relays hot. A held
    // frequency goes out as soon as transmit ends. `khz` empty means there
    // is no TX slice; the last frequency sent stays on the amp.
    void setFollowTx(bool on);
    bool followTx() const { return m_followTx; }
    void setTxFrequency(std::optional<int> khz, bool transmitting);

    // ── Network keying ───────────────────────────────────────────────────
    //
    // NOT wired to any transmit path in this revision, and deliberately so:
    // whether Ethernet keying is trusted as the SOLE keying path (vs.
    // remaining paired with the hardware KEY IN line, which runs in
    // parallel with `^TX` rather than instead of it) is an open maintainer
    // decision on #4097, and it is the decision that governs an emission.
    // Constitution Principle VI says a path that can transmit fails closed
    // when intent is not unambiguous; nothing in this build calls key(), so
    // the fail-closed state is the shipped state. These exist so the
    // mechanism the maintainer has to rule on is reviewable code with tests
    // rather than a description.
    //
    // key() is refused on UDP: a lost refresh datagram would drop the key
    // mid-transmission, and a lost `^RX` would leave release to the timeout.
    //
    // key() never emits the unbounded `^TX;`. It sends `^TX<kKeyTimeoutSec>;`
    // and refreshes it every kKeyRefreshMs for as long as the key is held,
    // so if this application dies or the LAN drops the amp unkeys itself
    // when the timeout expires instead of staying keyed indefinitely.
    //
    // While keyed, every poll also sends `^TQ;`; a reply of 0 means the amp
    // has already dropped the key, and the refresh stops.
    void key();
    void unkey();

signals:
    void connected();
    void disconnected();
    void connectionFailed(const QString& errorString);
    // Full current snapshot, emitted when a reply actually changed
    // something. Consumers read the fields they care about; unset optionals
    // mean "the amp has not reported this yet", not zero.
    void statusUpdated(const AetherSDR::Kpa1500::Status& status);
    // UDP only: the amp never reported the state a control command asked
    // for. `control` is a short user-facing name such as "Operate".
    void controlUnconfirmed(const QString& control);

private slots:
    void onReadyRead();
    void onUdpReadyRead();

private:
    void onTransportUp();
    void onTransportDown();
    void onTransportError(const QString& errorString);
    void onMessage(const Kpa1500::Message& message);
    void poll();
    bool sendRaw(const QByteArray& frame);   // control path: requires a live link
    bool writeFrame(const QByteArray& frame);  // any open transport, incl. UDP probing
    bool linkOpen() const;
    void armReconnect();
    void stopKeyRefresh();

    // UDP control confirmation. `satisfied` is checked against every reply
    // as it lands; a pending entry with the same `group` is replaced, so the
    // operator's latest request is the one being confirmed.
    struct PendingControl {
        QString group;
        QString label;
        QByteArray frame;
        QString readback;
        bool resend{true};
        int attempts{0};
        QElapsedTimer lastSent;
        std::function<bool(const Kpa1500::Message&, const Kpa1500::Status&)> satisfied;
    };
    void sendControl(PendingControl control);
    void checkPendingControls(const Kpa1500::Message& message);
    void retryPendingControls();
    void scheduleTrack();
    void sendTrack();

    QTcpSocket m_socket;
    QUdpSocket m_udp;
    Kpa1500::Transport m_transport{Kpa1500::Transport::Tcp};
    // Transport of the current/last attempt; m_transport may change under it.
    Kpa1500::Transport m_activeTransport{Kpa1500::Transport::Tcp};
    bool m_udpOpen{false};
    QString m_udpLastError;
    QElapsedTimer m_lastRx;
    std::vector<PendingControl> m_pending;

    bool m_followTx{false};
    bool m_transmitting{false};
    std::optional<int> m_trackWantKhz;
    std::optional<int> m_trackSentKhz;
    QElapsedTimer m_trackLastSent;
    QTimer m_trackTimer;
    static constexpr int kTrackIntervalMs = 300;
    Kpa1500::MessageParser m_parser;
    Kpa1500::Status m_status;

    QString m_lastHost;
    quint16 m_lastPort{Kpa1500::kDefaultPort};

    bool m_connected{false};
    bool m_autoReconnect{false};
    bool m_deliberateDisconnect{false};
    bool m_keyed{false};

    // Logs a fault once when it appears rather than on every poll — `^FL`
    // answers with the same code for as long as the fault stands.
    int m_lastFaultCode{0};

    QTimer m_reconnectTimer;
    static constexpr int kReconnectIntervalMs = 5000;

    // An unreachable IP otherwise falls back to the OS TCP connect timeout
    // (~21s on Windows) before connectionFailed() ever fires, which reads
    // as a hung dialog rather than a slow one. Same epoch-guarded
    // singleShot pattern as VkampConnection/DxClusterClient (#2380: a stale
    // timeout from attempt N must never abort a later attempt that already
    // succeeded).
    static constexpr int kConnectTimeoutMs = 8000;
    int m_connectEpoch{0};

    QTimer m_pollTimer;
    static constexpr int kPollIntervalMs = 500;
    // UDP has no head-of-line blocking, so the meters poll twice as fast.
    static constexpr int kUdpPollIntervalMs = 250;
    static constexpr int kUdpSilenceMs = 3000;
    static constexpr int kMaxDatagramBytes = 1024;
    static constexpr int kControlRetryMs = 600;
    static constexpr int kMaxControlAttempts = 5;
    // Rotates one slow query per tick alongside the fast set, so the meters
    // stay at the full poll rate while the configuration readbacks (band,
    // antenna, ATU, operate state) still refresh a couple of times a
    // second without putting the whole query set on the wire every 500ms.
    int m_slowPollIndex{0};

    // Keying refresh. kKeyRefreshMs is well under kKeyTimeoutSec so a
    // single dropped refresh does not unkey mid-transmission, while a dead
    // application still releases the amp within kKeyTimeoutSec.
    QTimer m_keyRefreshTimer;
    static constexpr int kKeyTimeoutSec = 10;
    static constexpr int kKeyRefreshMs = 3000;
};

}  // namespace AetherSDR
