#pragma once

#include "core/tnc/Ax25.h"
#include "core/tnc/Ax25LinkTiming.h"

#include <QByteArray>
#include <QElapsedTimer>
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
        Connecting,   // SABM sent, awaiting UA (outbound connect)
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

    // Live data-link counters for status display.
    int sendSeq() const { return m_vs; }      // V(S)
    int recvSeq() const { return m_vr; }       // V(R)
    int retries() const { return m_retryCount; }
    int maxRetries() const { return m_n2; }
    int unacked() const { return outstanding(); } // I-frames in flight
    int sendQueueBytes() const { return int(m_sendBuffer.size()); } // unsent data

    // Per-session telemetry (reset each time a link comes up).
    struct Stats {
        quint32 iSent{0};        // new I-frames transmitted
        quint32 iResent{0};      // I-frame retransmissions (T1 / REJ recovery)
        quint32 iRcvd{0};        // in-sequence I-frames accepted
        quint32 iDropped{0};     // out-of-sequence I-frames discarded (real gap)
        quint32 iDuplicate{0};   // already-received I-frames re-acked (our ack was lost)
        quint32 rrRcvd{0};       // RR received from peer
        quint32 rnrRcvd{0};      // RNR (peer busy) received
        quint32 rejRcvd{0};      // REJ received from peer
        quint32 rejSent{0};      // REJ we sent
        quint32 t1Timeouts{0};   // T1 expiries (no ack in time)
        quint32 t2Acks{0};       // deferred (T2) acknowledgements sent
        quint32 t3Polls{0};      // idle-link (T3) checkpoint polls sent
        quint32 rejRecoveries{0}; // REJ-triggered retransmit rounds this session
        quint32 frmrRcvd{0};     // FRMR (frame reject) from peer
        quint32 invalidNr{0};    // out-of-range N(R) ignored
        quint64 infoBytesSent{0};     // I-frame payload bytes out (excludes resends)
        quint64 infoBytesReceived{0}; // I-frame payload bytes in (in-sequence only)

        // Measured round-trip time: first transmission of an I-frame to the ack
        // that covers it. Retransmitted frames are EXCLUDED (Karn's algorithm) —
        // once a frame has gone out twice there is no way to know which copy the
        // ack answers, and counting it would bias the estimate toward T1 itself.
        // This is the number that says whether T1 is sized correctly; before it
        // existed there was no way to tell a mis-sized timer from a bad channel.
        quint32 rttSamples{0};
        qint64 rttLastMs{0};
        qint64 rttMinMs{0};
        qint64 rttMaxMs{0};
        qint64 rttSumMs{0};

        qint64 averageRttMs() const
        {
            return rttSamples > 0 ? rttSumMs / static_cast<qint64>(rttSamples) : 0;
        }
    };
    const Stats& stats() const { return m_stats; }

    // Milliseconds since the current session came up (0 when idle).
    qint64 sessionDurationMs() const;
    // One-line human-readable session digest for logs and the STATUS command.
    QString sessionSummary() const;

    // ---- Link timing -------------------------------------------------------
    //
    // The air interface this link runs over: baud rate plus the framing the
    // modulator actually transmits. Everything below can be derived from it.
    // Setting a profile does NOT change the timers on its own — call
    // applyRecommendedTimers() for that, so an operator override survives.
    void setLinkProfile(const ax25::LinkTimingProfile& profile) { m_profile = profile; }
    const ax25::LinkTimingProfile& linkProfile() const { return m_profile; }

    // Size T1/T2/T3 from the current profile and paclen. This is the intended
    // way to configure the link: the shipped VHF-derived constants were shorter
    // than a single HF transmission, so every HF I-frame timed out before it had
    // even finished going out. See Ax25LinkTiming.h and docs/HFMODEM.md §1.
    void applyRecommendedTimers();
    // The T1 this profile/paclen implies, without applying it (for UI display).
    int recommendedRetryTimeoutMs() const { return ax25::recommendedT1Ms(m_profile, m_paclen); }
    // Modelled airtime of one full-size I-frame, for UI display and logging.
    int expectedIFrameAirtimeMs() const { return ax25::frameAirtimeMs(m_profile, m_paclen); }
    // Modelled minimum round trip: our frame out, peer turnaround, ack back.
    int expectedRoundTripMs() const { return ax25::roundTripMs(m_profile, m_paclen); }

    // Tunables. Prefer applyRecommendedTimers() over hand-setting T1.
    void setPaclen(int bytes) { m_paclen = qBound(16, bytes, 256); }
    void setMaxRetries(int n2) { m_n2 = qBound(1, n2, 20); }
    void setRetryTimeoutMs(int t1) { m_t1Ms = qBound(1000, t1, 60000); }
    int retryTimeoutMs() const { return m_t1Ms; }
    int paclen() const { return m_paclen; }

    // Idle-link poll timer T3 (ms). A connected link with nothing outstanding
    // has T1 stopped, so without T3 a peer that simply vanishes leaves us
    // "Connected" forever: the terminal shows a link that is not there, and the
    // single-caller mailbox is locked out permanently because every other
    // caller's SABM is answered with DM. On expiry we send one RR(P=1)
    // checkpoint and let the normal T1/N2 machinery declare the link dead.
    // The 1 s floor is well below anything an operator would choose (T3 is
    // derived as 10x T1, never hand-set); it exists so the idle-poll behaviour
    // can be tested without a minute-long test.
    void setIdlePollMs(int t3) { m_t3Ms = qBound(1000, t3, 600000); }
    int idlePollMs() const { return m_t3Ms; }
    // Whether the idle-link poll is currently counting down. True exactly when
    // the link is connected with nothing outstanding.
    bool idlePollArmed() const;
    // Acknowledgement-delay timer T2 (ms). On a half-duplex link we must NOT key
    // up to acknowledge an in-sequence I-frame while the peer is still mid-burst
    // sending the rest of a window — doing so makes us deaf to the remaining
    // frames and stalls multi-frame replies (a long BBS help menu, say). Instead
    // we defer the RR ack: a polled frame (P=1, peer is now listening) is acked
    // at once, an unpolled one starts T2 and the coalesced RR is sent when the
    // burst goes idle. Must be < T1.
    void setAckDelayMs(int t2) { m_t2Ms = qBound(200, t2, 10000); }

    // Window k: max unacknowledged I-frames in flight (mod-8 caps it at 7).
    // Default 1 (MAXFRAME=1). On a HALF-DUPLEX radio link each I-frame is its own
    // PTT keyup; sending several back-to-back keeps us transmitting (and deaf)
    // long enough that the peer's acknowledgement lands while we cannot hear it,
    // which stalls into a T1 retransmit loop. k=1 sends one frame, then listens
    // for its ack before the next — the pattern that works reliably here. A
    // future single-keyup multi-frame TX path (or a full-duplex transport) can
    // safely raise this.
    void setWindow(int k) { m_window = qBound(1, k, 7); }

    // Initiate an outbound connect to `peer` (the terminal "client" role): send
    // SABM and await UA, retransmitting on T1 up to N2 before giving up. No-op
    // unless idle. On success emits connected(); on refusal (DM) or N2 exhaustion
    // emits connectFailed() then disconnected().
    //
    // `via` is an optional digipeater path (0-8 hops, e.g. WIDE1-1). When set,
    // every outbound frame in the session carries it so the digipeater(s) repeat
    // us to the peer; returning frames are matched on dest regardless of path.
    void connectTo(const ax25::Address& peer,
                   const QVector<ax25::Address>& via = {});
    // The digipeater path of the current/last session (empty for a direct link).
    QVector<ax25::Address> viaPath() const { return m_via; }

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

    // An outbound connectTo() attempt failed before reaching Connected — the peer
    // refused (DM) or never answered (N2 exhausted). `reason` is human-readable.
    // disconnected() still follows so callers that only watch that can rely on it.
    void connectFailed(const ax25::Address& peer, const QString& reason);

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
    // Attach the digipeater path, encode, key it on the air, and return the raw
    // bytes (so I-frames can be stored for retransmission exactly as sent).
    QByteArray transmit(ax25::Frame frame);
    void sendUFrame(ax25::FrameType type, bool pollFinal, bool command);
    void sendSupervisory(ax25::FrameType type, bool pollFinal, bool command);
    void pumpOutbound();             // segment send buffer -> I-frames
    void ackUpTo(int nr);            // slide window per received N(R)
    void retransmitUnacked();        // T1 expiry
    void startT1();
    void stopT1();
    void onT1Timeout();
    void startT2();                  // (re)arm the deferred-ack timer
    void stopT2();
    void onT2Timeout();              // burst idle -> send the coalesced RR ack
    void stopT3();
    void armT3IfIdle();              // connected + nothing in flight -> arm T3
    void onT3Timeout();              // idle link -> one RR(P=1) checkpoint
    void sendAck(bool pollFinal);    // RR (or piggyback) for everything received
    int outstanding() const;         // unacked I-frames in flight
    // Clear the retry counter because the link demonstrably made progress. NOT
    // the same as "a frame arrived": a peer repeating a stale RR proves nothing
    // and must not be allowed to hold us below N2 forever.
    void noteLinkProgress();
    // True when N(S) names a frame we have already accepted — the peer's copy
    // of our acknowledgement was lost. Distinct from a sequence gap, and it
    // must be answered rather than silently dropped.
    bool isDuplicateIFrame(int ns) const;
    void recordRttSample(int seq);   // Karn-safe RTT capture on ack
    void logSessionOpen();
    void logSessionClose(bool byPeer);

    ax25::Address m_primary; // configured primary listen address
    ax25::Address m_alias;   // optional configured vanity/alias address
    ax25::Address m_local;   // active session address (the one the caller dialed)
    ax25::Address m_remote;
    QVector<ax25::Address> m_via; // digipeater path for outbound frames (H=0)
    State m_state{State::Disconnected};

    int m_vs{0}; // V(S) next send sequence
    int m_vr{0}; // V(R) next expected receive sequence
    int m_va{0}; // V(A) last acknowledged send sequence

    int m_window{1}; // k: max outstanding I-frames; see setWindow() (half-duplex)
    int m_paclen{128};
    int m_n2{8};
    int m_t1Ms{6000};
    int m_t2Ms{2000};       // deferred-ack delay (half-duplex burst guard)
    int m_t3Ms{ax25::kAx25MinT3Ms}; // idle-link poll; see setIdlePollMs()
    int m_retryCount{0};
    // REJ-recovery rounds since the last real progress. A peer that REJs without
    // ever accepting the retransmission used to reset m_retryCount on every REJ,
    // so N2 was never reached and we retransmitted forever. Bounded by N2.
    int m_rejRecoveries{0};
    ax25::LinkTimingProfile m_profile{};
    bool m_peerBusy{false};  // peer sent RNR
    bool m_ackPending{false}; // received I-frame(s) not yet acknowledged
    // Reject-exception (AX.25): true once we've REJ'd a sequence gap. While set,
    // further out-of-sequence I-frames are discarded WITHOUT another REJ, so we
    // don't key the radio repeatedly and (on half-duplex) go deaf to the very
    // retransmission we asked for. Cleared when the awaited in-sequence frame
    // finally arrives.
    bool m_rejectSent{false};

    QByteArray m_sendBuffer;            // app data awaiting segmentation
    QByteArray m_sentIFrames[8];        // by N(S), for retransmission
    bool m_iFrameValid[8]{};            // slot occupied
    // RTT bookkeeping, by N(S). The send stamp is taken on FIRST transmission
    // only; once a slot has been retransmitted its RTT is ambiguous and the
    // sample is discarded rather than skewed (Karn's algorithm).
    qint64 m_iFrameSentMs[8]{-1, -1, -1, -1, -1, -1, -1, -1};
    bool m_iFrameResent[8]{};
    QTimer* m_t1{nullptr};
    QTimer* m_t2{nullptr};
    QTimer* m_t3{nullptr};
    // Monotonic session clock. Free-running from construction; session-relative
    // values are taken as deltas from m_sessionStartMs.
    QElapsedTimer m_clock;
    qint64 m_sessionStartMs{0};
    Stats m_stats;
};

} // namespace AetherSDR
