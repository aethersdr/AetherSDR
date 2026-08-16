#pragma once

#include <QElapsedTimer>
#include <QHostAddress>
#include <QTimer>
#include <QList>
#include <QObject>

#include <complex>
#include <span>
#include <cstdint>
#include <deque>
#include <vector>

#include "core/backends/hl2/MetisProtocol.h"

class QUdpSocket;

namespace AetherSDR::hl2 {

// Owns the Hermes-Lite 2 UDP wire (HPSDR Protocol 1 / "Metis"): discovery,
// start/stop, the config+gain+freq Command&Control round-robin (paced 1:1 with
// the EP6 IQ torrent), and EP6 ingest into normalized IQ blocks. Below the seam;
// the future Hl2Backend owns one MetisClient plus an Hl2RxDsp.
//
// Lives on Hl2Backend's dedicated I/O thread, not the GUI thread. That is not
// only about keeping WDSP off the UI: this class also paces EP2, and the HL2
// gateware watchdog halts the stream if EP2 stops arriving. With the pacer on
// the GUI thread, any GUI stall long enough to miss it would wedge the radio --
// after which the board stops answering discovery until it is power-cycled.
//
// RX-ONLY: every C&C it sends comes from MetisProtocol's even-C0 encoders, so
// the MOX bit is never set — this class cannot key the radio.
class MetisClient : public QObject {
    Q_OBJECT

public:
    explicit MetisClient(QObject* parent = nullptr);
    ~MetisClient() override;

    struct Params {
        QHostAddress host;
        quint16 port = kMetisPort;
        SampleRate sampleRate = SampleRate::R48k;
        // RX1's NCO. Receivers beyond the first start here too and are moved by
        // setRxFrequencyHz(rxIndex, hz); starting them all on one frequency is
        // deliberate — an unconfigured receiver parked at 0 Hz would render a
        // panadapter of DC and look like a hardware fault.
        std::uint32_t rxFrequencyHz = 10'000'000;
        int lnaGainDb = 20;
        // How many receivers to actually RUN. Phase 1 runs one. This is the
        // value the config register must carry -- not the board's capability.
        int numRx = 1;
        // What the board reported in its discovery reply (byte 20), or 0 if the
        // reply was a short one that omits it. Used only to clamp numRx: asking
        // a board for more receivers than it has is a configuration the
        // gateware cannot honour, and it does not report the refusal.
        int boardMaxRx = 0;
        // J16 open-collector filter selection (MetisProtocol kOc* bits). Rides
        // the config register, so it is part of the same Params the config
        // register is rebuilt from — see setSampleRate() for why a field that
        // shares a register with another has to be carried, not re-defaulted.
        std::uint8_t ocFilterByte = kOcNone;
    };

    // A discovered radio: its Metis reply plus the address to connect to.
    struct Discovered {
        DiscoveryReply reply;
        QHostAddress address;
    };

    // Transport counters for the network readouts. Everything here is measured
    // on THIS socket, which is why it lives at this level and not above it:
    // nothing further up the stack ever sees a datagram.
    //
    // The gap figures are timed once per SOCKET WAKEUP, not once per datagram,
    // and that distinction is the whole reason they are usable. A wakeup drains
    // every datagram queued behind it, so successive datagrams inside one drain
    // are microseconds apart no matter how badly the link is behaving — timing
    // those would report a rock-steady 0 ms through a stall that silenced the
    // audio. Wakeup-to-wakeup is when packets actually reached us.
    struct LinkCounters {
        quint64 rxBytes = 0;
        quint64 txBytes = 0;
        quint64 rxPackets = 0;      // EP6 datagrams accepted
        quint64 txPackets = 0;      // datagrams sent (EP2 + start/stop)
        quint64 drops = 0;          // cumulative EP6 sequence gaps
        // Over the publish window only, so a stall that has ended stops being
        // reported as if it were still happening. Negative = nothing measured.
        int meanGapMs = -1;
        int maxGapMs = -1;
        // "ip:port" of our bound socket. The address is the one the KERNEL
        // delivered on, taken from the first datagram that carries a
        // destination — a wildcard bind knows only the port, and "0.0.0.0"
        // names no interface to an operator debugging a multi-homed host.
        // "*:<port>" until (or unless) the platform supplies it.
        QString localEndpoint;
    };
    // I/O-THREAD ONLY, exactly like droppedPackets(): m_link is written from the
    // receive path and this object lives on the I/O thread. Returned BY VALUE
    // rather than by reference because LinkCounters holds a QString — handing out
    // a reference to one that another thread is assigning is an implicit-sharing
    // refcount race, not merely a torn integer. GUI-thread consumers read the
    // copy Hl2Backend mirrors from linkCountersUpdated instead.
    [[nodiscard]] LinkCounters linkCounters() const { return m_link; }

    // Blocking discovery broadcast; returns HPSDR/HL2 replies (deduped by MAC)
    // seen within timeoutMs. Safe to call before start().
    QList<Discovered> discover(int timeoutMs = 2000,
                               const QHostAddress& broadcast = QHostAddress::Broadcast,
                               quint16 port = kMetisPort);

    // start()/stop() and every live-control setter below MUST execute on this
    // object's own thread: start() constructs the QUdpSocket, and a socket takes
    // the affinity of the thread that creates it. Hl2Backend owns an I/O thread
    // and marshals these across; they are Q_INVOKABLE so it can.
    Q_INVOKABLE bool start(const Params& params);   // bind, send start + priming C&C, begin ingest
    Q_INVOKABLE void stop();                        // send stop, close socket
    [[nodiscard]] bool isRunning() const noexcept { return m_running; }
    [[nodiscard]] quint64 droppedPackets() const noexcept { return m_drops; }
    [[nodiscard]] const Hl2Telemetry& telemetry() const noexcept { return m_telemetry; }

    // Live control — latched into the next C&C round sent to the radio.
    // Move RX1's NCO. Equivalent to setRxFrequencyHz(0, hz).
    Q_INVOKABLE void setRxFrequencyHz(std::uint32_t hz);
    // Move receiver `rxIndex`'s NCO (zero-based). Out-of-range indices — beyond
    // effectiveNumRx(), or beyond the RX1..RX7 register run — are IGNORED rather
    // than clamped onto a neighbour: silently retuning a different receiver is
    // worse than not moving one, because the panadapter that did move is not the
    // one the operator was pointing at.
    Q_INVOKABLE void setRxFrequencyHz(int rxIndex, std::uint32_t hz);
    Q_INVOKABLE void setSampleRate(SampleRate rate);

    // Change how many receivers the radio streams, while it is running.
    //
    // THIS RESTARTS THE EP6 STREAM, and that is deliberate rather than lazy.
    // The receiver count changes the PAYLOAD LAYOUT — a round goes from 6N+2 to
    // 6M+2 bytes — and the EP6 packet carries no receiver-count field and no
    // marker for the packet where the change took effect. Simply re-sending the
    // config bank would leave a window of some milliseconds in which the radio
    // has switched layouts and the host has not, and every round in that window
    // is misread as garbage on EVERY receiver, with nothing reporting an error.
    //
    // metis-stop / reconfigure / metis-start makes the transition a hard edge
    // instead. It costs a brief gap in audio and spectrum, which is what adding
    // or closing a receiver looks like anyway, and it cannot silently corrupt.
    //
    // No-op if `count` resolves to the count already running. Frequencies for
    // receivers that survive are PRESERVED; new ones start on RX1's.
    Q_INVOKABLE void setReceiverCount(int count);
    Q_INVOKABLE void setLnaGainDb(int db);
    // Select the companion filter board's band filter (MetisProtocol kOc* bits).
    //
    // Latched into the config register and sent on the next round, and ALSO
    // pushed as a one-shot so the relays move with the band change rather than
    // up to three EP2 frames later. Filter switching that lags the retune is
    // the failure mode that matters on transmit.
    // Takes int, not uint8_t: this crosses threads via
    // QMetaObject::invokeMethod, which matches Q_ARG against the type name moc
    // recorded from this declaration, and `std::uint8_t` does not normalize to
    // the same string as `unsigned char`.
    Q_INVOKABLE void setBandFilter(int ocFilterByte);
    [[nodiscard]] std::uint8_t bandFilter() const noexcept { return m_params.ocFilterByte; }
    // Queue a one-shot filter-pipeline reset (MetisProtocol kC0Sync) to be sent
    // on the next EP2 frame, ahead of the round robin.
    Q_INVOKABLE void requestPipelineReset();

    // Receivers this client can both RUN and TUNE: the RX1..RX7 NCO registers
    // are one contiguous run (0x02..0x08) and RX8..RX12 are not. See ccRxFreq().
    static constexpr int kMaxTunableRx = 7;

    // numRx clamped to what the board says it has, and to kMaxTunableRx.
    // See Params.
    //
    // The STATIC form answers from a Params alone, without a running client.
    // That exists because the caller must know the receiver count BEFORE
    // start(): the DSP chains have to be built and configured first, and WDSP
    // channel setup is slow enough (~19 s on the first open a machine ever does,
    // generating FFTW wisdom -- docs/HERMES.md §22.3) that doing it after start()
    // stalls the I/O thread's EP2 pacer -- and the gateware watchdog halts the
    // stream when EP2 stops arriving.
    static int effectiveNumRx(const Params& p);
    int effectiveNumRx() const { return effectiveNumRx(m_params); }

    // ---- TRANSMIT ----
    //
    // This class could not key a radio at all until TX was added; every C0 was
    // even, so MOX was structurally always 0. That invariant is gone, and this
    // gate is what replaces it.
    //
    // enableTransmit() must be called explicitly before setMox() will do
    // anything. Default OFF, and setMox(true) with the gate closed is a no-op
    // that leaves every outgoing frame unkeyed -- not an error, because a
    // refused key must fail SAFE and stay refused rather than surfacing as
    // something a caller might retry past.
    //
    // This class does not read the environment and has no policy of its own.
    // Hl2Backend decides: an interactive run enables it, an automation run
    // defers to the bridge's AETHER_AUTOMATION_ALLOW_TX gate. Keeping the
    // mechanism here and the policy there is what lets the policy change --
    // as it just did -- without touching the part that is actually tested.
    //
    // hl2_tx_gate_test asserts the property directly: with the gate closed, no
    // emitted EP2 frame ever carries C0 bit 0, even with a key request standing.
    void enableTransmit(bool allowed) noexcept { m_txAllowed = allowed; }
    [[nodiscard]] bool transmitEnabled() const noexcept { return m_txAllowed; }

    // Key / unkey. Ignored unless enableTransmit(true) was called.
    Q_INVOKABLE void setMox(bool keyed);
    [[nodiscard]] bool isKeyed() const noexcept { return m_mox; }
    Q_INVOKABLE void setTxFrequencyHz(std::uint32_t hz);
    Q_INVOKABLE void setTxDriveLevel(int level);

    // Build the EP2 packet this client would send next, without sending it.
    // Exists so the gate can be tested on the exact bytes that would go out.
    std::array<std::uint8_t, kUsbPacketSize> buildNextControlPacket();

    // Queue transmit IQ. Samples are consumed kTxSamplesPerPacket at a time, one
    // packet per EP2 frame, so the queue drains at the 48 kHz EP2 rate.
    //
    // Underflow is SILENCE, not a stall: a short queue emits zeros rather than
    // repeating stale samples or blocking the pacer. Repeating would put a
    // periodic artefact on the air, and blocking would starve the radio's
    // watchdog. Overflow drops the oldest, because on transmit the freshest
    // audio is the one that matters.
    void queueTxIq(std::span<const std::complex<float>> iq);
    // Discard pending transmit audio. Call on unkey: whatever is still queued
    // belongs to the transmission that just ended.
    Q_INVOKABLE void flushTxIq();
    [[nodiscard]] std::size_t txQueueDepth() const noexcept { return m_txIq.size(); }

    // A baseband test tone, offsetHz from the TX carrier, amplitude 0..1.
    // amplitude <= 0 disables it. Takes precedence over queued IQ.
    //
    // Synthesised inside the packet builder, one packet's worth at a time, so it
    // is paced by EP2 itself rather than by a producer thread that could drift
    // against the pacer and underrun. Phase carries across packets, so the tone
    // is continuous rather than restarting every 2.625 ms.
    //
    // NEVER enabled implicitly. A radio that emits a carrier because a default
    // said so is an unintended transmission, so this is opt-in only.
    Q_INVOKABLE void setTxTestTone(double offsetHz, double amplitude);
    [[nodiscard]] bool txTestToneEnabled() const noexcept { return m_toneAmp > 0.0; }

signals:
    void linkUp();                                                  // first EP6 seen
    void linkDown();                                               // stopped
    // One per EP6 packet, carrying RX1 only. Kept for the single-receiver
    // consumers (bring-up probes, the TX-side tests) that have no notion of a
    // receiver index; it is emitted alongside iqBlocksReady, not instead of it.
    void iqBlockReady(const std::vector<std::complex<float>>& block);
    // One per EP6 packet, carrying EVERY active receiver: blocks[i] is DDC i,
    // and blocks.size() == effectiveNumRx(). The span is a view into a buffer
    // this object reuses, so a receiver must copy anything it keeps.
    void iqBlocksReady(const std::vector<std::vector<std::complex<float>>>& blocks);
    void dropsUpdated(quint64 drops);                             // cumulative EP6 gaps
    // Transport counters, published about once a second from the receive path.
    // Rate-limited for the same reason telemetryUpdated is: this would otherwise
    // cross to the GUI thread thousands of times a second to move a byte count.
    //
    // It stops arriving when EP6 stops, and that is load-bearing — the consumer
    // reads the absence as the link having gone quiet. Do NOT "fix" this by
    // driving it from a timer here.
    void linkCountersUpdated(const AetherSDR::hl2::MetisClient::LinkCounters& c);
    // Radio telemetry decoded from the EP6 C&C bytes: forward/reverse power,
    // temperature, TX FIFO depth, ADC overload, PTT. Free-running, so it
    // arrives without us issuing a request.
    void telemetryUpdated(const AetherSDR::hl2::Hl2Telemetry& t);
    // No EP6 arrived within kConnectTimeoutMs of start() — the radio is off,
    // unreachable, or already streaming to a different client.
    void connectFailed(const QString& reason);

private slots:
    void onReadyRead();
    void onEp2PacerTick();
    void onWatchdogTick();

private:
    void sendControlPacket();           // one round-robin EP2 C&C packet
    // Accumulate one socket wakeup into the gap window. Called at the TOP of
    // onReadyRead, because the instant the wakeup happened is what it measures.
    void accountReceiveWakeup();
    // Emit the counters if the publish window has elapsed. Called at the END of
    // onReadyRead, because the byte and packet totals are only true once this
    // wakeup's datagrams have been drained and counted.
    void publishLinkCountersIfDue();
    // Meter one outgoing datagram. Takes the socket's return value, so a write
    // the kernel refused is not counted as traffic that left the host.
    void countTx(qint64 bytesWritten) noexcept;
    // Send countPerBank C&C frames, pause, then countPerBank more. Run BEFORE
    // metis-start so the DDC latches sample rate / NCO / receiver count from a
    // real C&C frame; a stream started before any C&C has landed emits ADC-idle
    // samples (Q pinned to zero) until one does.
    void sendPrimingBurst(int countPerBank);

    // EP2 cadence follows the frame geometry, not the EP6 arrival rate: the
    // radio consumes one EP2 frame per kTxSamplesPerPacket samples, so at 48 kHz
    // that is 126/48000 s = 2625 us. Driving it from a wall clock (rather than
    // replying 1:1 to EP6) means a stalled receive path cannot starve the
    // radio's watchdog and deadlock the link.
    // EP2 carries the TX IQ + speaker audio stream, which the radio clocks at a
    // FIXED 48 kHz regardless of the RX sample rate (only EP6 scales with that).
    // One EP2 frame holds kTxSamplesPerPacket samples, so the cadence is a constant
    // 126/48000 s = 2625 us. Verified against the Thetis Protocol 1 client, whose
    // EP2 thread blocks on the 48 kHz audio subsystem rather than a timer.
    static constexpr int kEp2AudioRateHz     = 48000;
    static constexpr int kStartRetryMs       = 300;
    static constexpr int kMaxStartAttempts   = 5;
    // "EP6 is flowing" for the start-retry's purposes: a packet within this long.
    // Sits far ABOVE the slowest EP6 interpacket gap (2.6 ms, at 48 kHz with one
    // receiver) and far BELOW kStartRetryMs, so a running stream and a stopped one
    // are both unambiguous at every retry tick. See the retry timer's lambda for
    // why the test has to be recency rather than "has a packet ever arrived".
    static constexpr int kEp6FlowingWithinMs = 100;
    static constexpr int kEp2PacerTickMs     = 2;
    static constexpr int kEp2MaxBurstPerTick = 16;
    static constexpr int kWatchdogTickMs     = 25;
    static constexpr int kConnectTimeoutMs   = 2000;
    static constexpr int kSilenceTimeoutMs   = 2000;

    QTimer* m_ep2Timer = nullptr;         // paces EP2 off the wall clock
    QTimer* m_watchdogTimer = nullptr;    // EP6 silence detection
    QTimer* m_connectWatchdog = nullptr;  // single-shot: first-EP6 deadline
    QTimer* m_startRetryTimer = nullptr;  // re-sends metis-start until EP6 flows
    int     m_startAttempts = 0;          // start datagrams sent this connect
    QElapsedTimer m_ep2Clock;             // pacer reference clock
    QElapsedTimer m_sinceLastEp6;         // silence detection
    quint64 m_ep2Sent = 0;                // EP2 frames sent since m_ep2Clock
    qint64  m_ep2IntervalUs = 2625;       // derived from the sample rate
    bool    m_watchdogEnabled = true;     // gateware watchdog (anti-wedge)

    QUdpSocket* m_socket = nullptr;
    QHostAddress m_host;
    quint16 m_port = kMetisPort;
    Params m_params;

    // Current C&C, rebuilt from m_params on change. Touched only on this
    // object's thread (event-driven), so no synchronization is needed today.
    Cc m_ccConfig{};
    Cc m_ccGain{};
    // One NCO bank per receiver, indexed by DDC. Sized to effectiveNumRx() at
    // start(); every entry is a distinct register (0x02..0x08), so this is an
    // array of banks rather than one bank with a varying payload.
    std::vector<Cc> m_ccRxFreq;
    Cc m_ccTxFreq{};
    Cc m_ccTxDrive{};

    bool m_txAllowed = false;   // gate; see enableTransmit()
    std::deque<std::complex<float>> m_txIq;   // pending transmit samples
    // Roughly a quarter second at 48 kHz. Past this the operator is hearing
    // latency, so dropping is better than growing the backlog.
    static constexpr std::size_t kTxQueueMax = 12000;
    double m_toneHz = 0.0;
    double m_toneAmp = 0.0;
    double m_tonePhase = 0.0;   // radians, carried across packets
    bool m_mox = false;         // requested key state, only honoured if m_txAllowed

    std::uint32_t m_txSeq = 0;           // outgoing EP2 sequence
    unsigned m_roundRobin = 0;
    // One-shot C&C banks, drained one per EP2 frame BEFORE the round robin.
    // Ordering matters: a frequency change and its pipeline reset must reach the
    // radio in that order, and neither should wait up to three frames for the
    // rotation to come back around.
    std::deque<Cc> m_oneShot;           // which register pair to send next
    std::uint32_t m_expectedRxSeq = 0;   // for EP6 drop detection
    bool m_haveRxSeq = false;
    quint64 m_drops = 0;
    bool m_running = false;
    bool m_linkUp = false;
    // Reused per-packet decode buffers, one vector per running receiver. Sized
    // from m_ccRxFreq and reused rather than reallocated: at four receivers and
    // 384 kHz this path runs ~10 000 times a second.
    std::vector<std::vector<std::complex<float>>> m_blocks;
    Hl2Telemetry m_telemetry;                   // accumulated across RADDR cycles
    // Telemetry rides the C&C bytes of every EP6 frame, so it parses ~3000x/s at
    // 384 kHz. The meters only need ~10 Hz; rate-limit the cross-thread emit so
    // publishTelemetry() does not flood the GUI thread. (#4449 review)
    QElapsedTimer m_telemetryEmitClock;
    static constexpr qint64 kTelemetryMinIntervalMs = 100;

    // ---- transport counters (see LinkCounters) ----
    LinkCounters  m_link;
    bool          m_linkEndpointResolved = false;   // a datagram named our local address
    QElapsedTimer m_linkWindowClock;    // time since the last publish
    QElapsedTimer m_sinceLastWakeup;    // socket wakeup spacing
    int           m_linkWindowWakeups = 0;
    qint64        m_linkWindowGapSumUs = 0;
    qint64        m_linkWindowGapMaxUs = 0;
    static constexpr qint64 kLinkPublishIntervalMs = 1000;
};

}  // namespace AetherSDR::hl2

// Hl2Telemetry rides a queued signal from the I/O thread to the GUI thread.
// Declared HERE and not in MetisProtocol.h on purpose: that header is
// deliberately Qt-free so the wire primitives unit-test without linking Qt,
// and hl2_metis_protocol_test does exactly that.
Q_DECLARE_METATYPE(AetherSDR::hl2::Hl2Telemetry)
// Same reason: LinkCounters crosses the I/O thread to the GUI thread queued.
Q_DECLARE_METATYPE(AetherSDR::hl2::MetisClient::LinkCounters)
