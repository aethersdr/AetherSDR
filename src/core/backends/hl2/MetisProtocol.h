#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// HPSDR Protocol 1 ("Metis") wire primitives for the Hermes-Lite 2 backend.
//
// Direct C++ port of the live-validated prototypes/hl2/hpsdr.py spike (aetherd
// HL2 Phase 1a). Protocol facts (register map, EP2/EP6 framing, LNA gain
// register) are grounded clean-room in openHPSDR Protocol 1, the Hermes-Lite 2
// wiki/gateware, and the pihpsdr reference client (Principle I; see
// THIRD_PARTY_LICENSES). Where a fact is HL2-specific it has been checked
// against the Hermes-Lite 2 gateware RTL, which is the authority for what this
// hardware actually decodes.
//
// This layer is intentionally socket-free and Qt-free so it unit-tests against
// captured/synthetic frames without hardware; MetisClient owns the UDP socket
// and RX thread and calls into these functions.
//
// TRANSMIT: this layer CAN now encode a keyed frame. The old invariant here —
// "every C0 register-address byte is even, so MOX (C0 bit 0) is always 0, so
// these primitives cannot key the radio" — no longer holds, and pretending
// otherwise would be worse than losing it.
//
// What replaces it is a gate one level up: MetisClient will not set MOX unless
// transmit has been explicitly enabled, and hl2_tx_gate_test asserts that with
// the gate off NO emitted frame ever carries C0 bit 0. The encoders below are
// pure functions; encoding a keyed frame is not the same as sending one.

namespace AetherSDR::hl2 {

inline constexpr std::uint16_t kMetisPort = 1024;
// 24-bit signed full scale. (1<<23)-1, not 1<<23: the largest magnitude a
// 24-bit two's-complement sample can actually take is 8388607, and normalising
// by it is what pihpsdr does — matching keeps our dBFS scale identical to the
// reference rather than 0.0000001 dB adrift.
inline constexpr int kFullScale = (1 << 23) - 1;

// EP2 (host->radio) and EP6 (radio->host) are both 1032-byte USB-over-IP frames:
//   EF FE 01 <ep> | seq[4] | frame512 | frame512
// each 512-byte frame: 7F 7F 7F | C0 C1 C2 C3 C4 | 504 payload bytes
inline constexpr std::size_t kUsbPacketSize = 1032;
inline constexpr std::size_t kFrameSize = 512;
inline constexpr std::size_t kFramePayload = 504;     // 63 RX samples * 8 bytes
inline constexpr std::size_t kRxSampleBytes = 8;      // I[3] Q[3] mic[2], 24-bit BE
inline constexpr int kSamplesPerPacket = 126;         // 63 per frame * 2 frames

// C0 register-address bytes (address << 1). Bit 0 is MOX, not part of the
// address, so every constant here is even and keying is applied separately with
// withMox() — see kC0MoxBit.
inline constexpr std::uint8_t kC0Config = 0x00;   // addr 0x00: sample rate + #RX + ADC select
inline constexpr std::uint8_t kC0Rx1Freq = 0x04;  // addr 0x02: RX1 NCO frequency (Hz, 32-bit BE)
inline constexpr std::uint8_t kC0TxFreq  = 0x02;  // addr 0x01: TX1 NCO frequency (Hz, 32-bit BE)
inline constexpr std::uint8_t kC0TxDrive = 0x12;  // addr 0x09: TX drive level + PA/ATU/Alex bits

// MOX lives in C0 bit 0 of EVERY C&C frame, not in a register of its own: the
// radio reads it from whatever bank happens to be in flight. So keying is a
// property of the frame, and every bank has to carry it while transmitting.
inline constexpr std::uint8_t kC0MoxBit = 0x01;

// TX drive level occupies DATA[31:24] (C1). The Hermes-Lite 2 gateware decodes
// only the top nibble [31:28], but the byte-wide field is what the reference
// clients and hpsdrsim both read, so the value is carried as 0..255 and the
// hardware takes the coarse part of it.
inline constexpr int kTxDriveMax = 255;
inline constexpr std::uint8_t kC0AdcGain = 0x14;  // addr 0x0a: AD9866 LNA gain
// addr 0x0e. THIS ADDRESS MEANS TWO DIFFERENT THINGS, which is exactly the
// class of trap the HL2 oracle warns about:
//
//   generic openHPSDR : per-receiver ADC assignment. C1 holds RX1..RX4 (2 bits
//                       each, LSB first), C2 holds RX5..RX7, C3[4:0] TX att.
//   Hermes-Lite 2     : TX LNA gain. [15] enable hardware-managed TX gain,
//                       [14] LNA mode select for it, [13:8] the gain itself.
//
// We send it because the GENERIC meaning is mandatory: a conforming multi-ADC
// device leaves every receiver UNASSIGNED until this arrives and then emits
// correctly framed, correctly paced, all-ZERO IQ — indistinguishable from a
// dead antenna. Verified against hpsdrsim, whose rx_adc[] defaults to -1.
//
// On the HL2 the all-zero payload is inert: bit 15 clear leaves hardware-managed
// TX gain disabled, which is already the default — and transmit has since been
// brought up and verified on air with it left that way, so this is NOT a
// blocker for basic SSB.
//
// It still matters for two things neither of which is implemented: the T/R gain
// switch (the mechanism Quisk uses for fast turnaround) and PureSignal's
// unclipped feedback path. Both need 0x0e to carry a real value, and this round
// robin would zero it every third frame.
inline constexpr std::uint8_t kC0AdcAssignOrTxGain = 0x1C;

// addr 0x39: sync / reset. DATA[7:4] = 0x8 resets every decimation filter
// pipeline; 0x9 also phase-aligns the NCOs (needed for coherent multi-RX).
//
// *** NOTHING SENDS THIS TODAY. WRITING IT WEDGED A RADIO. ***
//
// Sending it after every NCO move meant a pan drag fired ~30 of these per
// second, and the board halted its stream and then stopped answering discovery
// until it was power-cycled. The encoder is kept because its byte layout is
// verified and worth not re-deriving; see MetisClient::requestPipelineReset()
// for the full account and the preconditions for bringing it back.
//
// In particular, do NOT trust the claim that used to stand here — that every
// other field is a command nibble whose "act" encoding has bit 3 set, so
// leaving them zero is "no action". That was inferred from the 0x8/0x9 pattern
// and never checked against the gateware RTL, and this register carries the
// watchdog enable at [27:24] and the master enable at [11:8].
inline constexpr std::uint8_t kC0Sync = 0x72;

// Config-register (C0=0x00) bit flags.
//
// NOTE: neither of these does anything on a Hermes-Lite 2. The HL2 gateware
// decodes only cmd_data[25:24] (sample rate), [6:3] (receiver count), [23:17]
// and [13:11] from this register — C1 bit 6 (cmd_data[30]) and C4 bit 2
// (cmd_data[2]) are not read by any module. They are kept because they are
// meaningful on genuine openHPSDR Hermes/Mercury hardware and are harmless
// here, but do not treat either as load-bearing for the HL2.
inline constexpr std::uint8_t kConfigMercury = 0x40;  // C1 bit6: ADC-as-DDC-source select on
                                                      // openHPSDR Hermes/Mercury. No-op on HL2.
inline constexpr std::uint8_t kConfigDuplex = 0x04;   // C4 bit2: pihpsdr sets this
                                                      // unconditionally. No-op on HL2.

enum class SampleRate : std::uint8_t { R48k = 0, R96k = 1, R192k = 2, R384k = 3 };
int sampleRateHz(SampleRate rate) noexcept;

// A 5-byte Command & Control payload: C0 (register address) + C1..C4 (data).
using Cc = std::array<std::uint8_t, 5>;

// Config register: sample rate + receiver count. Also carries the Mercury and
// duplex bits for openHPSDR compatibility; both are ignored by the HL2 gateware.
Cc ccConfig(SampleRate rate, int numRx = 1) noexcept;
// RX1 NCO frequency in Hz (32-bit big-endian across C1..C4).
Cc ccRx1Freq(std::uint32_t hz) noexcept;
// AD9866 LNA gain in dB, clamped to [-12, +48]; C4 = 0x40 | (dB + 12).
Cc ccRxGain(int db) noexcept;
// Per-receiver ADC assignment (see kC0AdcAssignOrTxGain). Phase 1 runs one receiver on
// ADC0, so every field is zero; the bank still has to be SENT for a conforming
// device to route ADC samples to RX1 at all.
Cc ccAdcAssign() noexcept;
// One-shot filter-pipeline reset. UNUSED — read the warning at kC0Sync before
// calling this from anywhere.
Cc ccPipelineReset() noexcept;

// TX1 NCO frequency in Hz (32-bit big-endian across C1..C4).
Cc ccTxFreq(std::uint32_t hz) noexcept;
// TX drive level (0..kTxDriveMax, carried in C1) plus the onboard PA enable.
//
// PA ENABLE IS 0x09[19], i.e. C2 bit 3, and it is NOT optional for a useful
// transmission: with the PA off the only output is the AD9866's own DAC level,
// which is milliwatts. Measured on hardware — a correct, modulated, keyed
// transmission with the PA disabled produced forward-power counts of zero.
//
// Defaulted OFF so that enabling the power amplifier is always something a
// caller did on purpose.
Cc ccTxDrive(int level, bool paEnable = false) noexcept;
// Set MOX (C0 bit 0) on a C&C bank. Keying is per-FRAME, so this is applied to
// whichever bank is being sent rather than to one dedicated register.
inline Cc withMox(Cc cc, bool keyed) noexcept
{
    cc[0] = static_cast<std::uint8_t>(keyed ? (cc[0] | kC0MoxBit)
                                            : (cc[0] & ~kC0MoxBit));
    return cc;
}

// Write 16-bit I/Q transmit samples into an EP2 packet built by ep2Packet().
//
// Host->radio sample layout is 8 bytes: 32 bits where Hermes put headphone
// audio, then 16-bit I, then 16-bit Q, all big-endian. 63 samples per 512-byte
// frame, two frames per packet.
//
// THE AUDIO SLOT IS NOT AUDIO. The HL2 has no codec, and the FIRST 32-bit
// "audio" word after each frame's C&C bytes is repurposed as EADDR, the
// extended-address register (base 0x3f). memcpy-ing a Hermes TX frame layout
// writes garbage into it. We leave every audio slot zero, which keeps EADDR
// zero, which is what "not using the extended space" has to look like.
//
// Samples beyond what the packet holds are ignored; a short span leaves the
// remainder as transmit silence.
void ep2WriteTxIq(std::array<std::uint8_t, kUsbPacketSize>& pkt,
                  std::span<const std::complex<float>> iq) noexcept;

// Transmit samples carried per EP2 packet (63 per frame, two frames).
inline constexpr int kTxSamplesPerPacket = 126;

// ---- EP6 Command & Control responses (radio -> host) ----
//
// C0[7] is ACK, and it CHANGES HOW THE REST OF C0 IS READ (oracle §5):
//   ACK == 0 (classic, free-running): C0[6:3] = RADDR[3:0], C0[2] Dot,
//            C0[1] Dash (always 0 — the HL2 has no internal keyer), C0[0] PTT.
//   ACK == 1 (reply to our RQST):     C0[6:1] = RADDR[5:0], C0[0] PTT.
//
// The radio free-runs through the classic addresses, so telemetry arrives
// without asking. Verified against hpsdrsim's responder, whose C0 sequence is
// 0, 8, 16, 24, 32 — i.e. RADDR 0..4 at C0[6:3].
struct Ep6Response {
    bool ack = false;
    int raddr = 0;
    bool ptt = false;
    bool dot = false;
    std::uint32_t data = 0;      // C1..C4, big-endian
};

// Decode the C&C bytes of one 512-byte EP6 frame. Returns nullopt if the frame
// is not sync-framed.
std::optional<Ep6Response> parseEp6Response(const std::uint8_t* frame) noexcept;

// Everything the classic response cycle carries. Fields are std::optional
// because each RADDR carries only part of it, so "not seen yet" stays
// distinguishable from "seen and zero" — a forward-power reading of 0 W is a
// real measurement, and rendering it as a dash because we conflated the two
// would be its own bug.
struct Hl2Telemetry {
    std::optional<int>  firmwareVersion;
    std::optional<bool> adcOverload;
    std::optional<bool> txInhibited;      // register bit is ACTIVE LOW; decoded here
    std::optional<int>  txFifoCount;
    std::optional<bool> txFifoUnderflow;
    std::optional<bool> txFifoOverflow;
    std::optional<int>  temperatureRaw;
    std::optional<int>  forwardPowerRaw;
    std::optional<int>  reversePowerRaw;
    std::optional<int>  biasCurrentRaw;
    bool ptt = false;

    // Merge a decoded response in, leaving untouched fields alone.
    void apply(const Ep6Response& r) noexcept;
};

// Standing-wave ratio from raw forward/reverse counts.
//
// The counts are UNCALIBRATED ADC readings, but SWR is a RATIO, so the unknown
// scale factor cancels as long as both come from the same converter — which is
// why SWR is meaningful here while absolute watts are not (oracle §6: "don't
// pretend uncalibrated counts are watts").
//
// Returns nullopt when there is no forward power to speak of: SWR is undefined
// with no carrier, and 1.0 would read as a perfect match rather than "unknown".
std::optional<double> swrFromRaw(int forwardRaw, int reverseRaw) noexcept;

// 64-byte Metis command: EF FE 04 <cmd>. cmd 0x01 = start IQ, 0x00 = stop.
std::array<std::uint8_t, 64> metisCommand(std::uint8_t cmd) noexcept;
// Bit 7 of the run/stop byte is the gateware's watchdog_disable flag
// (Hermes-Lite 2 gateware, rtl/dsopenhpsdr1.v — see THIRD_PARTY_LICENSES):
// 0 = watchdog ENABLED, 0x80 = disabled. We default to ENABLED, which is the
// anti-wedge mechanism: if this client dies without sending a stop, EP2 traffic
// ceases and the radio halts its own stream instead of streaming forever at a
// dead endpoint (after which it stops answering discovery until power-cycled).
inline constexpr std::uint8_t kRunWatchdogDisable = 0x80;

inline std::array<std::uint8_t, 64> metisStart(bool watchdogEnabled = true) noexcept
{
    return metisCommand(static_cast<std::uint8_t>(
        0x01 | (watchdogEnabled ? 0x00 : kRunWatchdogDisable)));
}
inline std::array<std::uint8_t, 64> metisStop(bool watchdogEnabled = true) noexcept
{
    return metisCommand(static_cast<std::uint8_t>(
        0x00 | (watchdogEnabled ? 0x00 : kRunWatchdogDisable)));
}

// 63-byte discovery request: EF FE 02 + 60 zero bytes (broadcast to :1024).
std::array<std::uint8_t, 63> discoveryRequest() noexcept;

struct DiscoveryReply {
    std::array<std::uint8_t, 6> mac{};
    std::uint8_t gatewareVersion = 0;   // raw byte; HL2 gateware e.g. 0x4A -> 7.4
    std::uint8_t boardId = 0;           // 0x06 = Hermes-Lite / Hermes-Lite 2
    bool streaming = false;             // discovery status byte 0x03 = already sending IQ
    // Receiver count the board reports. Only present on full-length replies
    // (>= 21 bytes); 0 means "not reported" and callers should assume 1.
    std::uint8_t numRx = 0;
    [[nodiscard]] bool isHermesLite2() const noexcept { return boardId == 0x06; }
};
// Parse a >=60-byte Metis discovery reply (EF FE <st> MAC[6] gwver board ...).
std::optional<DiscoveryReply> parseDiscoveryReply(std::span<const std::uint8_t> pkt) noexcept;

// Build a 1032-byte EP2 packet carrying two C&C registers (one per frame). The
// 504-byte payload is zero-filled, which is transmit SILENCE — ep2WriteTxIq()
// overwrites it when there is audio to send. The zero fill is also what keeps
// EADDR clear; see ep2WriteTxIq.
std::array<std::uint8_t, kUsbPacketSize> ep2Packet(std::uint32_t seq, const Cc& a,
                                                   const Cc& b) noexcept;

// Cheap header read: the EP6 sequence number, or nullopt if not an EP6 packet.
// Used for drop counting without decoding samples.
std::optional<std::uint32_t> ep6Seq(std::span<const std::uint8_t> pkt) noexcept;

// Decode an EP6 packet's IQ samples (24-bit signed big-endian, normalized to
// [-1, 1)) and append them to `out`. Returns the count appended, or -1 if `pkt`
// is not a valid EP6 packet (wrong length/header). Does not remove the DC
// offset — that is the DSP layer's job.
int ep6Samples(std::span<const std::uint8_t> pkt,
               std::vector<std::complex<float>>& out) noexcept;

}  // namespace AetherSDR::hl2
