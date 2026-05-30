#pragma once

#include "core/tnc/Ax25.h"

#include <QByteArray>
#include <QObject>
#include <QString>

class QTimer;

namespace AetherSDR {

// A single-connection AX.25 v2.0 connected-mode (LAPB) data-link state machine,
// mod-8 sequence space. It handles exactly one peer at a time, which is all the
// Personal Mailbox System needs (one simultaneous caller).
//
// Responsibilities:
//  - Accept an inbound SABM (connect request) and reply UA.
//  - Track V(S)/V(R)/V(A), acknowledge received I-frames with RR.
//  - Segment outbound application data into I-frames (<= paclen) and retransmit
//    unacknowledged I-frames on the T1 timeout, up to N2 retries.
//  - Honour RR/RNR/REJ, poll/final, and tear down on DISC or N2 exhaustion.
//
// It is transport-agnostic: it consumes already-decoded ax25::Frame objects and
// emits raw frames (address..info, no FCS) for the caller to key on the air via
// AetherAx25LibmodemShim::buildTransmitAudioFromFrame(). Timers run on the
// owning (GUI) thread. This class is reusable by the future AX.25 node/digipeater.
class Ax25Connection : public QObject {
    Q_OBJECT

public:
    enum class State {
        Disconnected, // no peer
        Connected,    // information transfer
        Disconnecting // DISC sent, awaiting UA
    };

    explicit Ax25Connection(QObject* parent = nullptr);
    ~Ax25Connection() override;

    // Our own address (the primary callsign-SSID we answer to). When idle this
    // also resets the active session address.
    void setLocalAddress(const ax25::Address& local);
    // An optional secondary "vanity" address we also answer to (e.g. AETHBBS).
    // Pass an invalid Address to clear it.
    void setAliasAddress(const ax25::Address& alias) { m_alias = alias; }
    // The address currently in use for this session — the one the caller dialed
    // (primary or alias). Equals the primary when idle.
    ax25::Address localAddress() const { return m_local; }
    ax25::Address remoteAddress() const { return m_remote; }

    State state() const { return m_state; }
    bool isConnected() const { return m_state == State::Connected; }

    // Tunables. Defaults are sized for 1200-baud VHF FM with PTT overhead.
    void setPaclen(int bytes) { m_paclen = qBound(16, bytes, 256); }
    void setMaxRetries(int n2) { m_n2 = qBound(1, n2, 20); }
    void setRetryTimeoutMs(int t1) { m_t1Ms = qBound(1000, t1, 60000); }
    // Window k: max unacknowledged I-frames in flight (mod-8 caps it at 7).
    // Default 1 (MAXFRAME=1). On a HALF-DUPLEX radio link each I-frame is its own
    // PTT keyup; sending several back-to-back keeps us transmitting (and deaf)
    // long enough that the peer's acknowledgement lands while we cannot hear it,
    // which stalls into a T1 retransmit loop. k=1 sends one frame, then listens
    // for its ack before the next — the pattern that works reliably here. A
    // future single-keyup multi-frame TX path (or a full-duplex transport) can
    // safely raise this.
    void setWindow(int k) { m_window = qBound(1, k, 7); }

    // Feed every decoded frame here. Frames not addressed to our local address
    // (dest mismatch) are ignored, so the caller can pass all RX traffic.
    void onFrameReceived(const ax25::Frame& frame);

    // Queue application data to send to the connected peer. No-op if not
    // connected. Data is buffered and segmented into I-frames automatically.
    void sendData(const QByteArray& data);

    // Initiate a graceful disconnect (sends DISC).
    void disconnect();

    // Drop the link immediately without sending anything (e.g. on shutdown).
    void reset();

signals:
    // A raw AX.25 frame (address..info, no FCS) is ready to transmit.
    void sendFrame(const QByteArray& rawNoFcs);

    // Connection established with the given peer.
    void connected(const ax25::Address& peer);

    // Connection torn down. `byPeer` is true when the peer initiated (DISC) or
    // the link failed (N2 exhausted); false for a locally requested disconnect.
    void disconnected(const ax25::Address& peer, bool byPeer);

    // Reassembled application data received from the peer (I-frame info fields).
    void dataReceived(const QByteArray& data);

    // Human-readable protocol activity for logging.
    void activity(const QString& message);

private:
    void enterConnected(const ax25::Address& peer);
    void enterDisconnected(bool byPeer);
    void transmit(const ax25::Frame& frame);
    void sendUFrame(ax25::FrameType type, bool pollFinal, bool command);
    void sendSupervisory(ax25::FrameType type, bool pollFinal, bool command);
    void pumpOutbound();             // segment send buffer -> I-frames
    void ackUpTo(int nr);            // slide window per received N(R)
    void retransmitUnacked();        // T1 expiry
    void startT1();
    void stopT1();
    void onT1Timeout();
    int outstanding() const;         // unacked I-frames in flight

    ax25::Address m_primary; // configured primary listen address
    ax25::Address m_alias;   // optional configured vanity/alias address
    ax25::Address m_local;   // active session address (the one the caller dialed)
    ax25::Address m_remote;
    State m_state{State::Disconnected};

    int m_vs{0}; // V(S) next send sequence
    int m_vr{0}; // V(R) next expected receive sequence
    int m_va{0}; // V(A) last acknowledged send sequence

    int m_window{1}; // k: max outstanding I-frames; see setWindow() (half-duplex)
    int m_paclen{128};
    int m_n2{8};
    int m_t1Ms{6000};
    int m_retryCount{0};
    bool m_peerBusy{false}; // peer sent RNR

    QByteArray m_sendBuffer;            // app data awaiting segmentation
    QByteArray m_sentIFrames[8];        // by N(S), for retransmission
    bool m_iFrameValid[8]{};            // slot occupied
    QTimer* m_t1{nullptr};
};

} // namespace AetherSDR
