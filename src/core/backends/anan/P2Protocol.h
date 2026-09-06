#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// openHPSDR Ethernet Protocol 2 wire primitives for the ANAN-G2 backend.
//
// Direct C++ port of the live-validated anan/spike/phase1a.py spike (aetherd
// ANAN P2 Phase 1a), run against a real ANAN-G2 (Saturn, protocol 4.3,
// firmware/gateware 27) on the bench. Protocol facts are grounded clean-room
// in the openHPSDR Ethernet Protocol v4.4 (Phil Harman VK6PH,
// TAPR/OpenHPSDR-Firmware; see THIRD_PARTY_LICENSES), the Saturn FPGA RTL,
// and p2app -- the radio-side Protocol 2 server and the counterparty on the
// wire, per the RFC's provenance model
// (docs/architecture/anan-p2-backend-design.md §2.9). Every non-obvious
// field cites the spec page it comes from, the way MetisProtocol.h does for
// Protocol 1.
//
// This layer is intentionally socket-free and Qt-free so it unit-tests
// against captured/synthetic frames without hardware; P2Client (a later
// commit) owns the UDP sockets and RX ingest thread and calls into these
// functions.
//
// TRANSMIT: there is no encoder for a DUC-Specific packet, TX drive, or PTT
// anywhere in this file. That is not a guard that can be bypassed -- the
// capability does not exist yet. TX is RFC §2.11 Phase 3, a separate future
// addition, gated behind the engine's TX arbiter above the seam
// (Constitution Principle VI: AetherSDR never transmits without operator
// intent).

namespace AetherSDR::anan {

inline constexpr std::uint16_t kRadioPort = 1024;

// The radio's own fixed listening ports for two command packet types, per
// the spec's default port table (p.19-20, reproduced in phase1a.py's own
// header comment). Unlike kDdc0DefaultPort below, THESE ARE load-bearing:
// General/DDC-Specific and High-Priority carry no shared "packet type" byte
// the way Discovery/General do (buildDdcSpecific()'s byte 4 is "number of
// ADCs", buildHighPriority()'s byte 4 is the run/PTT bitfield -- neither is
// a command discriminator), so the radio tells these two packet types apart
// by which port they arrive on. Sending either to kRadioPort instead is
// silently ignored by p2app -- proven in Phase 1a, where the spike sent
// DDC-Specific to 1025 and High-Priority to 1027 and that is the only
// configuration that ever produced a DDC0 IQ stream.
inline constexpr std::uint16_t kDdcSpecificPort = 1025;
inline constexpr std::uint16_t kHighPriorityPort = 1027;

// The port DDC0 IQ nominally originates FROM per the General Packet's byte
// 17-18 default (spec p.19-20). NOT load-bearing for where the PC receives
// it: Phase 1a measured DDC0 IQ arriving at the port the client's Discovery
// packet was SENT FROM, matching the spec's literal "Destination Port: the
// Source Port of the Host that initiated Discovery" rule for every radio->PC
// stream (p.43, p.51, p.54) -- not a fixed per-DDC port on the PC side.
// Kept here only as a documented fallback a client MAY also listen on; do
// not treat it as the primary demultiplexing port.
inline constexpr std::uint16_t kDdc0DefaultPort = 1035;

// Saturn boards' DSP clock. RFC §2.4; saturnregisters.c VSAMPLERATE.
inline constexpr std::uint32_t kDspClockHz = 122'880'000;

// 24-bit signed full scale. (1<<23)-1, not 1<<23 -- see MetisProtocol.h's
// kFullScale for the reasoning (the largest magnitude a 24-bit two's
// complement sample can actually take, matching the reference clients'
// dBFS scale rather than landing a fraction of a dB adrift).
inline constexpr int kFullScale24Bit = (1 << 23) - 1;

// Phase word: delta = 2^32 * F / Fdsp (spec p.21, p.44; RFC §2.4). This
// board REQUIRES phase words, not Hz -- Discovery Reply byte 21 == 1 on
// every G2 observed -- and General Packet byte 37 bit[3] must agree
// (buildGeneral() sets it). Verified in the field: phaseWord(10e6) tuned
// DDC0 to WWV and the carrier read back at 0 Hz baseband, 80.0 dB above the
// noise floor.
constexpr std::uint32_t phaseWord(double freqHz, std::uint32_t dspClockHz = kDspClockHz) noexcept
{
    constexpr double kTwoTo32 = 4294967296.0;
    const double delta = kTwoTo32 * freqHz / static_cast<double>(dspClockHz);
    // Round to nearest. freqHz is always non-negative for a receive tune, so
    // there is no negative-rounding case to handle here.
    return static_cast<std::uint32_t>(delta + 0.5);
}

// ---- Discovery (spec p.42-45) ----

// 60-byte discovery request: seq(4)=0, byte 4 = 0x02 (Discovery Command).
std::array<std::uint8_t, 60> buildDiscovery() noexcept;

struct DiscoveryReply {
    bool streaming = false;           // byte 4 == 0x03: hardware already has a session
    std::array<std::uint8_t, 6> mac{};// bytes 5-10, MSB first (p.43)
    std::uint8_t boardId = 0;         // byte 11; 10 = SATURN (ANAN-G2)
    std::uint8_t protoVersionRaw = 0; // byte 12, decimal-tenths (43 -> "4.3") -- the one
                                       // place the spec's own worked example is right
    std::uint8_t firmwareVer = 0;     // byte 13, an INTEGER gateware/build number, NOT
                                       // tenths despite the spec's worked example implying
                                       // one -- p2app writes GetFirmwareVersion() here
    std::uint8_t numDdc = 0;          // byte 20 -- CLAMP TO THIS, never probe past it. The
                                       // gateware may carry more DDC registers (VNUMDDC 10)
                                       // than p2app advertises (RFC Gap C).
    bool freqIsPhaseWord = false;     // byte 21: 0 = Hz, 1 = phase word
    std::uint8_t endianByteRaw = 0;   // byte 22, RAW -- see discoveryDeclaresBigEndian3Byte()
    std::uint8_t p2appBuild = 0;      // byte 23. "Beta version" per spec; p2app overwrites
                                       // it with P2APPVERSION on Saturn

    // Discovery-time PICKER filter only -- NOT for gating backend behaviour
    // once connected. The RFC is explicit that board type is a runtime
    // option on this platform (p2app.c:588 accepts -i saturn / -i orionmk2)
    // and must not decide what the backend does; the capability bytes and
    // firmware version are the load-bearing fields for that. This predicate
    // only answers "should a radio picker show this reply as an ANAN-G2".
    [[nodiscard]] constexpr bool isSaturn() const noexcept { return boardId == 10; }
};

// Byte 22 decoded per its OWN spec meaning (p.45): zero is the sentinel for
// "Big-Endian and 3-byte I&Q format", not "no formats declared" and not
// little-endian -- an early discovery-probe script inverted this with a bare
// C-style truthiness read (`'big' if pkt[22] else 'little'`) and it must
// never happen here. A nonzero byte is a bitmask whose bit 0 is the explicit
// BE flag. Measured against a real G2 (byte22==0): BE + 3-byte is the actual
// wire format, confirmed by an 80.0 dB SNR carrier decode vs 26.0 dB
// byte-swapped on the same capture (anan/spike/phase1a.py, WWV 10 MHz) --
// this is what buildGeneral() requests via byte 39, not a guess.
[[nodiscard]] constexpr bool discoveryDeclaresBigEndian3Byte(std::uint8_t byte22) noexcept
{
    return byte22 == 0 || (byte22 & 0x01) != 0;
}

// Parse a Discovery Reply (>=24 bytes: header through byte 23). Bounds-checks
// before indexing (Constitution Principle VII) -- this parses unauthenticated
// UDP.
std::optional<DiscoveryReply> parseDiscoveryReply(std::span<const std::uint8_t> data) noexcept;

// ---- General Packet (spec p.18-22) ----

// 60-byte General Packet. Port fields (bytes 5-22) are left at their
// zero/default values -- "if set to zero the default port will be used"
// (p.19-20) -- no renegotiation in Phase 1b. Byte 37 bit[3] = 1 (phase word,
// matching Discovery byte 21); byte 38 bit[0] = 1 (hardware reset timer /
// watchdog ON -- p.8: "a C&C packet must be sent at least every second
// (100 mS recommended) or the hardware switches out of RUN into standby".
// Left ON deliberately: this is the mechanism that self-heals an abandoned
// session -- Gap E measured a clean recovery from an unhandled `kill -9` on
// the client process, unlike the HL2's documented stranding); byte 39 =
// 0x00 (BE + 3-byte -- the only format Discovery byte 22==0 declares,
// confirmed by the 80.0 dB vs 26.0 dB measurement above, not a guess).
std::array<std::uint8_t, 60> buildGeneral() noexcept;

// ---- DDC-Specific Packet (spec p.23-26) ----

// 1444-byte DDC-Specific packet enabling DDC0 only, at `ddc0RateKsps` (must
// be one of 48/96/192/384/768/1536, p.24). Every other DDC stays disabled.
// `numAdcs` defaults to 2 -- the G2's two phase-synchronous ADCs (Apache
// Labs published spec) -- and is a parameter rather than a silent constant
// because it is a hardware fact this backend has not yet confirmed the
// radio would reject a wrong value for.
//
// `ditherEnabled`/`randomEnabled` set byte 5/6 -- one bit per ADC, bit N
// for ADCn (spec p.25's worked description of both fields). Applied to
// BOTH ADC0 and ADC1 bits together -- this radio exposes one Dither/Random
// control, not a per-ADC pair, so both bits track it in lockstep
// regardless of which ADC `ddc0AdcIndex` actually selects.
//
// `ddc0AdcIndex` sets byte 17, which the spec (p.25) describes as choosing
// which ADC's data DDC0 is fed from. 0 = ADC0, downstream of the switched
// Ant1/2/3 relay bank; 1 = ADC1, wired directly to its own RX2 jack --
// per the Appendix D block diagram (p.90), the only two receive paths this
// board offers into a DDC.
std::array<std::uint8_t, 1444> buildDdcSpecific(int ddc0RateKsps = 48, int numAdcs = 2,
                                                bool ditherEnabled = true,
                                                bool randomEnabled = true,
                                                int ddc0AdcIndex = 0) noexcept;

// ---- High Priority to Hardware (spec p.31-34) ----

// 1444-byte High Priority packet. `run` sets byte 4 bit[0] ONLY -- there is
// no PTT parameter in this function's signature, full stop, matching the
// spike's structural guarantee (Constitution Principle VI: AetherSDR never
// transmits without operator intent -- this codec cannot key even if a
// caller wanted it to; keying is a separate future addition gated behind
// the engine's TX arbiter). `ddc0FreqWord` sets bytes 9-12 (DDC0
// frequency/phase word, p.32).
//
// MUST be re-sent at least once a second (100 ms recommended, p.8) while a
// session should stay in RUN -- P2Client (a later commit) owns that
// keepalive cadence; this function only builds one packet.
//
// Also sets the Alex0 register (bytes 1432-1435, p.35 + Appendix D's
// SATURN/ANAN-G2-specific block diagram and bit table, p.90-91)
// unconditionally: ANT1 (bit 24), and HF Bypass (bit 12) when
// `bypassAdc0Filters` is true. Not a guess: "At power on all 32 bits are
// set to 0 by the FPGA code and remain in that state until an Alex command
// is received" (p.79) -- zero is a real, documented "relay de-energised"
// state, not a harmless default, on a board whose 3 antenna ports are
// explicitly software-selected. ANT1 always goes out; the block diagram
// (p.90) shows the RX1 BPF1 filter bank sitting IN SERIES between the
// antenna relay and the ADC, with its own dedicated Bypass relay -- with
// neither a specific band filter nor Bypass energised, that bank has no
// signal path through it at all (an antenna correctly routed to a dead end
// reads identically to no antenna). ANT2/ANT3 selection and per-band BPF
// are not exposed; wire them through AnanBackend/RadioConnectRequest::params
// when that becomes a real need.
//
// `bypassAdc1Filters` sets bit 12 of the SEPARATE Alex1 (BPF2/RX2) register
// (bytes 1430-1431, p.90-91's own "Alex1" bit table: bit 12 "HF Bypass 2"),
// the RX2/ADC1 filter bank's own bypass relay. RX2 has no antenna-select
// bits of its own in that table -- it is a single, dedicated jack, not a
// software-selected one -- so there is nothing here for ANT2/ANT3-style
// params to apply to.
std::array<std::uint8_t, 1444> buildHighPriority(bool run, std::uint32_t ddc0FreqWord,
                                                  bool bypassAdc0Filters = true,
                                                  bool bypassAdc1Filters = true) noexcept;

// ---- DDC I&Q Data (spec p.53-54) ----

inline constexpr int kDdcHeaderLen = 16;   // seq(4) + timestamp(8) + bitsPerSample(2) + samplesPerFrame(2)
inline constexpr int kDdcSampleBytes = 6;  // 3-byte I + 3-byte Q

struct DdcFrame {
    std::uint32_t seq = 0;
    int samples = 0;
    // Exactly samples * kDdcSampleBytes bytes, big-endian 24-bit I then Q
    // per sample. Borrows from the `data` span passed to parseDdcFrame() --
    // must not outlive it.
    std::span<const std::uint8_t> iqRaw;
};

// Parse and STRICTLY VALIDATE a DDC I&Q datagram. Rejects (returns nullopt)
// unless bitsPerSample == 24 AND data.size() == kDdcHeaderLen + samples *
// kDdcSampleBytes EXACTLY.
//
// This is a strict reject, not a clamp, on purpose. Every radio->PC stream
// -- Mic Data, High Priority Status, DDC0 IQ -- lands on the SAME PC-side
// port by default (see kDdc0DefaultPort's comment), and p2app streams Mic
// Data (p.51: 64 samples/packet, mono, 48 ksps) continuously once run=1
// with NO DUC/TX configured at all. Measured directly: before this check
// existed, Mic packets landing on a DDC0 listener were misdecoded as tiny
// garbage "DDC frames" -- producing an apparent ~40%-of-packets sequence-gap
// rate and a ~3x-too-high sample rate that were both artifacts of
// misparsing, not a real link problem. A genuine DDC0 frame's declared
// sample count exactly accounts for its datagram's length because p2app
// builds these deliberately; any mismatch means "not a DDC0 frame", not "a
// truncated one" -- clamping here would paper over exactly the ambiguity
// this check exists to remove.
std::optional<DdcFrame> parseDdcFrame(std::span<const std::uint8_t> data) noexcept;

// Decode one 24-bit signed big-endian I/Q sample pair (6 bytes) to
// normalized [-1, 1) values. BE ONLY -- the wire-format question is closed
// (see discoveryDeclaresBigEndian3Byte()); no little-endian path exists in
// production code, unlike the spike's diagnostic dual-decode.
std::complex<float> decodeIqSample(const std::uint8_t* be6) noexcept;

// Decode every sample in `frame.iqRaw` and append to `out`. Cannot fail --
// `frame` came from a successful parseDdcFrame(), whose exact-length check
// already guarantees `iqRaw.size() == frame.samples * kDdcSampleBytes`.
void decodeIq(const DdcFrame& frame, std::vector<std::complex<float>>& out);

}  // namespace AetherSDR::anan
