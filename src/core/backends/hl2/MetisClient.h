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
    };

    // A discovered radio: its Metis reply plus the address to connect to.
    struct Discovered {
        DiscoveryReply reply;
        QHostAddress address;
    };

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
    Q_INVOKABLE void setRxFrequencyHz(std::uint32_t hz);
    Q_INVOKABLE void setSampleRate(SampleRate rate);
    Q_INVOKABLE void setLnaGainDb(int db);
    // Queue a one-shot filter-pipeline reset (MetisProtocol kC0Sync) to be sent
    // on the next EP2 frame, ahead of the round robin.
    Q_INVOKABLE void requestPipelineReset();

    // numRx clamped to what the board says it has. See Params.
    int effectiveNumRx() const;

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
    void iqBlockReady(const std::vector<std::complex<float>>& block);  // one per EP6 packet
    void dropsUpdated(quint64 drops);                             // cumulative EP6 gaps
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
    // Send countPerBank C&C frames, pause, then countPerBank more. Run BEFORE
    // metis-start so the DDC latches sample rate / NCO / receiver count from a
    // real C&C frame; a stream started before any C&C has landed emits ADC-idle
    // samples (Q pinned to zero) until one does.
    void sendPrimingBurst(int countPerBank);

    // EP2 cadence follows the frame geometry, not the EP6 arrival rate: the
    // radio consumes one EP2 frame per kSamplesPerPacket samples, so at 48 kHz
    // that is 126/48000 s = 2625 us. Driving it from a wall clock (rather than
    // replying 1:1 to EP6) means a stalled receive path cannot starve the
    // radio's watchdog and deadlock the link.
    // EP2 carries the TX IQ + speaker audio stream, which the radio clocks at a
    // FIXED 48 kHz regardless of the RX sample rate (only EP6 scales with that).
    // One EP2 frame holds kSamplesPerPacket samples, so the cadence is a constant
    // 126/48000 s = 2625 us. Verified against the Thetis Protocol 1 client, whose
    // EP2 thread blocks on the 48 kHz audio subsystem rather than a timer.
    static constexpr int kEp2AudioRateHz     = 48000;
    static constexpr int kStartRetryMs       = 300;
    static constexpr int kMaxStartAttempts   = 5;
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
    Cc m_ccFreq{};
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
    std::vector<std::complex<float>> m_block;   // reused per-packet decode buffer
    Hl2Telemetry m_telemetry;                   // accumulated across RADDR cycles
    // Telemetry rides the C&C bytes of every EP6 frame, so it parses ~3000x/s at
    // 384 kHz. The meters only need ~10 Hz; rate-limit the cross-thread emit so
    // publishTelemetry() does not flood the GUI thread. (#4449 review)
    QElapsedTimer m_telemetryEmitClock;
    static constexpr qint64 kTelemetryMinIntervalMs = 100;
};

}  // namespace AetherSDR::hl2

// Hl2Telemetry rides a queued signal from the I/O thread to the GUI thread.
// Declared HERE and not in MetisProtocol.h on purpose: that header is
// deliberately Qt-free so the wire primitives unit-test without linking Qt,
// and hl2_metis_protocol_test does exactly that.
Q_DECLARE_METATYPE(AetherSDR::hl2::Hl2Telemetry)
