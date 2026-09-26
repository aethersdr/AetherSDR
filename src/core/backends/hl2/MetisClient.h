#pragma once
#include "core/TxCoordinator.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QTimer>
#include <QList>
#include <QSet>
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
        // Whether the GATE IS ACTUALLY RUNNING, read off m_params.bandscope at
        // publish time rather than echoed by whoever asked for it. The backend
        // used to mirror its own request, which could disagree with the client:
        // an enable posted across the thread hop just as the silence watchdog
        // cleared the intent left the health row saying one thing and the gate
        // doing another. Riding the counters makes the row true by
        // construction, at the cost of up to kLinkPublishIntervalMs of lag —
        // which is the honest reading, because until the client has it there
        // is nothing running to report. (PR #5650 review round 3.)
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
        // ---- EP6 silence recovery ----
        //
        // How many times the EP6 silence watchdog re-sent the run command
        // instead of declaring the link down, and how many of those the stream
        // came back from. ATTEMPTS WITHOUT COMPLETIONS is the shape that says
        // this recovery is not the right one for whatever is actually failing,
        // and it is only readable if both numbers travel together.
        //
        // On LinkCounters rather than behind getters of their own, because a
        // counter nobody reads records nothing: this struct is the file's
        // established path to the operator. Hl2Backend mirrors it from
        // linkCountersUpdated onto the GUI thread, healthSnapshot() turns it
        // into rows, and RadioHealthDialog::refresh() and the automation
        // bridge's `health` verb both iterate those rows generically -- so a
        // new field arrives on both with no registration anywhere. (NOT the
        // support bundle: SupportBundle::createBundle writes logs, system info,
        // radio info and settings, and no health rows at all. The dialog's own
        // copy-to-clipboard is the path a number takes into a bug report.) The
        // publish is also what makes the read thread-safe. A RECOVERY THAT SUCCEEDED IS
        // INVISIBLE EVERYWHERE ELSE by design -- m_linkUp never drops, so no
        // signal fires and no pane republishes -- so these two rows are the
        // only record that the audio's hole had a cause.
        //
        // Session-life, zeroed with the rest of m_link at start() and NOT at
        // stop(): a stop is not a reason to forget that a recovery happened.
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

    // Lock the VersaClock to an external 10 MHz reference at CL1, or return it
    // to the onboard crystal. Queues the twenty-four-bank reprogramming
    // sequence; see versaClockCl1Banks().
    Q_INVOKABLE void setCl1RefClock(bool externalRef);

    // Raise or clear the gateware's ATU tune request (0x09[20]). Rides the
    // drive-level bank, so this restates the current drive rather than being a
    // register of its own.
    Q_INVOKABLE void setAtuTuneRequest(bool request);
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
    // ONE bandscope block, on demand, with its 2048 samples kept.
    //
    // A SEPARATE THING FROM THE GATE ABOVE, and deliberately so. The gate is a
    // standing 1 Hz sampler for the headroom rows and keeps only statistics;
    // this raises wide_spectrum for the length of exactly one arming cycle —
    // ~13 ms measured, two block intervals — decodes that block's samples and
    // lowers it again. It does not start the period timer, does not touch
    // m_params.bandscope, and leaves nothing running behind it.
    //
    // WHY ON DEMAND AND NOT A SECOND STREAM: a continuous consumer would put a
    // second permanent load on this thread, which already carries EP2 pacing,
    // EP6 ingest, WDSP and the panadapter FFT. That cost has never been
    // measured — the plan's open question §3.4, which bench runs d94 and d95
    // did NOT answer because neither drove the radio with this code. A request
    // per frame avoids the question rather than guessing the answer to it.
    //
    // Answered exactly once, by bandscopeFrameReady or bandscopeFrameFailed.
    // A second request while one is outstanding is ignored: the caller already
    // has an answer coming.
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

    // ---- TX IQ FIFO fault accounting ----
    //
    // The FIFO is bounded ABOVE at kTxQueueMax and not below, and both ends
    // silently alter what goes on the air: overflow drops the oldest samples
    // (a discontinuity), underflow substitutes transmit silence for samples
    // that never arrived (a step to zero mid-envelope). queueTxIq's own
    // documentation above says so, and until these counters existed NOTHING
    // said it had happened -- not a log line, not a reading, not a test.
    //
    // THESE COUNT; THEY DO NOT REPAIR. The truncation is still exactly what it
    // was, deliberately: giving the FIFO a floor (a pre-roll, a target depth,
    // or a held/ramped last sample instead of a step to zero) changes what
    // this client transmits, and that is a separate decision from being able
    // to see the fault at all. Counting first is what makes that decision
    // measurable rather than argued.
    //
    // The spectral cost is measured, not assumed: on EP2 captures of live
    // speech, windows containing no starvation give 78.6-78.8 dB
    // opposite-sideband suppression and windows containing one give
    // 33.7-35.2 dB. A SINGLE zeroed sample takes it from 78.99 dB to 48.97 dB.
    // Gating a one-sided analytic spectrum with a real gate is an image
    // generator, and the gate here is the zero fill.
    //
    // PROVENANCE, because those numbers will otherwise be read as on-air
    // figures and they are not. The captures are EP2 traffic off the wire, but
    // the peer was hpsdrsim on loopback rather than a radio, so the producer
    // clock is not a live sound card. What they establish is that the
    // starvation HAPPENS in ordinary speech and what it costs WITHIN one
    // capture; they do not establish the on-air magnitude. Nor is 78.7 dB the
    // modulator's own suppression -- on a steady tone the same instrument reads
    // 87.15-87.19 dB, matching Hl2TxDsp::designFilters. 78.7 is the clean-window
    // baseline for speech through this instrument, which is what makes the
    // 33.7-35.2 dB comparison meaningful.
    //
    // WHY THE RADIO'S OWN FIFO TELEMETRY CANNOT SEE THIS, which is the trap:
    // Hl2Telemetry::txFifoFillMsbs and ::txFifoRecovery report the GATEWARE's
    // DSIQ FIFO -- 16384 deep on this board, with the 0-127 reading being the
    // TOP SEVEN BITS of its fill level rather than its depth or a sample count,
    // as MetisProtocol.h says at the decode -- and that FIFO is fed by EP2
    // PACKET ARRIVALS. An
    // underflow here does not drop a packet or shorten one -- onEp2PacerTick
    // emits a full-size EP2 frame on the wall clock either way, and
    // ep2WriteTxIq zero-fills the samples that were not supplied. The radio
    // therefore receives an unbroken 48 kHz sample stream whose CONTENT is
    // partly silence, and its FIFO fill is identical in both cases. A healthy
    // gateware FIFO reading is not evidence that this did not happen.
    //
    // AND THE BLINDNESS IS SYMMETRIC, which matters just as much and is the
    // easier half to forget. These counters see the HOST end of the CLIENT's
    // queue and nothing else. They are incremented before the socket write, so
    // a datagram lost on the wire, a genuine overrun of the gateware's DSIQ
    // FIFO, or anything else that happens after this client hands the bytes to
    // the socket is invisible to THEM exactly as a host starvation is invisible
    // to the gateware rows. The two instruments are DISJOINT, not overlapping:
    // neither can stand in for the other, and a fault occurring between the
    // socket write and the FIFO is invisible to both. "The counters are clean"
    // is not evidence that the transmission was.
    //
    // Underflow is counted only on the queued-IQ path while keyed. The CW and
    // test-tone paths synthesise a whole block per packet and cannot starve,
    // and an UNKEYED frame carries silence by design rather than by fault.
    //
    // Packets and samples are both counted because they separate two different
    // faults that the sample count alone conflates: a burst of whole-packet
    // underflows at key-down is the missing pre-roll (the queue has not been
    // primed yet), while occasional PARTIAL packets mid-over are the pacer and
    // the audio device clock drifting apart. Same counter, different shapes.
    //
    // THESE DO NOT ANSWER "WAS THIS OVER CLEAN", and the shape of the error is
    // a FALSE POSITIVE rather than a miss. THE FLOOR HAS TWO HALVES, at the two
    // ends of the over, and the first published description of it named only
    // the smaller one.
    //
    //   THE TAIL, at key-up. Real audio does not end on a packet boundary, so
    //   the last packet of a perfectly healthy over is short and is counted: at
    //   the moment the count is taken, "the queue is short" and "the over is
    //   over" are indistinguishable here. ONE underflow packet, plus
    //   1..kTxSamplesPerPacket-1 samples.
    //
    //   THE PRE-ROLL, at key-down, and it is the BIGGER half. It is the same
    //   missing pre-roll the paragraph above names, read as a floor rather than
    //   as a fault: Hl2Backend::applyKeying posts setMox to this client
    //   immediately, while the first block of transmit IQ cannot arrive until
    //   the audio chain has produced one -- Hl2TxDsp works in whole
    //   Config::dspBlockSize blocks at Config::inputSampleRateHz, and nothing
    //   primes m_txIq before MOX. Every tick in between takes the empty-queue
    //   arm and counts a WHOLE packet. So a clean over OPENS with a burst of
    //   whole-packet underflows and CLOSES with the short tail, and the burst
    //   is worth an order of magnitude more silent samples than the tail is.
    //
    // Both halves are measured and pinned in hl2_tx_gate_test rather than
    // described, so that changing either is a change that announces itself.
    // Neither is a defect in this client: they are what an unprimed,
    // wall-clock-paced FIFO reads on a healthy transmission, and they are the
    // reason these counters are a session budget and not a per-over verdict.
    //
    // txUnderflowPackets is the more polluted of the two: its floor is a fixed
    // per-over cost that does not grow with the over's length, while a real
    // starvation adds kTxSamplesPerPacket per packet for as long as it lasts.
    //
    // "TOTALS" MEANS SINCE PROCESS START. Nothing resets these -- not start(),
    // not a link edge, not flushTxIq() -- so they accumulate across reconnects
    // and, if the operator switches radios without restarting, across radios.
    // That is deliberate rather than an oversight, and the log line says so in
    // those words: the question they answer is "how much substituted silence
    // has this PROCESS put on the air", and a reset on a link edge would erase
    // exactly the slow accumulation they exist to show. An operator who wants a
    // per-link figure subtracts two readings.
    //
    // Suppressing either half of the floor would need the count deferred and
    // attributed against the key edges, which is a design change and is not
    // made here; the honest floor is a reportable state in the meantime.
    //
    // THREAD CONTRACT: I/O THREAD ONLY. See the note on the logging category in
    // MetisClient.cpp -- these are plain integers written by the EP2 pacer, and
    // reading them from anywhere else (healthSnapshot() is the obvious
    // temptation) is a cross-thread read of a non-atomic. The only caller today
    // is hl2_tx_gate_test, on one thread.
    [[nodiscard]] std::uint64_t txUnderflowPackets() const noexcept { return m_txUnderflowPackets; }
    [[nodiscard]] std::uint64_t txUnderflowSamples() const noexcept { return m_txUnderflowSamples; }
    [[nodiscard]] std::uint64_t txOverflowSamples() const noexcept { return m_txOverflowSamples; }

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
    // The block requestBandscopeFrame() asked for: kEp4BlockSamples converter
    // codes, contiguous in converter time (26.67 us of the 76.8 MSPS ADC),
    // normalised to [-1, 1) by kEp4FullScale.
    //
    // A QList because it crosses the I/O thread to the GUI thread queued and a
    // std::vector has no metatype; 8 KB per frame, and there is one frame per
    // request. Carried SEPARATELY from bandscopeBlockReady rather than folded
    // into Ep4Stats, because the statistics are emitted on every gated cycle
    // and the samples are not: decoding 2048 codes costs this thread real work
    // (an arithmetic shift per sample plus the copy) and nothing should pay it
    // unless something asked for a picture.
    //
    // UNCALIBRATED AND PRE-DDC, exactly as bandscopeBlockReady's are.
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
    // `drops` is what ep4SeqStep() charged for the gap BEFORE this packet.
    // The gate needs it because `seq % kEp4PacketsPerBlock` cannot see a loss
    // of a multiple of four: the phase survives it intact.
    // A ptt_resp transition reported by the radio: the bandscope's interlock
    // edges for keying this client did not initiate.
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
    void sendPrimingBurst(int countPerBank);
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
    // THE START RETRY'S WHOLE BUDGET MUST FIT INSIDE THE SILENCE WINDOW, and
    // two live call sites rest on it rather than one. setReceiverCount()'s
    // comment says why for the restart path -- a budget that outlasted the
    // window would let the watchdog fire while a retry was still pending, and
    // the retry would be dead code. onWatchdogTick()'s stage 1 rests on the
    // same inequality from the other side: it seeds m_startAttempts = 1 and
    // then uses `m_startRetryTimer->isActive()` as its "the attempt still has
    // time" test, which is only a bounded wait while the budget is shorter than
    // the window that produced it. Pinned here because both readings are
    // comments about constants declared in this file, and a comment does not
    // fail a build.
    static_assert(kMaxStartAttempts * kStartRetryMs < kSilenceTimeoutMs,
                  "the start-retry budget must expire inside one silence window");

    QTimer* m_ep2Timer = nullptr;         // paces EP2 off the wall clock
    QTimer* m_watchdogTimer = nullptr;    // EP6 silence detection
    QTimer* m_connectWatchdog = nullptr;  // single-shot: first-EP6 deadline
    QTimer* m_startRetryTimer = nullptr;  // re-sends metis-start until EP6 flows
    int     m_startAttempts = 0;          // start datagrams sent this connect
    QElapsedTimer m_ep2Clock;             // pacer reference clock
    QElapsedTimer m_sinceLastEp6;         // silence detection
    // A recovery is in flight for the CURRENT silence. Set when the watchdog
    // re-sends the run command, cleared by the EP6 packet that ends the
    // silence -- so it is per-silence, not per-session, and a link that goes
    // quiet twice gets two recoveries rather than one. Also cleared by stop()
    // and by setReceiverCount(), which restarts the stream by its own route and
    // must not have its EP6 credited to this one.
    bool m_silenceRecoveryArmed = false;
    // NO TEST SEAM HERE, AND THAT IS A STOP-GAP RATHER THAN A POSITION.
    // AGENTS.md routes a disconnected input to a socket-free test that injects
    // the transport, and lists hl2_receiver_count_restart_test's fake Metis
    // radio among four legacy exceptions tracked for extraction in #5254. This
    // coverage grows that exception. The reason it does is mechanical and not a
    // preference: stage 1's only observable is a datagram, it goes out through
    // m_socket, and there is no injectable datagram sink beside feedDatagram()
    // to observe it with. The socket-free shape #5254 wants needs that sink
    // first; when it lands, this belongs behind it.
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
        return !m_params.radioSerial.isEmpty()
            && m_cl1MaybeOn.contains(m_params.radioSerial);
    }

    // EVERY RADIO THIS PROCESS MAY HAVE LEFT ON CL1, by serial. The only record
    // that a still-powered radio is running from an external reference the
    // operator has since cleared; nothing on the wire can be asked. Survives
    // stop() and start() on purpose.
    //
    // A SET AND NOT ONE SERIAL. With a single QString, switching a SECOND radio
    // on overwrote the first one's record, and the first radio then never got
    // its off table — it stayed on CL1 with nothing left that knew (#5923
    // review). This object outlives a radio swap, so the record has to as well.
    //
    // ADDED PESSIMISTICALLY, at queue time, because a sequence interrupted
    // half-way may have switched the part already. REMOVED ONLY ON PROOF: see
    // m_cl1BanksUnsent.
    QSet<QString> m_cl1MaybeOn;
    // The sequence currently draining out of m_oneShot: whose it is, whether it
    // is the OFF table, and how many of its banks have not yet been CONFIRMED
    // SENT. Only when the last one is confirmed does its radio leave
    // m_cl1MaybeOn.
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
    // Contiguous FULL packets since the last short one.
    //
    // WHEN A STARVATION EPISODE IS OVER, and it is NOT "the next full packet".
    // The first version of this flushed the run on the very next full packet.
    // That is right for the burst shape -- the key-down pre-roll: N short
    // packets in a row, then the queue primes and stays primed -- and wrong for
    // the other shape this FIFO produces, which the accounting comment above
    // names itself: the pacer and the audio device clock drifting apart gives
    // ALTERNATING short and full packets. Flushing on every full packet turns
    // that into one ~400-character debug line per PAIR, up to ~190 lines a
    // second at the pacer's ~381 frames/s, which is precisely the "one line per
    // EP2 frame" outcome the coalescing exists to prevent -- and the failure
    // mode of #5813, where a high-rate burst flushed the support-log tail it
    // was meant to explain.
    //
    // So an episode ends when the queue has KEPT UP for about 100 ms, DERIVED
    // from the EP2 rate rather than typed as a packet count, plus the
    // unconditional flushes at every exit from the queued-IQ path: the key
    // going up, flushTxIq(), CW taking the stream, the test tone taking it.
    // Within an over that gives one line per episode; across overs the unkey
    // backstop still guarantees the line is attributed to the over it happened
    // in rather than to the next one.
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
