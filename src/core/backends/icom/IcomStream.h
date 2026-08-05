#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QMap>

#include <deque>
#include <QObject>

#include <cstdint>
#include <span>
#include <vector>

#include "core/backends/icom/IcomProtocol.h"

class QTimer;
class QUdpSocket;

namespace AetherSDR::icom {

// ONE RS-BA1 UDP stream: the session handshake, the three keepalive clocks, and
// the retransmission layer.
//
// Three of these exist per connection — control, serial and audio — each with
// its OWN session handshake, its own sequence space and its own retransmission
// state. That looks like triplication and it is what the protocol actually is;
// collapsing them would mean inventing a multiplexing layer the radio does not
// speak.
//
// Lives on IcomSession's I/O thread, not the GUI thread. A QUdpSocket takes the
// affinity of the thread that creates it, so start() must run there.
class IcomStream : public QObject {
    Q_OBJECT

public:
    // Which stream this is. It changes real behaviour, not just a label:
    //
    //   * Control delivers payloads IMMEDIATELY, with no reorder buffer. Its
    //     traffic is a request/response handshake where a stale retransmitted
    //     login is worse than a dropped one, and both reference implementations
    //     treat it this way.
    //   * Serial and Audio run the full reorder + retransmit path, because
    //     their payloads are a STREAM where order is meaning.
    enum class Role { Control, Serial, Audio };

    struct Config {
        QHostAddress host;
        quint16 remotePort = 0;
        // 0 means "let the OS choose". The chosen port is announced to the radio
        // in the control stream's request, so it does not need to be fixed —
        // and binding a fixed one needlessly fails when a stale session still
        // holds it.
        quint16 localPort = 0;
        Role role = Role::Control;
    };

    explicit IcomStream(QObject* parent = nullptr);
    ~IcomStream() override;

    // Bind, run the handshake, start the keepalives. Emits ready() on success
    // or failed() on timeout. Equivalent to bind() followed by beginHandshake().
    Q_INVOKABLE bool start(const AetherSDR::icom::IcomStream::Config& config);

    // Bind the socket WITHOUT starting the handshake.
    //
    // These two are separable because of an ordering constraint in the protocol:
    // the control stream's request has to ANNOUNCE the local ports the serial
    // and audio streams will use, and it is sent before either of them may
    // handshake. Binding early is how we learn those ports honestly instead of
    // hardcoding 50002/50003 and failing whenever a stale session still holds
    // one.
    Q_INVOKABLE bool bindOnly(const AetherSDR::icom::IcomStream::Config& config);
    Q_INVOKABLE void beginHandshake();

    Q_INVOKABLE void stop();

    [[nodiscard]] bool isReady() const noexcept { return m_ready; }
    [[nodiscard]] quint32 localSessionId() const noexcept { return m_localSid; }
    [[nodiscard]] quint32 remoteSessionId() const noexcept { return m_remoteSid; }
    [[nodiscard]] quint16 localPort() const noexcept { return m_boundPort; }

    // Send a packet the radio may later ask us to replay.
    //
    // The caller builds the packet with a placeholder header sequence; THIS
    // method stamps the real one and retains a copy. Sequence allocation has to
    // live here because the retention buffer is keyed by it, and a caller that
    // allocated its own would be able to send a sequence this stream cannot
    // replay — which presents as a retransmit request that can never be
    // satisfied and a stream that stalls behind it.
    void sendTracked(std::vector<std::uint8_t> packet);

    // Send without retention (handshake, pings, retransmit requests).
    void sendRaw(std::span<const std::uint8_t> packet);
    // Handshake packets go TWICE — the IC-705's WiFi drops the first packet of
    // a burst often enough that both reference implementations do this
    // unconditionally.
    void sendRawTwice(std::span<const std::uint8_t> packet);
    // Push whatever sendRaw() left in the socket's write buffer onto the wire.
    void flush();

    struct Counters {
        quint64 rxBytes = 0;
        quint64 txBytes = 0;
        quint64 rxPackets = 0;
        quint64 rxLost = 0;          // gaps retransmission could not repair
        quint64 retransmitsAsked = 0;
        quint64 retransmitsServed = 0;
        int rttMs = -1;              // negative == not measured
    };
    [[nodiscard]] Counters counters() const { return m_counters; }

signals:
    // Handshake complete; the stream will now carry payload.
    void ready();
    void failed(const QString& reason);
    // One in-order payload datagram. For Serial and Audio this has been through
    // the reorder buffer; for Control it is immediate.
    void payloadReady(const QByteArray& packet);
    // Packets the retransmitter could not recover. Audio uses this to insert
    // concealment rather than silently splicing across the hole.
    void packetsLost(int count);

private slots:
    void onReadyRead();
    void onIdleTick();
    void onPingTick();
    void onReorderTick();

private:
    void handleDatagram(const QByteArray& datagram);
    void handleRetransmitRequest(std::span<const std::uint8_t> pkt);
    void deliver(const QByteArray& packet);
    void queueForReorder(quint16 seq, const QByteArray& packet);
    void drainReorder();
    void retain(quint16 seq, const std::vector<std::uint8_t>& packet);

public:
    // The sequences currently held for retransmit, oldest first.
    //
    // Public for the eviction test, and worth having anyway: the failure this
    // guards against — evicting the newest packets after a sequence wrap — is
    // invisible from outside until a retransmit request quietly returns an Idle
    // instead of a payload, roughly every 11 minutes of transmit.
    [[nodiscard]] QList<quint16> retainedSequences() const
    {
        return QList<quint16>(m_replayOrder.begin(), m_replayOrder.end());
    }
    // Test seam for the same reason: retain() is where the wrap bug lived.
    void retainForTest(quint16 seq, const std::vector<std::uint8_t>& packet)
    {
        retain(seq, packet);
    }

private:
    // isPayload false for keepalives: they are tracked (they consume a sequence
    // number and can be asked for) but must not reset the quiet clock, or the
    // idle cadence never relaxes.
    void sendTrackedImpl(std::vector<std::uint8_t> packet, bool isPayload);

    QUdpSocket* m_socket = nullptr;
    QTimer* m_idleTimer = nullptr;
    QTimer* m_pingTimer = nullptr;
    QTimer* m_reorderTimer = nullptr;

    Config m_config;
    quint32 m_localSid = 0;
    quint32 m_remoteSid = 0;
    quint16 m_boundPort = 0;
    bool m_ready = false;
    bool m_gotRemoteSid = false;

    quint16 m_txSeq = 0;       // header sequence for tracked packets
    quint16 m_pingSeq = 0;

    // Replay buffer for packets the radio may ask us to resend. Bounded: past
    // a couple of hundred packets a retransmit request is archaeology, and an
    // unbounded map on the audio stream is an unbounded memory leak.
    QMap<quint16, std::vector<std::uint8_t>> m_replay;
    // Insertion order for m_replay, because the map is keyed by SEQUENCE and
    // the sequence space wraps. Evicting the map's lowest key drops the newest
    // packets once the counter rolls past 0xFFFF — see retain().
    std::deque<quint16> m_replayOrder;
    static constexpr int kReplayDepth = 256;

    // Reorder buffer (Serial and Audio only).
    QMap<quint16, QByteArray> m_reorder;
    quint16 m_lastDelivered = 0;
    bool m_haveDelivered = false;
    QElapsedTimer m_headWait;      // how long the head of a gap has been waiting
    bool m_gapPending = false;

    QElapsedTimer m_pingSentAt;
    QElapsedTimer m_idleSince;     // time since we last sent anything tracked
    Counters m_counters;
};

}  // namespace AetherSDR::icom

Q_DECLARE_METATYPE(AetherSDR::icom::IcomStream::Config)
Q_DECLARE_METATYPE(AetherSDR::icom::IcomStream::Counters)
