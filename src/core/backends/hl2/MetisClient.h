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

#include "core/backends/hl2/Hl2ControlRequest.h"
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
// TX remains fail-closed: enableTransmit() must explicitly open the final wire
// gate before either MOX or transmit samples can leave this object.
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
        // Whether the DUTY-CYCLE GATE should be running — the operator's
        // standing intent, not the instantaneous state of the run byte, which
        // the gate raises and lowers about sixteen times a second's worth of
        // packets in every second.
        //
        // It lives in Params for the same reason ocFilterByte does, and the
        // reason is measured rather than tidy: setReceiverCount() tears the
        // stream down with 0x00 and brings it back with 0x01, and RUNSTOP
        // clears `wide_spectrum` along with `run`. Without carrying it here,
        // adding a panadapter would silently switch the sensor off and nothing
        // would say so. See setSampleRate()'s standing comment: a field that
        // shares a rebuild with another has to be carried, not re-defaulted.
        bool bandscope = false;
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
        // ---- the duty-cycle gate ----
        // Blocks the gate ACCEPTED — four in-phase packets, after the stale
        // block was flushed. Not ep4Packets/4: most of what arrives is armed,
        // flushed or trailing, and none of that becomes a reading.
        quint64 bandscopeBlocks = 0;
        // Arming cycles abandoned because no complete block arrived inside
        // bandscopeGuardMs(). Should read zero in steady state, but a non-zero
        // value does NOT on its own mean the radio stopped answering the run
        // byte: bandscopeGuardMs() names two unmeasured multi-receiver cases
        // that can fire it benignly — the arming delay being clocked by EP6
        // samples rather than packets, and the EP4 rate at two, and at four or
        // more, receivers. Read its assumptions before suspecting hardware.
        quint64 bandscopeTimeouts = 0;
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
    // Latched into the config register, and that is the whole mechanism: the
    // config bank rides bank A of EVERY EP2 frame, so the relays follow within
    // one frame (~2.6 ms at 48 kHz). Filter switching that lags the retune is
    // the failure mode that matters on transmit, and bank A is what prevents it.
    //
    // Deliberately NOT also queued as a one-shot. A one-shot fills bank B, which
    // the radio applies AFTER bank A, and it would carry a SNAPSHOT -- so a
    // copy queued here would overwrite the live config for that frame with
    // whatever the register held at the moment the band changed. See the
    // definition and aethersdr/AetherSDR#4579.
    // Takes int, not uint8_t: this crosses threads via
    // QMetaObject::invokeMethod, which matches Q_ARG against the type name moc
    // recorded from this declaration, and `std::uint8_t` does not normalize to
    // the same string as `unsigned char`.
    Q_INVOKABLE void setBandFilter(int ocFilterByte);
    // Push the TRANSMIT frequency to the HL2 IO Board (I2C2 chip 0x1D) so an
    // attached amplifier, antenna relay or transverter follows the band.
    //
    // Five one-shot banks, LSB last, because that register is what commits the
    // value on the board — see ccIoBoardTxFrequency(), which owns the ordering.
    // They ride m_oneShot rather than the rotation for the same reason the
    // filter byte does: an amplifier still switched to the previous band when
    // the operator keys is the failure that matters, and the deque both
    // preserves order and drains one bank per EP2 frame (~2.6 ms at 48 kHz),
    // so all five land in about 13 ms.
    //
    // SENT UNCONDITIONALLY, with no "do you have an IO board" setting, on the
    // same reasoning the J16 filter byte is driven blind: a chip that is not on
    // the bus NACKs its address, the gateware's i2c_master raises missed_ack
    // and moves on. Costing an absent board nothing is what makes the setting
    // unnecessary, and a setting defaulted off is a support burden — the
    // symptom of forgetting it is an amplifier on the wrong band.
    //
    // quint64, not std::uint32_t: the board's field is 40 bits wide.
    Q_INVOKABLE void setIoBoardTxFrequencyHz(quint64 hz);
    [[nodiscard]] std::uint8_t bandFilter() const noexcept { return m_params.ocFilterByte; }
    // Queue a one-shot filter-pipeline reset (MetisProtocol kC0Sync) to be sent
    // on the next EP2 frame, ahead of the round robin.
    Q_INVOKABLE void requestPipelineReset();

    // ---- RQST/ACK (docs/HERMES.md §13 item 13, oracle §5) ----
    //
    // Ask the radio to acknowledge one C&C register write. Read
    // Hl2ControlRequest's header before using this: it is NOT a read, NOT an
    // RPC, and it can be refused for four separate reasons, all of which a
    // caller has to be prepared for.
    //
    // Returns FALSE, having sent nothing, when:
    //   - the stream is not running or no EP6 has arrived. Response slots exist
    //     only inside the EP6 frame the radio emits while `run` is set, so an
    //     idle radio would never answer and reporting a timeout for that would
    //     be a lie about the hardware;
    //   - a request is already outstanding (single outstanding, no queue);
    //   - a previous request timed out and its quarantine has not elapsed;
    //   - the address is not on the ALLOW-LIST in the .cpp. Not a deny-list:
    //     the set of RQST-able addresses is enumerated, so an address nobody
    //     considered is refused by default rather than permitted by default.
    //
    // TODAY THAT LIST IS 0x0a (AD9866 RX LNA gain) and 0x0e (ADC assign / TX
    // LNA gain). Both are RE-ASSERTED by the round robin, which is the rule the
    // list is built on. The reason for each, and the reason for the ones
    // deliberately left off — 0x01 the TX NCO, 0x09 TX drive/PA, 0x39
    // sync/reset, 0x3b the raw AD9866 SPI write, 0x3c/0x3d the two I2C buses —
    // is written at the list itself. Read it before adding one.
    //
    // WHAT IS GUARANTEED, exactly. This method cannot key a transmitter:
    // ccRegister() leaves C0[0] clear, the RQST bit is C0[7], and withRespRqst()
    // touches nothing else, so the transmit gate is untouched whatever the
    // address. That is a NARROWER claim than "the dangerous registers are
    // handled", and the narrow one is the true one — what keeps this method away
    // from the companion-board I2C bus (amplifiers, antenna relays,
    // transverters) is the allow-list and nothing else. Widening the list
    // widens the blast radius; widening it is not a refactor.
    //
    // `subsystemRead` selects Hl2ControlRequest::Echo::SubsystemRead, whose
    // reply carries the value READ instead of an echo of what was written. On
    // this gateware that is the two I2C commands (0x3c/0x3d) and nothing else,
    // and neither is on the allow-list — so TODAY THIS ARGUMENT IS REFUSED, not
    // honoured. It stays in the signature because the shape is right and item
    // 19's config EEPROM will need it; it is refused because on an address
    // whose reply IS an echo it would discard the data half of the match and
    // leave a six-bit address as the whole correspondence, which is precisely
    // the pairing the quarantine cannot always catch.
    Q_INVOKABLE bool requestRegister(int addr, quint32 data, bool subsystemRead = false);

    // I/O-THREAD ONLY, like linkCounters(). Returned by reference because the
    // machine holds no implicitly-shared members; GUI-thread consumers read the
    // controlReply* signals instead.
    [[nodiscard]] const Hl2ControlRequest& controlRequest() const noexcept
    {
        return m_ccRequest;
    }
    // Run or stop the wideband bandscope's DUTY-CYCLE GATE (endpoint 0x04).
    //
    // What this turns on is a SAMPLER, not the stream. Left running
    // continuously the bandscope is 380.95 datagrams a second — measured, flat
    // to 3 ppm across 48/96/192/384 kHz — which is ~3.3 Mbit/s of wire rate
    // beside EP6, about as much again as the IQ stream itself at 1 RX / 48 kHz
    // (see the table at kEp6LinkBudgetFraction). The gate raises the run byte's
    // wide_spectrum bit once per kBandscopeSampleMs, keeps ONE block, and lowers
    // it again: TWELVE datagrams a second, 0.11 Mbit/s.
    //
    // Twelve, not sixteen, and the count is the gate's own arithmetic rather
    // than a block's: a steady-state cycle resumes at phase 1 (capture ends on
    // phase 3, the trailing packet is phase 0), so 3 are discarded in Arming
    // waiting for the boundary, 4 are flushed, 4 are kept, and 1 trailing
    // packet follows the disable — 3+4+4+1. hl2_ep4_gate_test section 6 walks
    // exactly that cycle. 0.11 Mbit/s is on the same wire-byte basis as the 3.3
    // above (payload + UDP/IP/Ethernet + IFG, as ep6BitsPerSecond counts it);
    // payload alone it is 0.10. (PR #5650 review, K5PTB.)
    //
    // That the gate exists for BANDWIDTH and not for stream integrity is now a
    // measurement rather than a hope. Bench run d95 (Procedure B) found the
    // bandscope costs EP6 nothing: 0 drops in 2,113,847 EP6 datagrams with it
    // on against 0 in 3,123,233 with it off, gap distribution unchanged. There
    // is no correctness argument for a shorter duty cycle and no integrity
    // argument against a longer one.
    //
    // DEFAULT OFF, and there is no UI and no setting: this is reached only
    // through Hl2Backend::invokeExtension("hl2", "bandscope.enable", ...).
    //
    // Re-asserting `run` in the same byte is a no-op in the gateware's decode,
    // so nothing here restarts or perturbs the IQ stream. It is a no-op unless
    // the stream is already running: the run byte is only meaningful to a radio
    // that has been started, and metisStop() clears both bits anyway.
    Q_INVOKABLE void setBandscopeEnabled(bool on);

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
    // Whether the GATE is running — the operator's standing intent, which is the
    // only bandscope state that is stable long enough to report. The run byte
    // itself is up for only ~29 ms in every kBandscopeSampleMs — the 11 packets
    // a cycle consumes before the disarm, at the measured 2.625 ms apiece, plus
    // the 2.4-2.6 ms mid-stream arming latency — and nothing in Protocol 1 reads
    // it back, so a readout of the instantaneous bit would be both unknowable
    // and useless. (~11 ms here was one block's worth, the same undercount as
    // the "16 datagrams" above; found while deriving that one.)
    //
    // Survives setReceiverCount()'s stop/start (Params::bandscope); cleared by
    // start() and stop().
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

    // Software CW for a PC/USB/MIDI keyer. The carrier is generated in the EP2
    // packet builder so its envelope is sample-paced by the radio's fixed
    // 48 kHz transmit stream rather than by GUI or producer-thread timing.
    // It still requires MOX; Hl2Backend owns break-in and manual-PTT policy.
    Q_INVOKABLE void setCwKeyDown(bool down);
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
    // temperature, TX FIFO status, ADC overload, PTT. Free-running, so it
    // arrives without us issuing a request.
    //
    // "status", not "depth": the wire carries a recovery flag plus the top 7
    // bits of the fill level, and no sample count at all. See
    // Hl2Telemetry::apply().
    void telemetryUpdated(const AetherSDR::hl2::Hl2Telemetry& t);
    // ONE ACCEPTED BANDSCOPE BLOCK — 2048 contiguous converter samples, merged
    // from the four in-phase EP4 packets the gate kept. Never per packet: a
    // packet is a quarter of the record and its peak is not the record's.
    //
    // UNCALIBRATED, PRE-DDC. The levels in here are on the AD9866's own scale,
    // commensurable with the gateware's clip and good-level flags and with
    // nothing else. No antenna-referred comparison has been run (the study's
    // Procedure C, which needs a live antenna and is not scheduled), so nothing
    // downstream may treat these as absolute — and per IRadioBackend.h's own
    // rule for the rows they feed, nothing may make a DECISION from them at all.
    void bandscopeBlockReady(const AetherSDR::hl2::Ep4Stats& block);
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
    // One arriving EP4 datagram, offered to the gate. Separate from handleEp4's
    // counters on purpose: those describe the WIRE and count every datagram
    // whatever the gate is doing, and a reading is a different thing from an
    // arrival.
    //
    // Not noexcept, and neither is handleEp4 any longer: this one EMITS, and a
    // queued connection copies the block into a QVariant to cross the thread.
    void bandscopeOnPacket(std::uint32_t seq, std::span<const std::uint8_t> bytes);
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
    void sendPrimingBurst(int countPerBank);
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
    // Free-running from construction and never restarted: the RQST/ACK floor
    // differences it, so it must not be reset under an outstanding request the
    // way m_sinceLastEp6 is per packet.
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
    Cc m_ccTxDrive{};

    bool m_txAllowed = false;   // gate; see enableTransmit()
    std::deque<std::complex<float>> m_txIq;   // pending transmit samples
    // Roughly a quarter second at 48 kHz. Past this the operator is hearing
    // latency, so dropping is better than growing the backlog.
    static constexpr std::size_t kTxQueueMax = 12000;
    double m_toneHz = 0.0;
    double m_toneAmp = 0.0;
    double m_tonePhase = 0.0;   // radians, carried across packets
    bool m_cwMode = false;      // while true, CW owns TX IQ (silence between elements)
    bool m_cwKeyDown = false;
    double m_cwEnvelope = 0.0;  // 0..1 raised-cosine ramp position
    bool m_mox = false;         // requested key state, only honoured if m_txAllowed

    std::uint32_t m_txSeq = 0;           // outgoing EP2 sequence
    unsigned m_roundRobin = 0;
    // One-shot C&C banks, drained one per EP2 frame BEFORE the round robin.
    // Ordering matters: a frequency change and its pipeline reset must reach the
    // radio in that order, and neither should wait up to three frames for the
    // rotation to come back around.
    friend struct MetisClientTestAccess; // socket-free transport-state injection
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
    // EXACTLY ONE EP4 PACKET ARRIVES AFTER THE DISABLE, 24-61 us later, in four
    // of four measured cycles: the packet already inside usopenhpsdr1.v's WIDE
    // states, which START cannot interrupt. It belongs to the block we have
    // already emitted.
    //
    // Consumed and discarded by the gate wherever it lands. It is deliberately
    // NOT cleared when the next cycle arms: the trailing packet is the previous
    // capture's last word, and if the gate re-armed before it was seen — a
    // shorter sample period, or a stalled event loop — the flag is the only
    // thing stopping a packet seconds old from seeding the next block. If it
    // is instead LOST, the flag eats the first packet of the next cycle, which
    // costs one block interval out of a guard sized in hundreds.
    bool m_bsTrailingPending = false;
    // The run byte sendBandscopeRunByte() last COMPOSED, recorded before its
    // socket test so the gate's arguments are observable without a socket.
    // Read only by tests, through MetisClientTestAccess. Diagnostic state, not
    // a readback: nothing in Protocol 1 reports what the radio actually holds.
    std::uint8_t m_lastBandscopeRunByte = 0;
    quint64 m_bsBlocks = 0;
    quint64 m_bsTimeouts = 0;
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
    // ADC-overload numerator and denominator for the CURRENT publish window.
    //
    // They live here rather than in Hl2Telemetry because they are window
    // accumulators owned by this loop, while Hl2Telemetry::apply() is a
    // per-response merge that has no idea where a window begins. They are
    // stamped onto the telemetry struct and zeroed at each emit.
    //
    // THIS IS THE ONLY PLACE THE RATE STILL EXISTS. Everything downstream sees
    // the coalesced ~10 Hz emit, which samples a bit that cycles up to ~190
    // times a second -- so a consumer that counted overloads there would be
    // measuring its own sampling phase. See Hl2Telemetry's own comment.
    int m_adcWindowSamples = 0;
    int m_adcWindowOverload = 0;

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
