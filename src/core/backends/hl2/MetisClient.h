#pragma once
#include "core/TxCoordinator.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QTimer>
#include <QList>
#include <QObject>

#include <complex>
#include <span>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

#include "core/backends/hl2/Hl2ControlRequest.h"
#include "core/backends/hl2/Hl2BandMemoryPolicy.h"
#include "core/backends/hl2/MetisProtocol.h"

class QUdpSocket;

namespace AetherSDR::hl2 {

// Owns the Hermes-Lite 2 UDP wire (HPSDR Protocol 1 / "Metis"): discovery,
// start/stop, the C&C round robin carried on paced EP2, and EP6 ingest into IQ
// blocks. Hl2Backend runs it on a dedicated I/O thread because this class paces
// EP2: the gateware watchdog halts the stream if EP2 stops, after which the board
// needs a power cycle. TX is fail-closed: enableTransmit() must open the gate
// before MOX or transmit samples can leave this object.
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
        int lnaGainDb = kLnaDefaultGainDb;
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
        // Whether the duty-cycle gate should run: the operator's standing intent, not
        // the instantaneous run byte. Carried in Params (like ocFilterByte) because
        // setReceiverCount()'s stop/start clears wide_spectrum along with `run`; a field
        // sharing a rebuild must be carried, not re-defaulted.
        bool bandscope = false;

        // ---- hardware-variant options (Hl2HardwareOptions decides these) ----
        //
        // THE CONFIG-REGISTER PAIR RIDES HERE FOR THE SAME MEASURED REASON
        // ocFilterByte and bandscope do: setSampleRate() and setReceiverCount()
        // REBUILD the config bank from Params, so a field that shares that
        // register and is not carried here gets silently dropped the next time
        // an unrelated control changes. For the dither bit that would mean a
        // SquareSDR 2's loudspeaker switching itself off when the operator
        // changed the panadapter span.
        bool ditherBit = false;
        bool randomBit = false;

        // Whether this radio has a local audio codec (HL2+ or SquareSDR 2), and
        // so whether the EP2 audio slot may carry samples at all. FALSE is not
        // "no audio", it is "that slot is EADDR" — see ep2WriteTxAudio().
        bool hasCodec = false;

        // Whether the VersaClock should be locked to an external 10 MHz
        // reference at CL1. Carried here so start() can re-send the sequence:
        // the radio boots on its crystal, so every connect is a change.
        bool cl1RefClock = false;

        // WHICH radio this session is for, by serial. Carried for exactly one
        // reason: the CL1 latch below is per-radio and this object is not. A
        // MetisClient is built once in Hl2Backend's constructor and destroyed
        // in its destructor, so it outlives every connect AND every swap
        // between two HL2s — a latch without an identity on it would follow
        // the operator from one radio to the next. Empty when the serial is
        // not known yet, which the latch treats as "do not act".
        QString radioSerial;
    };

    // A discovered radio: its Metis reply plus the address to connect to.
    struct Discovered {
        DiscoveryReply reply;
        QHostAddress address;
    };

    // Transport counters for the network readouts, measured on this socket. Gaps are
    // timed per socket wakeup, not per datagram: one wakeup drains datagrams queued
    // microseconds apart, so per-datagram timing would read 0 ms through a stall.
    struct LinkCounters {
        quint64 rxBytes = 0;
        quint64 txBytes = 0;
        quint64 rxPackets = 0;      // EP6 datagrams accepted
        quint64 txPackets = 0;      // datagrams sent (EP2 + start/stop)
        quint64 drops = 0;          // cumulative EP6 sequence gaps
        // ---- the wideband bandscope (EP4), off by default ----
        //
        // A THIRD set of counters and not a widening of the three above: EP4 is
        // a different endpoint with its own 20-bit sequence counter and its own
        // reset, so folding it into rxPackets/drops would report a gap on every
        // packet and hide which stream a loss belongs to.
        quint64 ep4Packets = 0;     // bandscope datagrams accepted
        quint64 ep4Drops = 0;       // cumulative EP4 sequence gaps
        // Backward jumps of ep4_seq_no, which are a RESET and not a loss. Kept
        // apart from ep4Drops because exactly one is expected per stream start
        // (the gateware re-aligns the counter's low two bits while the capture
        // FIFO fills) and none afterwards — 15,003 recorded packets saw one per
        // start and zero thereafter. A second one mid-session is a real
        // anomaly, and it would be invisible inside a counter that is supposed
        // to stay at zero.
        quint64 ep4Rewinds = 0;
        // Duty-cycle gate. bandscopeEnabled is read off m_params.bandscope at publish
        // time (up to kLinkPublishIntervalMs late), so the row reports what the client is
        // running. bandscopeBlocks counts blocks the gate accepted (four in-phase packets
        // after the flush), not ep4Packets/4.
        bool bandscopeEnabled = false;
        quint64 bandscopeBlocks = 0;
        // Arming cycles abandoned because no complete block arrived inside
        // bandscopeGuardMs(). Should read zero in steady state, but a non-zero
        // value does NOT on its own mean the radio stopped answering the run
        // byte: bandscopeGuardMs() names two unmeasured multi-receiver cases
        // that can fire it benignly — the arming delay being clocked by EP6
        // samples rather than packets, and the EP4 rate at two, and at four or
        // more, receivers. Read its assumptions before suspecting hardware.
        quint64 bandscopeTimeouts = 0;
        // EP6 silence recovery: run-command re-sends by the silence watchdog, and how
        // many the stream came back from; attempts without completions means this is the
        // wrong recovery for the fault. A successful recovery is otherwise invisible
        // (m_linkUp never drops), so these healthSnapshot() rows are its only record.
        // Zeroed at start(), not at stop().
        quint64 silenceRecoveryAttempts = 0;
        quint64 silenceRecoveriesCompleted = 0;
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

    // Change the running receiver count. Restarts EP6 (metis-stop, reconfigure,
    // metis-start): the round size changes from 6N+2 to 6M+2 bytes and EP6 carries no
    // receiver-count field, so an in-place change would silently misread every round
    // in the switchover window. No-op if the count is unchanged; surviving receivers
    // keep their frequencies, new ones start on RX1's.
    Q_INVOKABLE void setReceiverCount(int count);
    Q_INVOKABLE void setLnaGainDb(int db);
    // Select the companion filter board's band filter (MetisProtocol kOc* bits).
    // Latched into the config register, which rides bank A of every EP2 frame, so the
    // relays follow within one frame (~2.6 ms at 48 kHz). Not also queued as a
    // one-shot: bank B applies after bank A and would carry a stale snapshot (#4579).
    // int, not uint8_t: cross-thread Q_ARG matches moc's recorded type name.
    Q_INVOKABLE void setBandFilter(int ocFilterByte);

    // The config register's dither and random bits. ONE setter for the pair
    // because they share C3 and a setter per bit would have to read the other
    // one back out of m_ccConfig to avoid clearing it.
    //
    // What the dither bit MEANS is not decided here — see kConfigDither and
    // Hl2HardwareOptions::ditherBitOnWire(). This level only puts it on the
    // wire and keeps it across a register rebuild.
    Q_INVOKABLE void setDitherRandomBits(bool dither, bool random);

    // Declare whether this radio has a local audio codec. Gates the EP2 audio
    // slot: false leaves it at zero, which is what a bare HL2's EADDR requires.
    // Turning it off also drops whatever speaker audio was queued, because that
    // audio has nowhere to go and must not leak into EADDR on the next frame.
    Q_INVOKABLE void setLocalCodec(bool present);

    // Stereo speaker audio for a codec radio: interleaved int16 L,R at 48 kHz,
    // the EP2 rate. Ignored outright when no codec is declared. Bounded — see
    // kSpeakerAudioCapSamples — because this is fed from the audio thread and a
    // stalled EP2 pacer must not grow a queue without limit.
    Q_INVOKABLE void submitSpeakerAudio(const QByteArray& interleavedInt16);

    // Discard samples captured before the operator silenced the radio speaker.
    Q_INVOKABLE void clearSpeakerAudio();

    // Lock the VersaClock to an external 10 MHz reference at CL1, or return it
    // to the onboard crystal. Queues the twenty-four-bank reprogramming
    // sequence; see versaClockCl1Banks().
    Q_INVOKABLE void setCl1RefClock(bool externalRef);

    // Raise or clear the gateware's ATU tune request (0x09[20]). Rides the
    // drive-level bank, so this restates the current drive rather than being a
    // register of its own.
    Q_INVOKABLE void setAtuTuneRequest(bool request);
    // Push the TX frequency to the HL2 IO Board (I2C2 chip 0x1D) so an amplifier,
    // relay or transverter follows the band. Five one-shot banks, LSB last because
    // that register commits the value (ccIoBoardTxFrequency() owns the order);
    // m_oneShot drains one per EP2 frame, ~13 ms in all. Sent unconditionally: an
    // absent board NACKs and the gateware's i2c_master moves on. quint64: the field
    // is 40 bits wide.
    Q_INVOKABLE void setIoBoardTxFrequencyHz(quint64 hz);
    [[nodiscard]] std::uint8_t bandFilter() const noexcept { return m_params.ocFilterByte; }
    // Queue a one-shot filter-pipeline reset (MetisProtocol kC0Sync) to be sent
    // on the next EP2 frame, ahead of the round robin.
    Q_INVOKABLE void requestPipelineReset();

    // RQST/ACK (docs/HERMES.md §13 item 13): ask the radio to acknowledge one C&C
    // register write. Not a read or an RPC; read Hl2ControlRequest's header first.
    // Returns false, sending nothing, when: the stream is not running or no EP6 has
    // arrived (response slots ride EP6); a request is outstanding; a timed-out
    // request's quarantine has not elapsed; or the address is not on the allow-list
    // in the .cpp (today 0x0a and 0x0e, both re-asserted by the round robin).
    // This cannot key TX (ccRegister() leaves C0[0] clear; RQST is C0[7]), but only
    // the allow-list keeps it off the companion I2C bus, so widening the list widens
    // the blast radius. `subsystemRead` is refused today: only 0x3c/0x3d reply with
    // read data and neither is allowed; on an echo address it would leave the six-bit
    // address as the only match.
    Q_INVOKABLE bool requestRegister(int addr, quint32 data, bool subsystemRead = false);

    // I/O-THREAD ONLY, like linkCounters(). Returned by reference because the
    // machine holds no implicitly-shared members; GUI-thread consumers read the
    // controlReply* signals instead.
    [[nodiscard]] const Hl2ControlRequest& controlRequest() const noexcept
    {
        return m_ccRequest;
    }
    // Run or stop the bandscope's duty-cycle gate (endpoint 0x04). Ungated, EP4 is
    // 380.95 datagrams/s (~3.3 Mbit/s, measured flat across sample rates). The gate
    // raises wide_spectrum once per kBandscopeSampleMs and keeps one block: 12
    // datagrams/s (3 arming + 4 flushed + 4 kept + 1 trailing; hl2_ep4_gate_test
    // section 6), 0.11 Mbit/s on the wire. It exists for bandwidth only: on the bench
    // EP4 cost EP6 no drops. Default off, reached only via the "hl2" extension
    // "bandscope.enable". No-op unless the stream is running; re-asserting `run` in
    // the same byte does not perturb the IQ stream.
    Q_INVOKABLE void setBandscopeEnabled(bool on);
    // One bandscope block on demand, with its 2048 samples. Separate from the gate:
    // raises wide_spectrum for one arming cycle (~13 ms), decodes the block and lowers
    // it, touching neither the period timer nor m_params.bandscope. On demand rather
    // than a continuous consumer because that load on the I/O thread is unmeasured.
    // Answered exactly once, by bandscopeFrameReady or bandscopeFrameFailed; a second
    // request while one is outstanding is ignored.
    Q_INVOKABLE void requestBandscopeFrame();

    // The gate's sampling period, for a consumer that has to reason about what
    // the duty cycle costs the reading.
    //
    // EXPOSED BECAUSE THE BIAS DEPENDS ON IT. A peak taken over one block per
    // period understates the full-rate peak by an amount that is a function of
    // exactly this number (Hl2BandscopeHeadroom.h::gatedPeakBiasDbForPeriod),
    // and a consumer that hard-coded the resulting decibels would silently stop
    // matching the gate the moment this changed. Reading it here is what keeps
    // the two in step.
    [[nodiscard]] static constexpr int bandscopeSamplePeriodMs() noexcept
    {
        return kBandscopeSampleMs;
    }
    // The gate's standing intent, not the run byte: the bit is up only ~29 ms per
    // kBandscopeSampleMs (11 packets at 2.625 ms plus 2.4-2.6 ms arming latency) and
    // Protocol 1 cannot read it back. Survives setReceiverCount()'s restart; cleared
    // by start(), stop(), link loss, and the gate giving up on gateware with no EP4.
    [[nodiscard]] bool bandscopeEnabled() const noexcept { return m_params.bandscope; }
    [[nodiscard]] quint64 ep4Packets() const noexcept { return m_link.ep4Packets; }
    [[nodiscard]] quint64 ep4Drops() const noexcept { return m_ep4Drops; }
    [[nodiscard]] quint64 ep4Rewinds() const noexcept { return m_ep4Rewinds; }
    [[nodiscard]] quint64 bandscopeBlocks() const noexcept { return m_bsBlocks; }
    [[nodiscard]] quint64 bandscopeTimeouts() const noexcept { return m_bsTimeouts; }

    // How long one arming cycle may take before the gate abandons it, in
    // milliseconds, for the rate and receiver count currently configured.
    // Exposed so the sizing can be asserted against the two measured arming
    // delays rather than inspected through a QTimer.
    [[nodiscard]] int bandscopeGuardIntervalMs() const noexcept;

    // Receivers this client can both RUN and TUNE: the RX1..RX7 NCO registers
    // are one contiguous run (0x02..0x08) and RX8..RX12 are not. See ccRxFreq().
    static constexpr int kMaxTunableRx = 7;

    // numRx clamped to what the board reports and to kMaxTunableRx. The static form
    // answers from Params alone because the DSP chains must be built before start():
    // WDSP setup can take ~19 s on first open (FFTW wisdom, docs/HERMES.md §22.3),
    // and doing it after start() stalls the EP2 pacer into the gateware watchdog.
    static int effectiveNumRx(const Params& p);
    int effectiveNumRx() const { return effectiveNumRx(m_params); }

    // Transmit gate. setMox() does nothing until enableTransmit(true); with the gate
    // closed setMox(true) is a silent no-op, so a refused key fails safe. Policy lives
    // in Hl2Backend (interactive enables; automation defers to
    // AETHER_AUTOMATION_ALLOW_TX). hl2_tx_gate_test asserts that no EP2 frame carries
    // C0 bit 0 while the gate is closed.
    void enableTransmit(bool allowed) noexcept { m_txAllowed = allowed; }
    [[nodiscard]] bool transmitEnabled() const noexcept { return m_txAllowed; }

    // Key / unkey. Ignored unless enableTransmit(true) was called.
    void setMox(bool keyed, const TxCoordinator::Operation& operation);
    // Only the backend's explicit CW break-in path uses CW holds to sustain
    // MOX. Ordinary manual PTT must not borrow a still-held bare CW element.
    void setCwMox(bool keyed, const TxCoordinator::Operation& operation);
    [[nodiscard]] bool isKeyed() const noexcept { return m_mox; }
    Q_INVOKABLE void setTxFrequencyHz(std::uint32_t hz);
    Q_INVOKABLE void setTxDriveLevel(int level);

    // Software CW for a PC/USB/MIDI keyer. The carrier is generated in the EP2
    // packet builder so its envelope is sample-paced by the radio's fixed
    // 48 kHz transmit stream rather than by GUI or producer-thread timing.
    // It still requires MOX; Hl2Backend owns break-in and manual-PTT policy.
    void setCwKeyDown(bool down, const TxCoordinator::Operation& operation);
    Q_INVOKABLE void clearCwKeying();
    [[nodiscard]] bool cwModeActive() const noexcept { return m_cwMode; }
    [[nodiscard]] bool cwKeyDown() const noexcept { return m_cwKeyDown; }

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
    void queueTxIq(std::span<const std::complex<float>> iq, const TxCoordinator::Context& context);
    // Discard pending transmit audio. Call on unkey: whatever is still queued
    // belongs to the transmission that just ended.
    Q_INVOKABLE void flushTxIq();
    [[nodiscard]] std::size_t txQueueDepth() const noexcept { return m_txIq.size(); }

    // TX IQ FIFO fault accounting. The FIFO is bounded at kTxQueueMax: overflow drops
    // the oldest samples, underflow zero-fills (ep2WriteTxIq). These count, they do
    // not repair; one zeroed sample takes opposite-sideband suppression from ~79 dB to
    // ~49 dB (hpsdrsim loopback). The radio's FIFO telemetry cannot see this (EP2
    // frames are always full-size), and these cannot see past the socket.
    // Underflow counts only on the keyed queued-IQ path. A clean over still counts a
    // floor (whole-packet underflows at key-down, a short tail at key-up;
    // hl2_tx_gate_test pins both), so these are totals since process start, never
    // reset. I/O thread only; not for healthSnapshot().
    [[nodiscard]] std::uint64_t txUnderflowPackets() const noexcept { return m_txUnderflowPackets; }
    [[nodiscard]] std::uint64_t txUnderflowSamples() const noexcept { return m_txUnderflowSamples; }
    [[nodiscard]] std::uint64_t txOverflowSamples() const noexcept { return m_txOverflowSamples; }

    // Baseband test tone, offsetHz from the TX carrier, amplitude 0..1 (<= 0
    // disables); takes precedence over queued IQ. Synthesised per EP2 packet with
    // phase carried across packets, so it is paced by EP2 and cannot underrun. Never
    // enabled implicitly: an unintended carrier is an unintended transmission.
    void setTxTestTone(double offsetHz, double amplitude, const TxCoordinator::Operation& operation);
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
    // The next IQ block is discontinuous with the previous one. Emitted
    // before iqBlockReady/iqBlocksReady in the same handleDatagram() call,
    // including accepted rewinds and duplicates. Direct consumers can clear
    // partial FFTs before accepting the new samples. `lost` is the forward
    // packet gap, or zero for a rewind/duplicate; dropsUpdated stays loss-only.
    void rxSequenceGap(quint32 lost);
    // Transport counters, published about once a second from the receive path.
    // Rate-limited for the same reason telemetryUpdated is: this would otherwise
    // cross to the GUI thread thousands of times a second to move a byte count.
    //
    // It stops arriving when EP6 stops, and that is load-bearing — the consumer
    // reads the absence as the link having gone quiet. Do NOT "fix" this by
    // driving it from a timer here.
    void linkCountersUpdated(const AetherSDR::hl2::MetisClient::LinkCounters& c);
    // Radio telemetry decoded from the EP6 C&C bytes: forward/reverse power,
    // temperature, TX FIFO status, ADC overload, PTT. Free-running, so it
    // arrives without us issuing a request.
    //
    // "status", not "depth": the wire carries a recovery flag plus the top 7
    // bits of the fill level, and no sample count at all. See
    // Hl2Telemetry::apply().
    void telemetryUpdated(const AetherSDR::hl2::Hl2Telemetry& t);
    // One accepted bandscope block: 2048 contiguous converter samples merged from the
    // four in-phase EP4 packets the gate kept (never per packet). Uncalibrated and
    // pre-DDC, on the AD9866's own scale: nothing downstream may treat these as
    // absolute or make a decision from them (IRadioBackend.h).
    void bandscopeBlockReady(const AetherSDR::hl2::Ep4Stats& block);
    // The block requestBandscopeFrame() asked for: kEp4BlockSamples converter codes,
    // contiguous (26.67 us of the 76.8 MSPS ADC), normalised to [-1, 1) by
    // kEp4FullScale. Separate from bandscopeBlockReady so only a request pays for the
    // decode; a QList so it can cross threads queued. Uncalibrated and pre-DDC.
    void bandscopeFrameReady(const QList<float>& samples);
    // The request could not be answered, with the reason in the operator's
    // words. Emitted once per failed request, never alongside a Ready.
    void bandscopeFrameFailed(const QString& reason);
    // No EP6 arrived within kConnectTimeoutMs of start() — the radio is off,
    // unreachable, or already streaming to a different client.
    void connectFailed(const QString& reason);

    // A RQST issued through requestRegister() was acknowledged. `addr` is the
    // address ASKED FOR, not the one in the ACK, so a caller never has to
    // reason about the 0x3F refusal encoding; `data` is the register's echo,
    // or the read value for a subsystem read.
    void controlReplyReady(int addr, quint32 data);
    // A RQST did not come back. `refused` distinguishes the radio saying no (a
    // subsystem was not ready) from the radio saying nothing at all. Both are
    // ordinary outcomes on this protocol, not errors — and after a timeout the
    // machine is in quarantine, so the next requestRegister() will refuse for
    // a while. Consumers must not retry immediately in this handler.
    void controlRequestFailed(int addr, bool refused);

private slots:
    // The sampling period elapsed: start one arming cycle if the interlocks allow.
    void onBandscopeTick();
    // An arming cycle took longer than bandscopeGuardIntervalMs(). Abandon it.
    void onBandscopeGuardTimeout();
    void onReadyRead();
    void onEp2PacerTick();
    void onWatchdogTick();

private:
    void sendControlPacket();           // one round-robin EP2 C&C packet
    // Append `bank` to m_oneShot only while a session is running -- the gate
    // for the live-control setters that queue a single bank (#4579). It keeps
    // a bank out of the queue while stopped; what an ended session left
    // undrained is stop()'s to discard or keep.
    // setIoBoardTxFrequencyHz() refuses at its top instead, for its own dedupe.
    void queueOneShotIfRunning(const Cc& bank);
    // One datagram off this socket, whatever endpoint it came from: the EP6/EP4
    // branch, the sequence accounting, telemetry and the IQ decode. Split out of
    // onReadyRead's drain loop so the whole ingest path can be driven from
    // recorded bytes with no socket bound — see MetisClientTestAccess.
    void handleDatagram(std::span<const std::uint8_t> bytes);
    // Account one bandscope datagram: its sequence step, and the counters. Takes
    // the already-parsed sequence alongside the bytes, because the caller has
    // had to parse it to know the datagram was EP4 at all.
    void handleEp4(std::uint32_t seq, std::span<const std::uint8_t> bytes);
    // ---- the duty-cycle gate (see setBandscopeEnabled) ----
    //
    // Idle -> Arming -> Flushing -> Capturing -> Idle, once per
    // kBandscopeSampleMs. Arming raises wide_spectrum; Flushing throws away the
    // block that was already in the capture FIFO when it went up; Capturing
    // keeps the next one and lowers the bit again.
    enum class BandscopeState { Idle, Arming, Flushing, Capturing };
    // A ptt_resp transition reported by the radio: the bandscope interlock's edge for
    // keying this client did not initiate.
    // bandscopeOnPacket() offers one EP4 datagram to the gate, apart from handleEp4's
    // wire counters; it emits, so it is not noexcept. `drops` is what ep4SeqStep()
    // charged before this packet, since `seq % 4` cannot see a loss of four.
    void onRadioPttEdge(bool keyed);
    void bandscopeOnPacket(std::uint32_t seq, std::uint32_t drops,
                           std::span<const std::uint8_t> bytes);
    // Begin one arming cycle: raise wide_spectrum, clear the accumulator, arm
    // the guard. Refused, and silently, while the interlocks below say so.
    void bandscopeArm();
    // Lower wide_spectrum and return to Idle. `expectTrailing` records that one
    // more packet is still coming — see m_bsTrailingPending.
    void bandscopeDisarm(bool expectTrailing);
    // True while a bandscope block would be a picture of our own transmitter.
    [[nodiscard]] bool bandscopeInterlocked() const noexcept;
    // One run-byte datagram with wide_spectrum set or clear and `run` kept set.
    // The only place the gate touches the wire.
    void sendBandscopeRunByte(bool wideSpectrum);
    // Start or stop the period timer to match m_params.bandscope. Called by
    // setBandscopeEnabled() and again after setReceiverCount()'s restart, which
    // is what carries the sensor across a panadapter being added.
    void applyBandscopeGate();
    // Answer an outstanding on-demand frame request with a failure, if there is
    // one. Separate from resetBandscopeGate() — which is noexcept and must stay
    // so — because this EMITS, and a queued emit allocates.
    void failPendingBandscopeFrame(const QString& reason);
    // Drop every piece of in-flight cycle state and stop both timers, leaving
    // m_params.bandscope alone. The counters are cumulative and survive.
    void resetBandscopeGate() noexcept;
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
    // Blocking (two msleep(10)), so only start() may call it, before the EP2
    // pacer and the EP6 stream exist. A restart on a live stream is spaced by
    // advanceReceiverCountRestart() instead.
    void sendPrimingBurst(int countPerBank);
    // One bank of the priming burst: countPerBank C&C frames, back to back.
    void sendPrimingBank(int countPerBank);
    // A 64-byte run/stop datagram to the radio (unicast, m_host:m_port), through
    // m_commandSinkForTest when a test installed one. Used by the receiver-count
    // restart and the bandscope run byte; start(), stop() and the start retry
    // still write the socket directly.
    qint64 sendCommandDatagram(const std::array<std::uint8_t, 64>& cmd);
    bool hasCommandTransport() const noexcept
    {
        return m_socket != nullptr || static_cast<bool>(m_commandSinkForTest);
    }
    // setReceiverCount()'s stop -> prime -> start -> prime sequence, driven by
    // m_restartTimer instead of msleep so the EP2 pacer keeps feeding the radio
    // and EP6 keeps draining. Each call performs the next step once at least
    // kPrimingBankSpacingMs has passed since the previous one.
    void advanceReceiverCountRestart();
    // Arm the timer for the step after this one, measuring from now.
    void scheduleReceiverCountRestartStep();
    // Abandon a restart in flight (stop(), or a newer setReceiverCount()).
    void cancelReceiverCountRestart() noexcept;
    // True between a restart's metis-stop and its metis-start: every datagram
    // that reaches the socket then is in the OLD payload layout, and no run byte
    // may go out, because any run byte with bit 0 set IS a start.
    bool restartAwaitingStart() const noexcept
    {
        return m_restartStep == RestartStep::PrimeBeforeStart
            || m_restartStep == RestartStep::Start;
    }
    // Seed the start-retry budget and start its timer. The datagram itself is
    // NOT sent here: the three callers put different bytes on the wire --
    // start() and setReceiverCount() send metisStart(), onWatchdogTick()'s
    // stage 1 sends metisRunCommand() -- and folding the send in would either
    // take a 64-byte array nobody but this helper would build or hide the one
    // line a reader of those paths most needs to see. What it does remove is
    // the third hand-rolled copy of "m_startAttempts = 1; timer->start()" and
    // the off-by-one that copy invited: the seed of 1 COUNTS THE DATAGRAM THE
    // CALLER JUST SENT, so what remains is kMaxStartAttempts - 1 re-sends.
    void armStartRetry();
    // Offer one decoded EP6 C&C response to the RQST/ACK machine and publish
    // whatever verdict that produces. Separated from the datagram loop so a
    // test can drive the reply path with a synthetic Ep6Response and no socket
    // — see MetisClientTestAccess.
    void ingestControlResponse(const Ep6Response& resp);
    // Advance the RQST/ACK deadline by one EP6 frame and publish any verdict
    // that falls out — a timeout has no ACK to carry it. `nowMs` is the
    // wall-clock half of the deadline: a frame count alone is 2.08 ms at
    // 384 kHz with three receivers, which is inside a single recorded delivery
    // gap. Passed in rather than read inside Hl2ControlRequest so that class
    // keeps no clock, and passed in HERE rather than read inside this method so
    // the socket-free tests can pin a rate without real time passing.
    void tickControlRequest(qint64 nowMs);
    // Monotonic milliseconds for the two calls above. Origin is arbitrary — the
    // value is only ever differenced inside Hl2ControlRequest.
    [[nodiscard]] qint64 controlNowMs() const noexcept;
    // Emit whatever verdict the machine has settled, if any.
    void publishControlVerdict();
    // Confirm that the packet buildNextControlPacket() just produced reached the
    // socket, and start the RQST deadline if it carried the request bank. Takes
    // the socket's return value, so a write the kernel refused leaves the
    // request Queued for the next frame instead of burning it — which is what
    // makes Hl2ControlRequest::onRequestSent()'s "handed to the socket" true
    // rather than aspirational. Exposed to MetisClientTestAccess so the
    // socket-free tests drive the same two-step seam the transport does.
    // `nowMs` starts the deadline's wall-clock floor and must come from the
    // same clock as tickControlRequest()'s.
    void onControlPacketSent(qint64 bytesWritten, qint64 nowMs) noexcept;
    // Give up the outstanding request because the stream it belonged to is
    // gone: publish anything already settled, tell a caller that is still
    // waiting, then reset. Used by stop() and by the silence watchdog's
    // link-down, which must behave the same way.
    void dropControlRequest();

    // EP2 carries TX IQ and speaker audio at a fixed 48 kHz whatever the RX rate, so
    // the pacer sends one frame per kTxSamplesPerPacket samples: 126/48000 s = 2625
    // us (as Thetis's Protocol 1 EP2 thread does). Wall-clock pacing, not 1:1 with
    // EP6, keeps a stalled receive path from starving the radio's watchdog.
    static constexpr int kEp2AudioRateHz     = 48000;
    static constexpr int kStartRetryMs       = 300;
    static constexpr int kMaxStartAttempts   = 5;
    // How many FURTHER re-sends armStartRetry() leaves, for anything that
    // reports the budget to a human. The seed of 1 counts the datagram the
    // caller has already sent, so "budget 5" in a log line is wrong by one.
    static constexpr int kStartResendsAfterArm = kMaxStartAttempts - 1;
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
    // setReceiverCount()'s restart and onWatchdogTick()'s stage 1 both rely on the
    // start-retry budget expiring inside one silence window: otherwise the watchdog
    // fires with a retry pending, and stage 1's `isActive()` wait is unbounded.
    static_assert(kMaxStartAttempts * kStartRetryMs < kSilenceTimeoutMs,
                  "the start-retry budget must expire inside one silence window");

    QTimer* m_ep2Timer = nullptr;         // paces EP2 off the wall clock
    QTimer* m_watchdogTimer = nullptr;    // EP6 silence detection
    QTimer* m_connectWatchdog = nullptr;  // single-shot: first-EP6 deadline
    QTimer* m_startRetryTimer = nullptr;  // re-sends metis-start until EP6 flows
    int     m_startAttempts = 0;          // start datagrams sent this connect
    // The receiver-count restart in flight, if any. The step names what the
    // NEXT timeout does. See advanceReceiverCountRestart().
    enum class RestartStep : std::uint8_t {
        Idle,              // no restart in flight
        PrimeBeforeStart,  // send the second pre-start priming bank
        Start,             // discard stale EP6, send metis-start, first post-start bank
        PrimeAfterStart,   // send the second post-start bank
        Finish,            // arm the start retry, re-apply the bandscope gate
    };
    // The spacing the two msleep(10) calls in sendPrimingBurst() gave each bank.
    // A FLOOR: a step that fires early is re-armed for the remainder.
    static constexpr int kPrimingBankSpacingMs = 10;
    static constexpr int kPrimingFramesPerBank = 3;
    QTimer* m_restartTimer = nullptr;     // single-shot: the next restart step
    QElapsedTimer m_restartStepClock;     // since the previous restart step
    RestartStep m_restartStep = RestartStep::Idle;
    int m_restartStalePackets = 0;        // old-layout datagrams discarded this restart
    QElapsedTimer m_ep2Clock;             // pacer reference clock
    QElapsedTimer m_sinceLastEp6;         // silence detection
    // A recovery is in flight for the CURRENT silence. Set when the watchdog
    // re-sends the run command, cleared by the EP6 packet that ends the
    // silence -- so it is per-silence, not per-session, and a link that goes
    // quiet twice gets two recoveries rather than one. Also cleared by stop()
    // and by setReceiverCount(), which restarts the stream by its own route and
    // must not have its EP6 credited to this one.
    bool m_silenceRecoveryArmed = false;
    // Stage 1's only observable is a datagram on m_socket; with no injectable sink
    // beside feedDatagram(), its test uses a fake Metis radio (socket exception, #5254).
    // Free-running and never restarted: the RQST/ACK floor differences it, so it must
    // not reset under an outstanding request.
    QElapsedTimer m_controlClock;
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
    // Queue (or re-queue) the twenty-four VersaClock banks, and remove any
    // still waiting. Private because the ORDER and the replace-don't-append
    // rule are part of the contract and a caller outside this class cannot
    // honour them; setCl1RefClock() is the way in.
    void queueCl1Sequence(bool externalRef);
    void dropQueuedCl1Banks();
    // True for a bank this class queued as part of a VersaClock sequence. One
    // predicate, because three sites ask the question — the drop, the drain and
    // the send confirmation — and a fourth spelling of it is how they diverge.
    [[nodiscard]] static bool isCl1Bank(const Cc& bank) noexcept;
    // THE RECOVERY RULE, named once. True when start() must send the OFF table
    // for this radio even though the setting is clear: this process switched it
    // on and never confirmed switching it back. start() reads it, and it is the
    // property hl2_cl1_reference_test asserts — the condition is the whole of
    // the fix for #5923's first blocker, so it is worth a name rather than an
    // expression buried in a 200-line function.
    [[nodiscard]] bool cl1RecoveryPending() const noexcept
    {
        return cl1RecoveryPendingFor(m_params.radioSerial);
    }
    // Process-owned recovery survives backend recreation on a family switch.
    // Registry operations run only on the control plane, never in audio callbacks.
    [[nodiscard]] static bool cl1RecoveryPendingFor(const QString& serial) noexcept;
    // The sequence currently draining out of m_oneShot: whose it is, whether it
    // is the OFF table, and how many of its banks have not yet been CONFIRMED
    // SENT. Only when the last one is confirmed does its radio leave
    // the process recovery registry.
    //
    // WHY CONFIRMED AND NOT MERELY QUEUED. queueCl1Sequence() used to clear the
    // record the instant the OFF table was queued, before any of its twenty-four
    // writes reached the transport. A disconnect in that window had stop() drop
    // the remainder AND left start() with nothing to recover from, so a radio
    // that was still powered stayed on the external reference for good. Counting
    // confirmations instead means an interrupted sequence simply never finishes
    // its countdown, the record stands, and the next connect re-sends the whole
    // table — which is the behaviour stop()'s own comment already claimed.
    QString m_cl1SequenceRadio;
    bool    m_cl1SequenceIsOff = false;
    int     m_cl1BanksUnsent = 0;
    // Mirrors m_requestOnBuiltPacket: the packet just built carried a CL1 bank,
    // and onControlPacketSent() decides whether it counts.
    bool    m_cl1BankOnBuiltPacket = false;

    Cc m_ccTxDrive{};
    // The ATU tune request's standing state, held because it shares 0x09 with
    // the drive level: setTxDriveLevel() has to re-assert it or a drive change
    // mid-tune would drop the request.
    bool m_atuTune = false;
    // Speaker audio awaiting the EP2 pacer, interleaved L,R at 48 kHz.
    std::deque<std::int16_t> m_speakerAudio;
    // ~250 ms of stereo at 48 kHz. Deep enough to ride out the audio thread's
    // block jitter, shallow enough that a stall is heard as a gap rather than
    // as a quarter-second of delay that never recovers.
    static constexpr std::size_t kSpeakerAudioCapSamples = 48000 / 4 * 2;

    bool m_txAllowed = false;   // gate; see enableTransmit()
    std::deque<std::complex<float>> m_txIq;   // pending transmit samples
    TxCoordinator::Context m_txIqContext;
    // Roughly a quarter second at 48 kHz. Past this the operator is hearing
    // latency, so dropping is better than growing the backlog.
    static constexpr std::size_t kTxQueueMax = 12000;
    // Monotonic for the life of the PROCESS; see the accessors above. Never
    // reset on key, unkey, link loss or a change of radio -- a per-over counter
    // would answer a different question and would lose the drift that
    // accumulates, which is the one these exist to show. The log line calls
    // them "totals since process start" for that reason and not "session".
    std::uint64_t m_txUnderflowPackets = 0;
    std::uint64_t m_txUnderflowSamples = 0;
    std::uint64_t m_txOverflowSamples = 0;
    // Length of the starvation currently in progress, in packets and in
    // substituted silent samples, so ONE log line describes the whole EPISODE
    // instead of one line per EP2 frame at 380 frames/second. Reported and
    // cleared by reportTxUnderflowRun(), which every exit from the queued-IQ
    // path calls: a flush, the key going up, or the stream being taken by CW or
    // the test tone.
    std::uint64_t m_txUnderflowRunPackets = 0;
    std::uint64_t m_txUnderflowRunSamples = 0;
    // Contiguous full packets since the last short one. A starvation episode ends
    // after ~100 ms of full packets (derived from the EP2 rate), not on the next full
    // one: clock drift yields alternating short/full packets, and flushing per full
    // packet would log a line per pair (#5813). Every exit from the queued-IQ path
    // (unkey, flushTxIq(), CW, test tone) also flushes the episode.
    static constexpr std::uint64_t kUnderflowRunQuietPackets =
        (static_cast<std::uint64_t>(kEp2AudioRateHz) / 10u)
            / static_cast<std::uint64_t>(kTxSamplesPerPacket);
    std::uint64_t m_txUnderflowRunQuietPackets = 0;
    void reportTxUnderflowRun();
    double m_toneHz = 0.0;
    double m_toneAmp = 0.0;
    double m_tonePhase = 0.0;   // radians, carried across packets
    bool m_cwMode = false;      // while true, CW owns TX IQ (silence between elements)
    bool m_cwKeyDown = false;
    double m_cwEnvelope = 0.0;  // 0..1 raised-cosine ramp position
    bool m_mox = false;         // requested key state, only honoured if m_txAllowed
    TxCoordinator::Operation m_moxOperation;
    TxCoordinator::Operation m_cwOperation;
    TxCoordinator::Operation m_toneOperation;
    void setMoxImpl(bool keyed, const TxCoordinator::Operation& operation, bool cwBreakIn);

    std::uint32_t m_txSeq = 0;           // outgoing EP2 sequence
    unsigned m_roundRobin = 0;
    // One-shot C&C banks, drained one per EP2 frame BEFORE the round robin.
    // Ordering matters: a frequency change and its pipeline reset must reach the
    // radio in that order, and neither should wait up to three frames for the
    // rotation to come back around.
    friend struct MetisClientTestAccess; // socket-free transport-state injection
    std::function<qint64(const std::array<std::uint8_t, kUsbPacketSize>&)> m_packetSinkForTest;
    std::function<qint64(const std::array<std::uint8_t, 64>&)> m_commandSinkForTest;
    std::deque<Cc> m_oneShot;           // which register pair to send next
    // The single RQST slot. Drained AFTER m_oneShot, never before: a one-shot is
    // a write the operator asked for, and letting a read-back overtake it would
    // answer with the value from before the change.
    Hl2ControlRequest m_ccRequest;
    // Whether the packet buildNextControlPacket() last produced carries the RQST
    // bank. Lives here rather than being inferred from m_ccRequest's state
    // because the state is what the confirmation CHANGES: by the time
    // onControlPacketSent() runs, "is it still Queued" cannot distinguish a
    // request that went out from one that never did.
    bool m_requestOnBuiltPacket = false;
    // Last transmit frequency handed to the IO board, and whether one ever was.
    // A separate flag rather than a 0 sentinel: 0 Hz is not a plausible tuned
    // frequency, but "never sent" still has to survive a radio that legitimately
    // reports it, and the first push after connect must go out even if the
    // backend's throttle happens to compute the same value it had before.
    quint64 m_ioBoardTxFreqHz = 0;
    bool m_ioBoardTxFreqSent = false;
    std::uint32_t m_expectedRxSeq = 0;   // for EP6 drop detection
    bool m_haveRxSeq = false;
    quint64 m_drops = 0;
    // The SAME triple for EP4, and deliberately not shared with the one above.
    // ep4_seq_no is a separate 20-bit counter in the gateware with its own
    // reset (`if (~run)` zeroes both independently), so a client that tracked
    // one expectation across both endpoints would report a gap on every single
    // packet of whichever stream it saw second.
    std::uint32_t m_expectedEp4Seq = 0;
    bool m_haveEp4Seq = false;
    quint64 m_ep4Drops = 0;
    quint64 m_ep4Rewinds = 0;

    // ---- the duty-cycle gate ----
    //
    // How often one block is taken. THE BOUND, stated because REPORTING.md §7
    // asks for it: the release law the eventual consumer would run requires
    // 3000 ms of dwell before any step, so a sampler slower than ~1 Hz would
    // make that dwell a property of this timer rather than of the plant. The
    // CHOICE is 1000 ms — one sample per dwell-third, 12 datagrams/s,
    // 0.11 Mbit/s. Nothing measured supports a faster one.
    static constexpr int kBandscopeSampleMs = 1000;
    // Consecutive guard timeouts, with NO EP4 datagram seen in the whole
    // session, after which the gate stops asking. Ten seconds of a gateware
    // that does not implement endpoint 0x04 is enough to conclude it never
    // will; anything that has produced even one EP4 packet is a link or timing
    // problem and keeps retrying. (PR #5650 review round 3.)
    static constexpr int kMaxConsecutiveBandscopeTimeouts = 10;
    // How long after unkey a block is still refused. FIND-16 / d83's measured
    // post-unkey transient is 178-285 ms, median 229; 300 ms clears it. The HL2
    // receives while it transmits and hears itself at enormous strength, so a
    // block taken inside this window is a picture of our own PA and not of the
    // band.
    static constexpr int kBandscopeUnkeyHoldoffMs = 300;

    BandscopeState m_bsState = BandscopeState::Idle;
    QTimer* m_bandscopeTimer = nullptr;   // paces one arming cycle per sample period
    QTimer* m_bandscopeGuard = nullptr;   // single-shot: this cycle's deadline
    // Packets consumed in the current Flushing / Capturing run, which is also
    // the phase (seq % 4) the next packet of that run must carry.
    int m_bsPhase = 0;
    Ep4Stats m_bsBlock;                   // the block being accumulated
    // ---- the on-demand frame (see requestBandscopeFrame) ----
    //
    // A request is outstanding. Cleared by whichever of the two answers goes
    // out, so the two flags below can never both be live for one request.
    bool m_bsFrameRequested = false;
    // THIS cycle is decoding samples, latched when Capturing is entered and not
    // read from m_bsFrameRequested per packet. The latch is what makes the
    // partial-block case finite: a request that arrives after a cycle has
    // already begun capturing finds this false, so that block's samples were
    // never decoded and the request is served by ONE further cycle — at which
    // point the latch is necessarily true, because the flag was set before it
    // was taken.
    bool m_bsCaptureSamples = false;
    // The samples of the cycle being captured. Reserved once; cleared, never
    // reallocated, at each Capturing entry.
    std::vector<float> m_bsSamples;
    // Exactly one EP4 packet arrives 24-61 us after the disable (the one already in
    // usopenhpsdr1.v's WIDE states, which START cannot interrupt); it belongs to the
    // block already emitted. Not cleared on re-arm: if the gate re-arms before it
    // arrives, this flag is all that stops a stale packet seeding the next block.
    bool m_bsTrailingPending = false;
    // The run byte sendBandscopeRunByte() last COMPOSED, recorded before its
    // socket test so the gate's arguments are observable without a socket.
    // Read only by tests, through MetisClientTestAccess. Diagnostic state, not
    // a readback: nothing in Protocol 1 reports what the radio actually holds.
    std::uint8_t m_lastBandscopeRunByte = 0;
    quint64 m_bsBlocks = 0;
    quint64 m_bsTimeouts = 0;
    // The current RUN, cleared by any completed block. m_bsTimeouts above is
    // the session total and never falls.
    int m_bsConsecutiveTimeouts = 0;
    // Since the MOX falling edge; invalid until the first unkey of the session.
    QElapsedTimer m_sinceUnkey;
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
    // ADC-overload numerator and denominator for the current publish window, stamped
    // onto the telemetry and zeroed at each emit. Here because Hl2Telemetry::apply()
    // is a per-response merge with no notion of a window. The only place the true
    // rate exists: downstream sees a ~10 Hz emit of a bit cycling up to ~190 times/s.
    int m_adcWindowSamples = 0;
    int m_adcWindowOverload = 0;
    // Forward-power maximum for the same window; see
    // Hl2Telemetry::forwardPowerPeakRaw for why the last value is not enough.
    ForwardPowerWindow m_fwdWindow;

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
// Same reason again: one accepted bandscope block crosses to the GUI thread
// queued, and Ep4Stats is declared in the Qt-free MetisProtocol.h.
Q_DECLARE_METATYPE(AetherSDR::hl2::Ep4Stats)
