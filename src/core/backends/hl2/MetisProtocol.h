#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// HPSDR Protocol 1 ("Metis") wire primitives for the Hermes-Lite 2 backend.
// Socket-free and Qt-free; MetisClient owns the UDP socket and RX thread.
// HL2-specific facts are checked against the Hermes-Lite 2 gateware RTL, the
// authority for what the hardware decodes (see THIRD_PARTY_LICENSES).
//
// TRANSMIT: these encoders CAN produce a keyed frame (withMox). The gate is in
// MetisClient, which never sets MOX unless transmit is explicitly enabled;
// hl2_tx_gate_test asserts no frame carries C0 bit 0 with the gate off.

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
inline constexpr std::size_t kFramePayload = 504;

// EP6 payload is a sequence of ROUNDS: [ I(3) Q(3) ] x numRx | mic(2), so a
// round is 6*numRx + 2 bytes. A round never straddles a 512-byte frame: the
// gateware zero-pads the tail (usopenhpsdr1.v, MIC0 -> `(byte_no[8:0] >
// round_bytes) ? RXDATA2 : PAD`), so rounds-per-frame is a floor division.
// numRx=4: 19 rounds (494 bytes) + 10 pad bytes.
inline constexpr std::size_t kRxIqBytes   = 6;   // 24-bit BE I + 24-bit BE Q
inline constexpr std::size_t kRoundMicBytes = 2; // mic / VNA word closing a round

// Highest receiver count this protocol layer will encode. The gateware field is
// 4 bits (0x00[6:3], 0000=1 .. 1011=12); the shipping hl2b5up_main variant is
// built with NR=4 and reports that at discovery byte 0x13. Never assume this
// number — clamp against what the board reported (MetisClient::Params).
inline constexpr int kMaxReceivers = 12;

constexpr std::size_t ep6RoundBytes(int numRx) noexcept
{
    if (numRx < 1) numRx = 1;
    return static_cast<std::size_t>(numRx) * kRxIqBytes + kRoundMicBytes;
}

constexpr int ep6RoundsPerFrame(int numRx) noexcept
{
    return static_cast<int>(kFramePayload / ep6RoundBytes(numRx));
}

// Samples PER RECEIVER in one EP6 packet (two frames).
constexpr int ep6SamplesPerPacket(int numRx) noexcept
{
    return 2 * ep6RoundsPerFrame(numRx);
}

// Sustained EP6 packet rate, in packets/second, for a sample rate and receiver
// count. Rises with BOTH: adding receivers shrinks the per-receiver payload of
// a fixed-size packet, so the radio sends more of them.
constexpr double ep6PacketsPerSecond(int sampleRateHz, int numRx) noexcept
{
    const int perPacket = ep6SamplesPerPacket(numRx);
    return perPacket > 0 ? static_cast<double>(sampleRateHz) / perPacket : 0.0;
}

// Sustained EP6 wire rate in bits/second, including UDP (8), IPv4 (20) and
// Ethernet (14 + 4 FCS) headers plus the 20-byte preamble/inter-frame gap.
// The HL2 link is 100BASE-T and drops rather than refuses, gapping every
// receiver at once; Hl2Backend applies the policy.
constexpr double ep6BitsPerSecond(int sampleRateHz, int numRx) noexcept
{
    constexpr double kWireBytesPerPacket = kUsbPacketSize + 8 + 20 + 18 + 20;
    return ep6PacketsPerSecond(sampleRateHz, numRx) * kWireBytesPerPacket * 8.0;
}

// Share of the 100BASE-T link EP6 may fill. 70% is a headroom choice, not a
// measurement: the rest covers EP2, ARP/discovery, other switch traffic and
// burstiness. Wire rates (Mbit/s):
//            1 RX    2 RX    3 RX    4 RX
//    48 k     3.3     5.9     8.4    11.1
//    96 k     6.7    11.7    16.9    22.2
//   192 k    13.4    23.4    33.7    44.4
//   384 k    26.8    46.8    67.5    88.8  <- 4 RX at 384 k is refused
inline constexpr double kEp6LinkBitsPerSecond = 100.0e6;
inline constexpr double kEp6LinkBudgetFraction = 0.70;

// The most receivers that (rate, receiver-count) budget admits at `sampleRateHz`,
// at least 1. A caller that wants more must slow down first.
constexpr int maxReceiversAtRate(int sampleRateHz, int hardMax = kMaxReceivers) noexcept
{
    constexpr double budget = kEp6LinkBitsPerSecond * kEp6LinkBudgetFraction;
    int best = 1;
    for (int n = 1; n <= hardMax; ++n) {
        if (ep6BitsPerSecond(sampleRateHz, n) <= budget)
            best = n;
    }
    return best;
}

// The single-receiver EP6 block, 126 samples. Named rather than spelled 126 so
// the places that are genuinely single-receiver (the bring-up DSP tests) say so,
// instead of sharing a constant with code that must scale.
inline constexpr int kEp6BlockSamples = ep6SamplesPerPacket(1);

// EP2 (host->radio) is a different and receiver-count-INDEPENDENT layout: a
// fixed 8 bytes of [audio/EADDR(4) | I(2) | Q(2)] per transmit sample, always
// 63 per frame. Kept separate from the EP6 geometry above precisely so that
// adding receivers cannot silently reshape the transmit packet.
inline constexpr std::size_t kTxSampleBytes = 8;

// C0 register-address bytes (address << 1). dsopenhpsdr1.v (CMDCTRL) splits
// host->radio C0 three ways: [7] resprqst, [6:1] six-bit address (0x00..0x3F),
// [0] MOX. So every constant here is even; keying is applied with withMox()
// and RQST with withRespRqst().
inline constexpr std::uint8_t kC0Config = 0x00;   // addr 0x00: sample rate + #RX + ADC select
inline constexpr std::uint8_t kC0Rx1Freq = 0x04;  // addr 0x02: RX1 NCO frequency (Hz, 32-bit BE)
inline constexpr std::uint8_t kC0TxFreq  = 0x02;  // addr 0x01: TX1 NCO frequency (Hz, 32-bit BE)
inline constexpr std::uint8_t kC0TxDrive = 0x12;  // addr 0x09: TX drive level + PA/ATU/Alex bits

// MOX lives in C0 bit 0 of EVERY C&C frame, not in a register of its own: the
// radio reads it from whatever bank happens to be in flight. So keying is a
// property of the frame, and every bank has to carry it while transmitting.
inline constexpr std::uint8_t kC0MoxBit = 0x01;

// C0 bit 7, host->radio: RESPONSE REQUEST. The radio answers that command with
// an ACK frame on EP6 (C0[7] set). NOT a read bit: the write is applied
// regardless and the reply echoes cmd_data (control.v RESP_START). Only the
// AD9866 SPI and I2C commands return a read value (RESP_READ). Only
// Hl2ControlRequest sets it.
inline constexpr std::uint8_t kC0RespRqstBit = 0x80;

// Highest register address the six-bit C0 field can carry.
inline constexpr int kMaxRegisterAddress = 0x3F;

// Radio->host ACK address meaning "I could not do that". The response FSM
// substitutes 6'h3f for the command address when a subsystem was not ready
// (control.v, RESP_ACK), so this is a refusal and not a register. We never
// REQUEST 0x3F — it is the extended-address escape (see ep2WriteTxIq) — so the
// two readings never collide on our wire.
inline constexpr int kRespAddrError = 0x3F;

// TX drive level occupies DATA[31:24] (C1). The Hermes-Lite 2 gateware decodes
// only the top nibble [31:28], but the byte-wide field is what the reference
// clients and hpsdrsim both read, so the value is carried as 0..255 and the
// hardware takes the coarse part of it.
inline constexpr int kTxDriveMax = 255;
inline constexpr std::uint8_t kC0AdcGain = 0x14;  // addr 0x0a: AD9866 LNA gain
// addr 0x0e means two things:
//   generic openHPSDR: per-receiver ADC assignment. C1 = RX1..RX4 (2 bits each,
//                      LSB first), C2 = RX5..RX7, C3[4:0] TX att.
//   Hermes-Lite 2:     TX LNA gain. [15] enable HW-managed TX gain, [14] its
//                      LNA mode, [13:8] the gain.
// It must be sent: a conforming multi-ADC device (hpsdrsim rx_adc[] = -1)
// streams all-zero IQ until assigned. All-zero is inert on the HL2. The T/R
// gain switch and PureSignal would need a real value here, which the round
// robin would zero every third frame.
inline constexpr std::uint8_t kC0AdcAssignOrTxGain = 0x1C;

// addr 0x39: sync / reset. DATA[7:4] = 0x8 resets every decimation filter
// pipeline; 0x9 also phase-aligns the NCOs. NOTHING SENDS THIS: ~30/s during a
// pan drag halted the stream and discovery until power-cycle. Zeroing the
// other fields is not known to be "no action"; the register also holds the
// watchdog enable [27:24] and master enable [11:8]. See
// MetisClient::requestPipelineReset() before using it.
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

// C3 bits DATA[11] and DATA[12]: LT2208 dither and random on openHPSDR. On
// the HL2 the dither bit is NOT inert: it drives the CL2 band-voltage output
// (control.v `band_volts_enabled <= cmd_data[11]`), or on an AK4951 board the
// speaker (i2c_bus2.v `ak4951_spon_next = cmd_data[11]`). It is not a codec
// interlock; Hl2HardwareOptions::Codec decides what it carries. The random bit
// has no known HL2 use.
inline constexpr std::uint8_t kConfigDither = 0x08;   // C3 bit3 = DATA[11]
inline constexpr std::uint8_t kConfigRandom = 0x10;   // C3 bit4 = DATA[12]

enum class SampleRate : std::uint8_t { R48k = 0, R96k = 1, R192k = 2, R384k = 3 };
int sampleRateHz(SampleRate rate) noexcept;

// Companion filter board (J16 open-collector outputs). The gateware forwards
// config bits 0x00[23:17] (plus RX-antenna 0x00[13] as bit 7) as one byte to
// I2C address 0x20 (HL2 wiki Protocol.md "Filter Board"). The N2ADR board
// decodes bits 6:0 one-hot; values are Quisk's Hermes_BandDict verbatim
// (quisk_conf_defaults.py). With no board fitted the write is inert, so it is
// driven unconditionally.
inline constexpr std::uint8_t kOcLpf160   = 0x01;   // 160 m low-pass
inline constexpr std::uint8_t kOcLpf80    = 0x02;   // 80 m
inline constexpr std::uint8_t kOcLpf60_40 = 0x04;   // 60 m + 40 m share one
inline constexpr std::uint8_t kOcLpf30_20 = 0x08;   // 30 m + 20 m
inline constexpr std::uint8_t kOcLpf17_15 = 0x10;   // 17 m + 15 m
inline constexpr std::uint8_t kOcLpf12_10 = 0x20;   // 12 m + 10 m
inline constexpr std::uint8_t kOcHpfAmBc  = 0x40;   // AM broadcast blocking HPF
inline constexpr std::uint8_t kOcNone     = 0x00;   // every relay released

// The open-collector byte for a frequency in Hz, defined across the whole
// tuning range (not just amateur bands); every amateur band matches Quisk's
// table. No HPF below 1.6 MHz (it would remove MW) or on 160 m (supply spurs
// couple into those inductors, HL2 wiki Options.md); nothing above 30 MHz.
std::uint8_t ocFilterByteForHz(double hz) noexcept;

// Human-readable name of an open-collector filter selection, for logging.
const char* ocFilterName(std::uint8_t oc) noexcept;

// A 5-byte Command & Control payload: C0 (register address) + C1..C4 (data).
using Cc = std::array<std::uint8_t, 5>;

// Config register: sample rate + receiver count + the J16 open-collector filter
// byte. Also carries the Mercury and duplex bits for openHPSDR compatibility;
// both are ignored by the HL2 gateware.
//
// ocFilterByte is the value from ocFilterByteForHz(); only bits [6:0] are used
// (they land in DATA[23:17]). Bit 7 is the RX-antenna bit and lives elsewhere in
// the register, so it is masked off here rather than silently switching antennas
// on a caller who passed a full I2C byte.
Cc ccConfig(SampleRate rate, int numRx = 1, std::uint8_t ocFilterByte = kOcNone,
            bool dither = false, bool random = false) noexcept;
// NCO frequency in Hz (32-bit big-endian across C1..C4) for receiver `rxIndex`,
// zero-based: RX1 is index 0 at register 0x02, up to RX7 at 0x08. Clamped to
// that run — see the note in the .cpp about why RX8..RX12 are not reachable by
// continuing the arithmetic.
Cc ccRxFreq(int rxIndex, std::uint32_t hz) noexcept;
// RX1 NCO frequency in Hz. Equivalent to ccRxFreq(0, hz).
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
// TX drive level (0..kTxDriveMax, in C1) plus 0x09 flag bits, both default OFF
// so a caller always asks for them explicitly:
//   paEnable = 0x09[19] (C2 bit 3). Without it output is the AD9866 DAC alone,
//              milliwatts (forward-power counts read zero).
//   atuTune  = 0x09[20] (C2 bit 4), a request: the AH-4 handler tunes while it
//              is held and keyed. Leave clear for an I2C-driven ATU on the
//              N2ADR IO board (Hl2HardwareOptions::atuGateware).
Cc ccTxDrive(int level, bool paEnable = false, bool atuTune = false) noexcept;

// Direct I2C writes: 0x3c (I2C1, internal: Versa clock, AD9866) and 0x3d
// (I2C2, external companion bus). Payload per gateware/rtl/i2c.v:
//   C1 = DATA[31:24] cookie 0x06 write / 0x07 read
//   C2 = DATA[23] stop at end, DATA[22:16] 7-bit chip address
//   C3 = DATA[15:8] register;  C4 = DATA[7:0] data byte
// One two-byte I2C write per bank. Neither address is on
// MetisClient::requestRegister's allow-list, so RQST stays clear.
// addr 0x3b is a raw AD9866 SPI write (ad9866ctrl.v, cookie 8'h06) that can
// hit any converter register, so it has no encoder and is not allow-listed.
inline constexpr std::uint8_t kC0Ad9866Spi = 0x76;  // addr 0x3b << 1
inline constexpr std::uint8_t kC0I2c1 = 0x78;          // addr 0x3c << 1
inline constexpr std::uint8_t kC0I2c2 = 0x7A;          // addr 0x3d << 1
inline constexpr std::uint8_t kI2cCookieWrite = 0x06;  // C1
inline constexpr std::uint8_t kI2cStopAtEnd   = 0x80;  // C2 bit 7
// AND 0x07 IS THE READ COOKIE, on both buses. Named here without an encoder,
// because the distinction between "the protocol cannot" and "this client does
// not" is worth keeping straight. i2c_bus2.v accepts either address on
// `cmd_data[31:25] == 7'h03` — i.e. C1 of 0x06 or 0x07 — and then branches on
// the low bit:
//
//     state_next = cmd_data[24] ? STATE_READ_CMDADDR : STATE_CMDADDR;
//
// A read walks STATE_READ_DATA0..4, assembles four bytes into `resp_data`, and
// control.v's RESP_READ returns them as `cmd_resp_data_i2c`. So a VersaClock
// register CAN be read back over Protocol 1; nothing here implements it.
//
// WHAT A READBACK WOULD AND WOULD NOT BE WORTH. It reads the register we wrote,
// so it proves the write arrived — which is more than we have today. It does NOT
// prove the PLL has locked to anything: no status bit is routed out of the part
// to the command plane, so "locked to CL1" and "programmed for CL1 with no cable
// attached" still look identical from here. Do not build a lock indicator on it.
inline constexpr std::uint8_t kI2cCookieRead  = 0x07;  // C1; no encoder, see above

// HL2 IO Board (N2ADR), I2C2 chip 0x1D: switches amplifiers, relays and
// transverters from the TX frequency, which only the host supplies. Registers
// 0..4 hold it in Hz, MSB first; writing register 4 (LSB) commits all five
// from the board's register file (HL2IOBoard/n2adr_lib/i2c_slave_handler.c).
// So send the LSB LAST and send all five, or a stale byte is committed.
inline constexpr std::uint8_t kIoBoardI2cAddr      = 0x1D;
inline constexpr std::uint8_t kIoBoardRegTxFreqMsb = 0;   // DATA bits 39:32
inline constexpr std::uint8_t kIoBoardRegTxFreqLsb = 4;   // DATA bits  7:0, COMMITS

// One single-byte I2C write on the external companion bus (I2C2, addr 0x3d).
// `chip` is the device's 7-bit address; the stop bit is always set, as the
// wiki advises for forward compatibility.
Cc ccI2c2Write(std::uint8_t chip, std::uint8_t reg, std::uint8_t data) noexcept;

// The same, on the INTERNAL bus (I2C1, addr 0x3c). Different bus, different
// hazard: I2C2 reaches a companion board that can be absent, while I2C1
// reaches the board's own VersaClock — the part that clocks the AD9866. A
// wrong write here does not fail to switch a relay, it stops the radio
// sampling. There is exactly one caller, versaClockCl1Banks() below.
Cc ccI2c1Write(std::uint8_t chip, std::uint8_t reg, std::uint8_t data) noexcept;

// ---- CL1 external 10 MHz reference (VersaClock 5P49V5923, I2C1 chip 0x6A) ----
//
// WHAT THIS IS. The HL2 runs from a 38.4 MHz crystal multiplied to 76.8 MHz by
// an on-board VersaClock. Feeding a GPSDO into the CL1 jack does not switch
// anything by itself: the VersaClock has to be REPROGRAMMED to take its
// reference from that input instead of the crystal, and going back means
// reprogramming it again. There is no single "external reference" bit.
//
// THE TABLES ARE NOT DERIVED AND CANNOT BE. They are the register/value pairs
// piHPSDR and deskHPSDR both carry verbatim, which in turn came from the
// Hermes-Lite 2 project; the 5P49V5923's PLL dividers, input mux and
// feedback configuration are a solved layout for this one board and nothing
// in the datasheet would let a reader re-derive these twenty-four pairs
// without the board's schematic and its loop filter. They are reproduced
// rather than re-computed for exactly that reason, and they are the reason
// this is a fixed table rather than a function of anything.
//
// SENT ON CHANGE AND ON CONNECT, never re-asserted. The radio boots on its
// crystal every time, so connecting is a change; and twenty-four banks in the
// one-shot queue at every rotation would starve the NCO refresh.
//
// ORDER MATTERS. The pairs configure the input mux before the PLL that locks
// to it; sending them out of order can leave the part running from a reference
// that is not there yet. They go out in the order given, one bank per EP2
// frame, which is ~63 ms for the whole sequence at 48 kHz.
inline constexpr std::uint8_t kVersaClockI2cAddr = 0x6A;
inline constexpr std::size_t kVersaClockCl1Banks = 24;
std::array<Cc, kVersaClockCl1Banks> versaClockCl1Banks(bool externalRef) noexcept;

// The five C&C banks that write `hz` into the IO board's TX-frequency
// registers, already in send order (MSB first, committing LSB last).
inline constexpr std::size_t kIoBoardTxFreqBanks = 5;
// ccIoBoardTxFrequency shifts by 8 * (kIoBoardRegTxFreqLsb - reg); a bank
// count that overran Msb..Lsb would underflow that into an undefined shift.
static_assert(kIoBoardTxFreqBanks
                  == static_cast<std::size_t>(kIoBoardRegTxFreqLsb
                                              - kIoBoardRegTxFreqMsb + 1),
              "IO board frequency bank count must span Msb..Lsb exactly");
std::array<Cc, kIoBoardTxFreqBanks> ccIoBoardTxFrequency(std::uint64_t hz) noexcept;
// A C&C bank addressing an arbitrary six-bit register, for Hl2ControlRequest's
// runtime-named registers. Prefer the named encoders. The address is masked to
// 0x3F so it cannot alias into RQST or MOX.
Cc ccRegister(int addr, std::uint32_t data) noexcept;

// Set or clear the response-request bit (C0 bit 7) on a C&C bank.
//
// Orthogonal to withMox(), which touches bit 0 only, so the two compose in
// either order and neither can set the other's bit.
inline Cc withRespRqst(Cc cc, bool request) noexcept
{
    cc[0] = static_cast<std::uint8_t>(request ? (cc[0] | kC0RespRqstBit)
                                              : (cc[0] & ~kC0RespRqstBit));
    return cc;
}

// Set MOX (C0 bit 0) on a C&C bank. Keying is per-FRAME, so this is applied to
// whichever bank is being sent rather than to one dedicated register.
inline Cc withMox(Cc cc, bool keyed) noexcept
{
    cc[0] = static_cast<std::uint8_t>(keyed ? (cc[0] | kC0MoxBit)
                                            : (cc[0] & ~kC0MoxBit));
    return cc;
}

// Write 16-bit I/Q transmit samples into an EP2 packet built by ep2Packet().
// Each 8-byte sample is [audio(4) | I(2) | Q(2)] big-endian, 63 per frame.
// The audio slot stays zero: on a bare HL2 the first one per frame is EADDR
// (extended address, base 0x3f), and zero means "unused". Excess samples are
// ignored; a short span leaves silence.
void ep2WriteTxIq(std::array<std::uint8_t, kUsbPacketSize>& pkt,
                  std::span<const std::complex<float>> iq) noexcept;

// Write 16-bit stereo speaker audio into the slot ep2WriteTxIq() leaves zero.
// PRECONDITION: the radio has a codec (HL2+ AK4951 board or SquareSDR 2); on a
// bare HL2 the first slot per frame is EADDR and a sample there is a command.
// MetisClient gates this on Hl2HardwareOptions::hasLocalCodec(). `audio` is
// interleaved L,R big-endian, 126 samples at 48 kHz; a short span leaves the
// zero fill (silence, and a zero EADDR).
void ep2WriteTxAudio(std::array<std::uint8_t, kUsbPacketSize>& pkt,
                     std::span<const std::int16_t> audio) noexcept;

// Transmit samples carried per EP2 packet (63 per frame, two frames).
inline constexpr int kTxSamplesPerPacket = 126;

// EP6 C&C responses. C0[7] is ACK and changes how C0 is read:
//   ACK == 0 (free-running): C0[6:3] = RADDR[3:0], C0[2] Dot, C0[1] Dash
//            (always 0 on HL2), C0[0] PTT.
//   ACK == 1 (reply to RQST): C0[6:1] = RADDR[5:0], C0[0] PTT.
// The HL2's free-running resp_addr is 2 bits (control.v), so RADDR cycles
// 0..3 (3 = debug); hpsdrsim cycles 0..4. One response slot per 512-byte frame
// (usopenhpsdr1.v SYNC_RESP), and none once the stream stops.
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

    // TX IQ FIFO status, DATA[15:8] of RADDR 0 (`dsiq_status`, gateware
    // 883a338; layout in apply()). Top 7 bits of the read-side fill level,
    // 0..127 (fifos.v:101): coarse occupancy, NOT a sample count (#17).
    std::optional<int>  txFifoFillMsbs;
    // One flag for two faults, FIFO empty OR writes blocked when full
    // (fifos.v:105-106); report it as a "TX pacing fault", not either one.
    std::optional<bool> txFifoRecovery;
    std::optional<int>  temperatureRaw;
    std::optional<int>  forwardPowerRaw;
    std::optional<int>  reversePowerRaw;
    std::optional<int>  biasCurrentRaw;
    bool ptt = false;

    // ADC overload as a rate. `adcOverload` is the last value, a ~10 Hz sample
    // (kTelemetryMinIntervalMs) of a bit that changes up to ~190/s. MetisClient's
    // receive loop accumulates these per publish interval: RADDR-0 responses
    // seen, and how many carried the bit. The denominator varies with rate, RX
    // count and command-response displacement, so it must be carried; nothing
    // arrives while not streaming, and absence is not "clean". DATA[24] is
    // &clip_cnt, a 2-bit saturating counter: set means >= 3 clip events per
    // interval, clear does NOT mean no clipping (#5354; see Hl2AutoGainPolicy.h).
    int adcSamples = 0;
    int adcOverloadSamples = 0;
    int adcWindowMs = 0;

    // Maximum of the publish window's non-ACK RADDR-1 DATA[15:0] (the radio
    // re-samples forward power every other EP6 response, control.v:261, so the
    // last value alone misses speech peaks); nullopt when the window saw none.
    // `forwardPowerSamples` counts them; the window length is `adcWindowMs`.
    std::optional<int> forwardPowerPeakRaw;
    int forwardPowerSamples = 0;

    // Merge a decoded response in, leaving untouched fields alone. ACK
    // responses contribute only PTT: their raddr is the command address and
    // data our own echo, which would otherwise decode as telemetry.
    void apply(const Ep6Response& r) noexcept;
};

// Accumulator for Hl2Telemetry::forwardPowerPeakRaw: the maximum of DATA[15:0]
// over non-ACK RADDR-1 responses that carry a measurement. Kept here so the
// rule is testable without a socket.
struct ForwardPowerWindow {
    std::optional<int> peak;
    int samples = 0;

    void observe(const Ep6Response& r) noexcept
    {
        if (r.ack || r.raddr != 0x01)
            return;
        // A temperature word of 0 (DATA[31:16]; -50 C by hl2TemperatureCelsius)
        // marks a response that is not a measurement: gateware 74.2 sends a few
        // just after the T/R switch-over, and their forward word can be far
        // above the carrier. The last-value fields in apply() still take it.
        if ((r.data >> 16) == 0)
            return;
        const int v = static_cast<int>(r.data & 0xFFFF);
        if (!peak || v > *peak)
            peak = v;
        ++samples;
    }
    void clear() noexcept
    {
        peak.reset();
        samples = 0;
    }
};

// Directional-coupler counts -> watts via Quisk's reference curve (see the
// .cpp). Indicative only: not a calibration of any particular radio.
double directionalWatts(int raw) noexcept;

// Detector output in arbitrary VOLTAGE units: sqrt(directionalWatts(raw)).
// Only ratios of two of these (SWR) are meaningful.
double detectorVolts(int raw) noexcept;

// Minimum forward counts below which SWR is noise; shared by the meter and the
// Radio Health snapshot. Criterion: noise moves reported SWR by <= 0.25 at p95
// for true SWR 1.0..3.0. Bench run D89 (one HL2, dummy load): reverse sd 2.73
// counts, forward residual sd 3.77, offset rev = 3.41 + 0.00097 * fwd.
//     forward counts        16     32     64     96    160    256    320
//     p95 error (measured) 6.35   1.95   0.91   0.65   0.38   0.26   0.20
// 320 is the smallest gridded count meeting it (#4578). SWR is absent below
// ~74 mW forward. Subtracting unkeyed floors before TX could lower it to ~200.
inline constexpr int kMinForwardCountsForSwr = 320;

// Reverse-channel counts with no reflected power: the intercept of
// rev = 3.41 + 0.00097*fwd from bench run D89 (one HL2, gateware v74, N2ADR,
// dummy load, 7.1 MHz). For hl2_metis_protocol_test to check the SWR gate
// against; never use it to correct a reading.
inline constexpr double kMeasuredReverseFloorCounts = 3.41;

// The clip rate for a window as a whole percent, or nullopt when the window
// has fewer than `minSamples` observations. nullopt renders "not reported" and
// keeps a control loop from releasing gain into a stalled stream.
[[nodiscard]] constexpr std::optional<int> adcClipRatePercent(
    int samples, int overloadSamples, int minSamples = 4) noexcept
{
    if (samples <= 0 || samples < minSamples) {
        return std::nullopt;
    }
    const int over = overloadSamples < 0 ? 0
                   : (overloadSamples > samples ? samples : overloadSamples);
    // Rounded to nearest, in integer arithmetic: a rate quoted to two
    // significant figures would be precision this observation does not have.
    return (over * 200 + samples) / (samples * 2);
}

// Standing-wave ratio from raw forward/reverse counts, each mapped through
// detectorVolts() first because the detector curve is not linear. Returns
// nullopt with no forward power (1.0 would read as a perfect match).
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

// Bit 1 of the run/stop byte is `wide_spectrum` (dsopenhpsdr1.v RUNSTOP:
// `run <= eth_data[0]; wide_spectrum <= eth_data[1]`), enabling the EP4
// wideband bandscope. Sent only via metisRunCommand(), never metisStart().
// metisStop() (0x00) clears both bits, stopping the bandscope too.
inline constexpr std::uint8_t kRunWideSpectrum = 0x02;

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

// The run byte re-sent mid-stream (twice a second) to set `wide_spectrum`
// with `run` held at 1; re-asserting run is a gateware no-op, and clearing it
// would stop IQ. connect's metisStart() byte stays `0x01`, which
// hl2_metis_protocol_test and three fake-radio fixtures rely on.
inline std::array<std::uint8_t, 64> metisRunCommand(bool wideSpectrum,
                                                    bool watchdogEnabled = true) noexcept
{
    return metisCommand(static_cast<std::uint8_t>(
        0x01 | (wideSpectrum ? kRunWideSpectrum : 0x00)
             | (watchdogEnabled ? 0x00 : kRunWatchdogDisable)));
}

// 63-byte discovery request: EF FE 02 + 60 zero bytes (broadcast to :1024).
std::array<std::uint8_t, 63> discoveryRequest() noexcept;

struct DiscoveryReply {
    std::array<std::uint8_t, 6> mac{};
    std::uint8_t gatewareVersion = 0;   // raw byte; HL2 gateware e.g. 0x4A -> 7.4
    std::uint8_t boardId = 0;           // 0x06 = Hermes-Lite / Hermes-Lite 2
    bool streaming = false;             // discovery status byte 0x03 = already sending IQ
    // Receiver count the board reports, from discovery offset 0x13. Only present
    // on full-length replies (> 19 bytes); 0 means "not reported" and callers
    // apply their own default. See parseDiscoveryReply for why this offset is
    // 19 and not 20, and what the byte at 20 actually is.
    std::uint8_t numRx = 0;
    [[nodiscard]] bool isHermesLite2() const noexcept { return boardId == 0x06; }

    // Telemetry, offsets 0x17-0x29 (gateware 883a338, offset = 0x3B - dbyte_no):
    // the EP6 response quantities in the same raw units, readable without a
    // stream. nullopt = this reply did not carry it (short reply); 0 is a
    // reading, though gateware without EXTENDED_RESP sends hard zeros.
    std::optional<std::uint32_t> responseData;   // 0x17-0x1a, `resp_data`
    std::optional<bool> extCwKey;                // 0x1b[7]
    std::optional<bool> ptt;                     // 0x1b[6]  `ptt_resp` = cw_on|ext_ptt
    std::optional<bool> paExtTr;                 // 0x1b[5]
    std::optional<bool> paIntTr;                 // 0x1b[4]
    std::optional<bool> txOn;                    // 0x1b[3]
    std::optional<bool> cwOn;                    // 0x1b[2]
    // 0x1b[1:0]. NOT a count of clips — see the two-state warning in the .cpp.
    std::optional<int>  adcClipCount;
    std::optional<int>  temperatureRaw;          // 0x1c-0x1d, 12 bits
    std::optional<int>  forwardPowerRaw;         // 0x1e-0x1f, 12 bits
    std::optional<int>  reversePowerRaw;         // 0x20-0x21, 12 bits
    std::optional<int>  biasCurrentRaw;          // 0x22-0x23, 12 bits
    std::optional<int>  txFifoFillMsbs;          // 0x24[6:0], as in Hl2Telemetry
    std::optional<bool> txFifoRecovery;          // 0x24[7],   as in Hl2Telemetry
    std::optional<int>  txBufferLatencyMs;       // 0x26[6:0]
    // 0x28[4:0]. SAFETY-RELEVANT: 31 does not mean "the longest hang"; it
    // disables the gateware's PTT auto-unkey entirely (softerhardware/
    // Hermes-Lite2 issue #178). Reading it without a stream is how an
    // application can tell the operator their radio's own dead-man's switch is
    // off — which is a thing worth knowing before keying, not after.
    std::optional<int>  pttHangTimeMs;
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

// Multi-receiver form: demultiplex an EP6 packet into one output vector per
// receiver. `out.size()` IS the receiver count the packet is decoded against —
// it must match what the radio was configured with at 0x00[6:3], because the
// wire carries no receiver-count field and a mismatch silently reinterprets
// every subsequent round. Samples are APPENDED to each vector. Returns samples
// appended PER RECEIVER, or -1 for a bad packet or `out` empty / larger than
// kMaxReceivers.
int ep6SamplesMulti(std::span<const std::uint8_t> pkt,
                    std::span<std::vector<std::complex<float>>> out) noexcept;

// Temperature in degrees C from `temperatureRaw`, shared by the EP6 path
// (Hl2Backend::temperatureCelsius) and the discovery path (Hl2TelemetryService)
// so they agree. HL2 wiki formula, not checked against a thermometer.
[[nodiscard]] constexpr double hl2TemperatureCelsius(int raw) noexcept
{
    return (3.26 * (static_cast<double>(raw) / 4096.0) - 0.5) / 0.01;
}

// EP4 wideband bandscope: a second radio->host stream on the EP6 socket,
// enabled by kRunWideSpectrum, framed `EF FE 01 04`, 1032 bytes (usopenhpsdr1.v
// WIDE1..WIDE4, fifos.v usbs_fifo; confirmed on a v74.2 board). It carries raw
// pre-DDC AD9866 codes: 2048 consecutive 12-bit samples at 76.8 MSPS per
// 4-packet block. Same `rx_data` the gateware clip detector reads (ad9866.v
// `rxclipp = (rx_data == 12'b011111111111)`), so levels share the overload scale.
inline constexpr std::size_t kEp4SamplesPerPacket = 512;   // 1024 bytes / 2
// Four packets drain the 2048-word usbs_fifo, and they are NOT four packets
// apart in wall time — measured at ~2.6 ms between the packets of a block and
// ~10.5 ms between blocks. The 2048 samples are contiguous in CONVERTER time
// (26.67 us) whatever the wall-clock spread; see kEp4BlockSamples.
inline constexpr int         kEp4PacketsPerBlock  = 4;
inline constexpr int         kEp4BlockSamples     = 2048;
// 12-bit two's complement, so the code range is [-2048, +2047] and the POSITIVE
// extreme is 2047, not 2048. Normalising by 2048 keeps 0 dBFS at the negative
// rail and at the gateware's own rxclip threshold; see Ep4Stats::clippedSamples
// for why the clip predicate is not a symmetric `abs(code) >= 2048`.
//
// NOT kFullScale: that is the EP6 24-bit DDC scale and applying it here would
// read every bandscope block as ~66 dB quieter than it is.
inline constexpr int         kEp4FullScale        = 2048;
// hermeslite_core.v `parameter CLK_FREQ = 76800000`; clk_ad9866 is derived from
// rffe_ad9866_clk76p8. First Nyquist zone DC..38.4 MHz. Also the full-rate
// reference for Hl2BandscopeHeadroom.h's gatedPeakBiasDbForPeriod().
inline constexpr double      kAdcSampleRateHz     = 76.8e6;
// `ep4_seq_no` is declared `logic [19:0]`: byte 4 of the header is a hardwired
// 8'h00 and byte 5 masks to a nibble. It wraps at 1,048,576 — about 46 minutes
// at the measured 381 packets/s — and a detector that assumes EP6's 32 bits
// reports one enormous gap per wrap.
inline constexpr std::uint32_t kEp4SeqModulus = 1u << 20;
// The forward-gap guard at 20-bit scale: a backward jump is a reset, not loss.
// Every stream start rewinds: usopenhpsdr1.v
//     ep4_seq_no <= (bs_tvalid) ? ep4_seq_no_next : {ep4_seq_no_next[19:2],2'b00};
// zeroes the low bits until the FIFO fills, so a fresh stream emits 0, 1, 2, 0.
inline constexpr std::uint32_t kEp4SeqForwardGapMax = kEp4SeqModulus / 2;

// One step of the EP4 sequence counter, classified. Pure so it can be tested
// against recorded sequences without a socket.
struct Ep4SeqStep {
    std::uint32_t drops = 0;   // packets genuinely lost before this one
    bool rewind = false;       // the counter went backwards: a reset, not loss
};

constexpr Ep4SeqStep ep4SeqStep(std::uint32_t expected, std::uint32_t got) noexcept
{
    Ep4SeqStep step;
    // Modular difference, so the 20-bit wrap at kEp4SeqModulus is an ordinary
    // step of one rather than a gap of a million.
    const std::uint32_t gap = (got - expected) & (kEp4SeqModulus - 1);
    if (gap == 0)
        return step;                                   // in sequence
    if (gap < kEp4SeqForwardGapMax)
        step.drops = gap;                              // forward gap = real loss
    else
        step.rewind = true;                            // backward jump = reset
    return step;
}
// Duty-cycle gate timing constants, MEASURED on a v74.2 board (bench runs
// d94 `d94-ep4-bandscope-existence`, d95 `d95-procedure-b-ep6-cost`), not
// derived from RTL.

// EP4 packet rate: ~380.95/s at 1 RX, flat to 3 ppm over 48..384 kHz (clocked
// by `bs_cnt` off the 76.8 MHz converter clock); 320.0/s at 3 RX, because
// usopenhpsdr1.v's START arbitration serves EP6 before the bandscope. Other
// receiver counts are unmeasured.
inline constexpr double kEp4PacketsPerSecond1Rx = 380.95238095238096;
inline constexpr double kEp4PacketsPerSecond3Rx = 320.0;

// 10.5 ms at one receiver — the interval between BLOCKS, not between the
// packets of one. A block's four packets arrive ~2.62 ms apart, spanning
// 7.87 ms; the next block begins 10.5 ms after the previous one did.
inline constexpr double kEp4BlockIntervalMs =
    1000.0 * kEp4PacketsPerBlock / kEp4PacketsPerSecond1Rx;
// 12.5 ms, the SLOWEST cadence the radio has been seen to produce. The guard
// below uses this one and not the configuration's own, because a deadline has
// to assume the slowest behaviour that has actually been observed, and the
// rate at two receivers — and at four, five and six — is not known.
inline constexpr double kEp4BlockIntervalSlowestMs =
    1000.0 * kEp4PacketsPerBlock / kEp4PacketsPerSecond3Rx;

// The EP6 packet interval, which is what the arming delay is really measured in.
constexpr double ep6PacketIntervalMs(int sampleRateHz, int numRx) noexcept
{
    const double pps = ep6PacketsPerSecond(sampleRateHz, numRx);
    return pps > 0.0 ? 1000.0 / pps : 0.0;
}

// EP6 packets between a `run` transition (0x00 -> 0x03) and the first EP4
// datagram: measured 129 at both 48 kHz (0.3297 s) and 384 kHz (0.0418 s), so
// it is stream-clocked, not a timer. 160 = 129 + 24 % margin.
inline constexpr int kEp4ArmingEp6Packets = 160;

// Constexpr ceiling, because std::ceil is not constexpr before C++23 and this
// figure has to be available to a static_assert.
constexpr int ep4CeilToInt(double v) noexcept
{
    const int t = static_cast<int>(v);
    return (v > static_cast<double>(t)) ? t + 1 : t;
}

// How long the gate waits for a complete bandscope block. Both terms of the
// max are needed:
//   * 10 x slowest block interval (125 ms): mid-stream enable, first EP4 in
//     ~2.5 ms, then a flushed and a captured block (5x margin).
//   * 160 EP6 packet intervals: a run transition through 0x00 (e.g.
//     setReceiverCount's stop/start), first EP4 after 129 EP6 packets.
// Unmeasured above 1 RX: whether arming counts EP6 packets or samples (if
// samples, 3 RX / 48 kHz misses once), and the EP4 rate at 2 or 4+ RX. A miss
// costs one bandscopeTimeouts increment and a retry, never a wedged gate.
constexpr int bandscopeGuardMs(int sampleRateHz, int numRx) noexcept
{
    const double byBlock  = 10.0 * kEp4BlockIntervalSlowestMs;
    const double byPacket = kEp4ArmingEp6Packets * ep6PacketIntervalMs(sampleRateHz, numRx);
    return ep4CeilToInt(byBlock > byPacket ? byBlock : byPacket);
}

// The two cases that made the max necessary, pinned where they cannot drift
// from the constants above.
static_assert(bandscopeGuardMs(48000, 1) == 420,
              "48 kHz / 1 RX must clear the measured 129-packet (0.339 s) arming delay");
static_assert(bandscopeGuardMs(384000, 1) == 125,
              "384 kHz / 1 RX falls back to the block-interval term");

// dBFS of half a code — 20*log10(1 / (2*kEp4FullScale)). A block of all-zero
// codes has no representable level at all; the true answer is -inf, which no
// readout can render and no arithmetic downstream survives. This floor says
// "below the smallest code this converter has" without inventing a level.
inline constexpr double kEp4FloorDbfs = -72.25;

// Accumulated magnitude statistics over one EP4 packet, or over a whole block
// merged from four of them. Deliberately NOT a spectrum: nothing here plans,
// allocates or transforms, and this type must never grow an FFT — see
// WdspChannel::fftwSetupLock() for why a second FFTW user in this process is a
// hazard rather than a convenience.
struct Ep4Stats {
    int    samples        = 0;
    int    peakAbs        = 0;    // 0..kEp4FullScale
    double sumSquares     = 0.0;  // of raw codes, so rms shares peak's scale
    // Signed sum of the raw codes, so rmsDbfs() can remove the mean: a DC
    // offset would otherwise inflate rms and deflate the crest (broadband noise
    // ~11-12 dB vs ~3 dB for a carrier) (#5802). Exact in a double: 2048 codes
    // sum to <= ~4.2e6, squares to <= ~8.6e9.
    double sum            = 0.0;
    // Codes at either converter rail, counted with the gateware's OWN
    // predicate rather than a symmetric one: ad9866.v fires rxclipp at
    // 12'b011111111111 (+2047) and rxclipn at 12'b100000000000 (-2048). A
    // symmetric `abs(code) >= 2048` can never fire on a POSITIVE clip, because
    // +2048 is not a code a 12-bit two's-complement converter can produce.
    int    clippedSamples = 0;
    // Uncalibrated pre-DDC dBFS, comparable only with the gateware's clip and
    // good-level flags (not an S-meter, the WDSP ADC peak, or antenna level).
    // peakDbfs() is ABSOLUTE (largest |code|); rmsDbfs() is AC-coupled (about
    // the record's mean). The mixed reference is deliberate (#5802); the mean
    // the RMS removes is published by dcDbfs() and meanCodes() below (#5856).
    [[nodiscard]] double peakDbfs() const noexcept;
    [[nodiscard]] double rmsDbfs()  const noexcept;
    // Peak-to-RMS in dB, or nullopt unless BOTH are above kEp4FloorDbfs, a
    // sentinel rather than a level. A DC pedestal with sub-code AC deviation
    // would otherwise publish 60-90 dB of meaningless "crest" (#5802).
    [[nodiscard]] std::optional<double> crestDb() const noexcept;
    // The record's mean, the DC level rmsDbfs() removes, as
    // 20*log10(|mean| / kEp4FullScale) on the same pre-DDC scale (#5856). Named
    // for where it is measured, not for a cause. kEp4FloorDbfs is returned only
    // for no samples or a mean of exactly zero; a non-zero mean under half a
    // code computes BELOW the floor, unclamped, as rmsDbfs() does.
    [[nodiscard]] double dcDbfs() const noexcept;
    // The same mean, SIGNED, in raw converter codes. At two decimals a mean
    // of about half a code prints the same -72.25 as the zero-mean sentinel in
    // dcDbfs(); this prints 0.50 against 0.00. 0.0 for a record with no
    // samples, so callers check `samples` first.
    [[nodiscard]] double meanCodes() const noexcept;
    // Fold another packet's statistics in. Peak takes the max, everything else
    // sums, so a merged block's mean and variance are those of the 2048-sample
    // concatenation.
    void merge(const Ep4Stats& other) noexcept;
};

// The EP4 sequence number, masked to its real 20 bits, or nullopt if `pkt` is
// not an EP4 packet. The mask is not decoration: the gateware hardwires the
// high bits, and masking here means a corrupted or spoofed header cannot push
// the expected-sequence state outside the modulus the gap arithmetic assumes.
std::optional<std::uint32_t> ep4Seq(std::span<const std::uint8_t> pkt) noexcept;

// Decode an EP4 packet's 512 ADC codes, normalised to [-1, 1) by
// kEp4FullScale, and append them to `out`. Returns the count appended, or -1
// if `pkt` is not a valid EP4 packet. Each wire word is little-endian 16-bit
// holding the 12-bit code shifted left by four (usopenhpsdr1.v `WIDE3` emits
// `{bs_tdata[3:0], 4'b0000}`, `WIDE4` `bs_tdata[11:4]`); the decoder shifts
// right four and sign-extends from bit 11.
int ep4Samples(std::span<const std::uint8_t> pkt, std::vector<float>& out) noexcept;

// Magnitude statistics over one EP4 packet, or nullopt if `pkt` is not one.
// Allocation-free and transform-free: this is what runs on the I/O thread.
std::optional<Ep4Stats> ep4Stats(std::span<const std::uint8_t> pkt) noexcept;

}  // namespace AetherSDR::hl2
