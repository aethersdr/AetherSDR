#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// CI-V — the Icom command plane.
//
// This is the half of the stack that is COMMON to every transport. Over WiFi it
// travels inside the RS-BA1 serial stream (IcomProtocol.h); over USB the exact
// same bytes go straight down a serial port with no envelope at all. Keeping
// this file transport-free is what makes a future USB / local-serial mode a
// small increment rather than a second backend.
//
// Grounded on Icom's own IC-705 CI-V Reference Guide (A7560-8EX-1, Jul 2020),
// which is tier 1 in ~/oracles/icom/icom-oracle.md §0. Command numbers and
// data ranges are facts from that document; nothing here is transcribed from
// wfview (GPL-3, read-only reference).
//
// Qt-free and socket-free so civ_codec_test exercises it standalone.

namespace AetherSDR::icom {

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

inline constexpr std::uint8_t kCivPreamble = 0xFE;
inline constexpr std::uint8_t kCivEom      = 0xFD;   // end of message
inline constexpr std::uint8_t kCivOk       = 0xFB;   // acknowledged
inline constexpr std::uint8_t kCivNg       = 0xFA;   // rejected

// Our address as a controller. 0xE0 is the conventional default and what every
// Icom ships expecting.
inline constexpr std::uint8_t kControllerAddress = 0xE0;

// Broadcast/"any" — some radios answer transceive with this as the destination
// rather than with our address, so the receive path must accept it.
inline constexpr std::uint8_t kBroadcastAddress = 0x00;

// The cap past which an unterminated frame is a resync artefact rather than a
// frame.
//
// NOT 80. Hamlib and kappanhang both use 80 and both are right for what they
// do — 80 is the longest COMMAND frame, and neither of them decodes scope data.
// A scope sweep is command 0x27 0x00 carrying 475 points, which is ~496 bytes
// on an IC-705 and ~710 on an IC-7610, so an 80-byte cap silently discards
// every panadapter frame while commands keep working perfectly. The symptom is
// a black waterfall on a session that is demonstrably healthy.
//
// 1200 covers the widest sweep any current model produces with room to spare.
inline constexpr std::size_t kMaxFrameBytes = 1200;

// The longest frame we will ever SEND, and the cap for frames forwarded from a
// host serial port. This is the 80 that Hamlib means.
inline constexpr std::size_t kMaxCommandFrameBytes = 80;

// A decoded CI-V frame, with the preamble and terminator stripped.
struct CivFrame {
    std::uint8_t to   = 0;
    std::uint8_t from = 0;
    std::uint8_t cmd  = 0;
    // Whether `sub` is meaningful. A bare command (0x03 "read frequency") has
    // no subcommand, and defaulting `sub` to 0 would make it indistinguishable
    // from subcommand 0x00 — which is a real, different thing on 0x27 and 0x1C.
    bool hasSub = false;
    std::uint8_t sub = 0;
    std::vector<std::uint8_t> data;

    [[nodiscard]] bool isOk() const noexcept { return cmd == kCivOk; }
    [[nodiscard]] bool isNg() const noexcept { return cmd == kCivNg; }
};

// Build a frame. `to` is the radio's CI-V address.
[[nodiscard]] std::vector<std::uint8_t> buildFrame(std::uint8_t to, std::uint8_t cmd,
                                                   std::span<const std::uint8_t> payload = {});
[[nodiscard]] std::vector<std::uint8_t> buildFrameSub(std::uint8_t to, std::uint8_t cmd,
                                                      std::uint8_t sub,
                                                      std::span<const std::uint8_t> payload = {});

// Whether a command is sub-addressed — i.e. whether the byte after it is a
// subcommand or the first byte of the payload. This is a per-command fact and
// there is exactly ONE list of it: parseFrame() decodes by it, and the CI-V
// trace labels by it. A second copy can drift, and a drift here produces the
// wrong-but-plausible decode the enumeration exists to prevent.
[[nodiscard]] bool commandHasSubcommand(std::uint8_t command);

// Decode one complete frame (FE FE … FD). Returns nullopt if it is malformed.
[[nodiscard]] std::optional<CivFrame> parseFrame(std::span<const std::uint8_t> frame);

// Reassembles CI-V frames from an arbitrarily-chunked byte stream.
//
// The stream is NOT frame-aligned: a datagram can split a frame, batch several,
// or start mid-frame after a loss. Three rules make resynchronisation reliable,
// and all three come from the reference implementations having been bitten:
//
//   1. Sync on the DOUBLE preamble. A single 0xFE is not a frame start — it
//      appears inside BCD data — and accepting one leaves the decoder
//      permanently one frame behind.
//   2. Terminate on 0xFD, and also on 0xFC. The radio uses 0xFC as an alternate
//      terminator in some replies.
//   3. Time out a partial frame. Without it, one truncated frame swallows every
//      subsequent byte forever. The caller drives this via tick().
class CivReassembler {
public:
    // Feed bytes; returns every complete frame that became available.
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> feed(std::span<const std::uint8_t> bytes);
    // Abandon a partial frame. Call when the frame timeout (100 ms) expires.
    void timeout() noexcept;
    void reset() noexcept;
    [[nodiscard]] bool framePending() const noexcept { return m_started; }

private:
    std::vector<std::uint8_t> m_buf;
    bool m_started = false;   // both preamble bytes seen
    bool m_sawOne  = false;   // exactly one preamble byte seen
};

// ---------------------------------------------------------------------------
// BCD — and the two directions it runs
// ---------------------------------------------------------------------------
//
// This is the single most common source of Icom bugs, so it gets named helpers
// rather than open-coded shifts:
//
//   FREQUENCY runs LITTLE-endian  — least-significant digit pair FIRST.
//       14.250000 MHz -> 00 60 25 14 00
//   EVERYTHING ELSE runs BIG-endian — most-significant pair first.
//       level 0255    -> 02 55
//
// Mixing them produces a radio that tunes to 21.4 MHz when asked for 14.21, or
// a power level of 55 % when asked for 2 %.

// Frequency, in Hz, as N BCD bytes little-endian. 5 bytes on every current
// model; the IC-905 uses 6 above 10 GHz, which is why this is a parameter and
// not a hardcoded literal.
inline constexpr std::size_t kFreqBytes = 5;
[[nodiscard]] std::vector<std::uint8_t> encodeFreq(std::uint64_t hz,
                                                    std::size_t bytes = kFreqBytes);
[[nodiscard]] std::optional<std::uint64_t> decodeFreq(std::span<const std::uint8_t> bcd);

// A scope EDGE frequency, which can be NEGATIVE.
//
// The IC-7300MK2's CI-V guide documents `0xF` in the 1 GHz digit — the high
// nibble of the last byte — as a sign flag meaning the lower edge is negative.
// That happens when a wide span sits near the bottom of the tuning range, so
// the scope window extends below 0 Hz.
//
// decodeFreq() stays STRICT and rejects it, deliberately: an OPERATING
// frequency is never negative, and a corrupt one that decodes to something
// still retunes the radio — on transmit that is an out-of-band emission. Only
// the scope decoder should reach for this variant.
[[nodiscard]] std::optional<std::int64_t> decodeFreqSigned(std::span<const std::uint8_t> bcd);

// A 0000..9999 value as two BCD bytes, big-endian. This is the shape of every
// level, meter reading and menu index in the protocol.
[[nodiscard]] std::array<std::uint8_t, 2> encodeLevel(int value);
[[nodiscard]] std::optional<int> decodeLevel(std::span<const std::uint8_t> bcd);

// Icom continuous controls expose a 0000..0255 register while the radio's
// front panel and AetherSDR both present 0..100. The front panel truncates on
// read, so writes must select the first raw value in the requested percentage
// bucket. Keeping the pair here prevents individual controls from drifting by
// one through different rounding rules.
[[nodiscard]] int percentToLevelRaw(int percent);
[[nodiscard]] int levelRawToPercent(int raw);

// A single BCD byte, 00..99.
[[nodiscard]] std::uint8_t encodeBcdByte(int value);
[[nodiscard]] int decodeBcdByte(std::uint8_t b);

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

namespace cmd {
inline constexpr std::uint8_t kSetFreqTrx   = 0x00;   // transceive: freq changed
inline constexpr std::uint8_t kSetModeTrx   = 0x01;   // transceive: mode changed
inline constexpr std::uint8_t kReadFreq     = 0x03;
inline constexpr std::uint8_t kReadMode     = 0x04;
inline constexpr std::uint8_t kSetFreq      = 0x05;
inline constexpr std::uint8_t kSetMode      = 0x06;
inline constexpr std::uint8_t kLevel        = 0x14;   // sub-addressed levels
inline constexpr std::uint8_t kMeter        = 0x15;   // sub-addressed meters
inline constexpr std::uint8_t kFunction     = 0x16;   // sub-addressed on/off functions
inline constexpr std::uint8_t kPower        = 0x18;   // 00 off, 01 on
inline constexpr std::uint8_t kReadId       = 0x19;   // sub 00: read transceiver ID
inline constexpr std::uint8_t kSetting      = 0x1A;   // memory / filter / SET menu
inline constexpr std::uint8_t kControl      = 0x1C;   // PTT, tuner, XFC
inline constexpr std::uint8_t kScope        = 0x27;
// The attenuator, and it is NOT sub-addressed like 0x14/0x16 — the single
// data byte IS the setting, in BCD dB. The IC-705 takes 00 (off) and 20
// (20 dB, HF and 50 MHz only); other models publish other steps, which is
// why the backend advertises the positions rather than assuming them.
inline constexpr std::uint8_t kAttenuator  = 0x11;
// IC-7300MK2 receive-only antenna switch: sub 00, 00=main antenna, 01=RX-ANT.
inline constexpr std::uint8_t kRxAntenna   = 0x12;
// RIT / dTX. Icom calls transmit incremental tuning "dTX"; the operator-facing
// name everywhere else is XIT, and they are the same control.
inline constexpr std::uint8_t kTuneOffset   = 0x21;
// OPERATING MODE, DATA STATE AND IF FILTER — ALL THREE IN ONE FRAME.
//
// This is the command that tells USB from USB-D. Commands 01/04/06 carry only
// the mode byte, and USB and USB-D share it (0x01), so nothing in that family
// can express or report the DATA flag. 0x26 carries mode, DATA on/off and the
// filter slot together, per VFO, and it is both readable and writable.
//
// ONE FRAME MATTERS, not just one command. Setting the three separately lets
// them clobber each other: on a real radio an ordinary-mode write can clear
// DATA, so a mode change followed by a DATA write is a race with the radio's
// own side effects, and a filter change sent as a plain mode write drops the
// operator straight out of DATA. 0x26 makes the three a single transaction the
// radio applies or refuses as a unit.
inline constexpr std::uint8_t kVfoMode      = 0x26;
}  // namespace cmd

// The subcommand of 0x26 is the VFO it addresses.
//
// SELECTED ONLY, today. The unselected VFO is where split lives, and modelling
// split needs a second frequency the backend does not yet carry — publishing
// the unselected VFO's mode without it would be a mode indicator for a VFO
// nothing else in the app knows about. The constant is named so the split work
// has somewhere to land rather than a bare 0x01 appearing later.
namespace vfoMode {
inline constexpr std::uint8_t kSelected   = 0x00;
inline constexpr std::uint8_t kUnselected = 0x01;
}  // namespace vfoMode

namespace level {
inline constexpr std::uint8_t kAf        = 0x01;
inline constexpr std::uint8_t kRf        = 0x02;
inline constexpr std::uint8_t kSquelch   = 0x03;
inline constexpr std::uint8_t kNrLevel   = 0x06;
inline constexpr std::uint8_t kCwPitch   = 0x09;
inline constexpr std::uint8_t kRfPower   = 0x0A;
inline constexpr std::uint8_t kMicGain   = 0x0B;
inline constexpr std::uint8_t kKeySpeed  = 0x0C;
inline constexpr std::uint8_t kCompLevel = 0x0E;
inline constexpr std::uint8_t kNbLevel   = 0x12;
// The manual notch's POSITION within the passband, 0000..0255 — not a
// frequency, and not a depth. It moves with the filter, which is why the
// seam carries this as a percentage rather than Hz.
inline constexpr std::uint8_t kNotchPos  = 0x0D;
// VOX gain — the trigger threshold. NOT the delay: the guide puts VOX DELAY in
// the SET menu at 1A 05 0359 (00..20), and 14 17 is the ANTI-vox gain, which is
// a different control again.
inline constexpr std::uint8_t kVoxGain   = 0x16;
inline constexpr std::uint8_t kAntiVox   = 0x17;
inline constexpr std::uint8_t kMonitor   = 0x15;
}  // namespace level

namespace meter {
inline constexpr std::uint8_t kSquelchStatus = 0x01;
inline constexpr std::uint8_t kSMeter        = 0x02;
inline constexpr std::uint8_t kOverflow      = 0x07;   // ADC OVF indicator
inline constexpr std::uint8_t kPower         = 0x11;
inline constexpr std::uint8_t kSwr           = 0x12;
inline constexpr std::uint8_t kAlc           = 0x13;
inline constexpr std::uint8_t kComp          = 0x14;
inline constexpr std::uint8_t kVd            = 0x15;   // PA supply rail
inline constexpr std::uint8_t kId            = 0x16;   // PA current
}  // namespace meter

namespace func {
inline constexpr std::uint8_t kPreamp        = 0x02;   // 00 off, 01 P.AMP1, 02 P.AMP2
inline constexpr std::uint8_t kAgc           = 0x12;   // 01 fast, 02 mid, 03 slow
inline constexpr std::uint8_t kNoiseBlanker  = 0x22;
inline constexpr std::uint8_t kNoiseReduce   = 0x40;
inline constexpr std::uint8_t kAutoNotch     = 0x41;
inline constexpr std::uint8_t kCompressor    = 0x44;
inline constexpr std::uint8_t kMonitorFn     = 0x45;
inline constexpr std::uint8_t kVox           = 0x46;
inline constexpr std::uint8_t kManualNotch   = 0x48;
// 00 wide, 01 mid, 02 narrow. Read at connect and left alone otherwise —
// no seam verb carries a notch width, and overwriting the operator's own
// choice would be worse than not offering it.
inline constexpr std::uint8_t kManualNotchWidth = 0x57;
inline constexpr std::uint8_t kBreakIn       = 0x47;   // 00 off, 01 semi, 02 full
inline constexpr std::uint8_t kDialLock      = 0x50;
}  // namespace func

// SET-menu items reached through 1A 05 <two BCD bytes>.
//
// MOD INPUT IS THE ONE THAT DECIDES WHETHER WE ARE HEARD AT ALL. The radio
// modulates from ONE source per mode class, and if that source is not WLAN then
// every byte of network audio we send is discarded — the radio keys, transmits
// a bare carrier or its own microphone, and reports zero forward power. There
// is no error anywhere: the transmit path is working perfectly into a modulator
// that is listening somewhere else.
namespace setting {
inline constexpr int kVoxDelay        = 359;   // 00..20, in 0.1 s steps
}  // namespace setting

// Read or write a 1A 05 SET-menu item. `item` is the DECIMAL menu number as
// printed in the guide (118, 119, ...); it is encoded as two BCD bytes.
[[nodiscard]] std::vector<std::uint8_t> cmdReadSetting(std::uint8_t to, int item);
[[nodiscard]] std::vector<std::uint8_t> cmdWriteSetting(std::uint8_t to, int item,
                                                        std::uint8_t value);

// Command 0x21 — RIT and dTX (which is what Icom calls XIT).
namespace tuneOffset {
inline constexpr std::uint8_t kFrequency = 0x00;   // signed, +/- 9.99 kHz
inline constexpr std::uint8_t kRitOnOff  = 0x01;
inline constexpr std::uint8_t kXitOnOff  = 0x02;   // the guide writes this dTX
}  // namespace tuneOffset

namespace control {
inline constexpr std::uint8_t kPtt   = 0x00;   // 00 RX, 01 TX
// THE ANTENNA TUNER, not a tune carrier. 00 off, 01 on, 02 start a matching
// cycle. AetherSDR's setTune() means "raise a steady carrier at tune power",
// which on an Icom is COMPOSED — save mode, switch to RTTY/CW, set drive,
// key with kPtt — not commanded. Wiring the TUNE button here gives the operator
// a control that runs an ATU which may not even be attached.
inline constexpr std::uint8_t kTuner = 0x01;
inline constexpr std::uint8_t kXfc   = 0x02;
inline constexpr std::uint8_t kReadTxFreq = 0x03;
}  // namespace control

namespace scope {
inline constexpr std::uint8_t kWaveData    = 0x00;
// BOTH of these must be ON before 0x27 0x00 produces anything. Enabling only
// kOnOff turns the scope on the radio's own screen and sends us nothing — the
// number-one "my panadapter is black" cause.
inline constexpr std::uint8_t kOnOff       = 0x10;
inline constexpr std::uint8_t kDataOutput  = 0x11;
inline constexpr std::uint8_t kMainSub     = 0x12;   // IC-705: fixed 00
inline constexpr std::uint8_t kSingleDual  = 0x13;   // IC-705: fixed 00
inline constexpr std::uint8_t kMode        = 0x14;   // 0000 centre, 0001 fixed
inline constexpr std::uint8_t kSpan        = 0x15;   // centre mode only
inline constexpr std::uint8_t kEdgeNumber  = 0x16;   // fixed mode, 0001..0003
inline constexpr std::uint8_t kHold        = 0x17;
inline constexpr std::uint8_t kReference   = 0x19;   // -20.0..+20.0 dB, 0.5 steps
inline constexpr std::uint8_t kSweepSpeed  = 0x1A;   // 0000..0002
inline constexpr std::uint8_t kFixedEdge   = 0x1E;
}  // namespace scope

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

// The wire values of command 0x06 / 0x04's first data byte.
enum class CivMode : std::uint8_t {
    Lsb  = 0x00,
    Usb  = 0x01,
    Am   = 0x02,
    Cw   = 0x03,
    Rtty = 0x04,
    Fm   = 0x05,
    Wfm  = 0x06,
    CwR  = 0x07,
    RttyR = 0x08,
    Dv   = 0x17,
};

// Translate to and from AetherSDR's neutral mode vocabulary (the strings
// SliceModel uses). Returns nullopt for a mode this radio has no equivalent
// for, rather than silently substituting one — a slice that asked for DIGU and
// got USB is a decoder that will not decode.
[[nodiscard]] std::optional<CivMode> modeFromNeutral(const std::string& neutral,
                                                      bool& dataModeOut);
[[nodiscard]] std::string modeToNeutral(CivMode mode, bool dataMode);

// The three things command 0x26 carries about one VFO.
struct VfoModeState {
    CivMode mode = CivMode::Usb;
    bool dataMode = false;
    // 1..3, or 0 when the radio named a slot outside that range. Zero means
    // "unspecified", NOT slot 0: the filter ladder is defined over 1..3, and
    // adopting an out-of-range byte as a slot would drop the operator's IF
    // filter out of it. The caller keeps whatever slot it already had.
    int filter = 0;
};

// Filter selection. The IC-705 offers three fixed IF filters, not a continuous
// passband, so a request in Hz can only SNAP. Returns 1, 2 or 3.
//
// The widths are the radio's own SSB defaults; the operator can redefine them
// in the SET menu and we have no way to read the redefinition back. So this is
// a best-effort mapping, and the honest consequence is that the passband the
// UI shows must come from what the radio reports, never from what we asked for.
//
// MODE-DEPENDENT, and that is the whole point. The radio has three filter slots
// and what each one MEANS changes with the mode: FIL1 is 3.0 kHz in SSB, 1.2 kHz
// in CW, 9 kHz in AM and 15 kHz in FM. Snapping every mode against the SSB
// thresholds — which this used to do — put every AM width on FIL1 and every CW
// width on FIL3, so the operator had three buttons and one filter.
//
// Ladders from the IC-705's own defaults (hamlib rigs/icom/ic7300.c,
// RIG_MODEL_IC705). The operator can redefine them in the SET menu and there is
// no command to read the redefinition back, so this stays best-effort — and the
// consequence is load-bearing: the passband the UI shows must come from what the
// radio REPORTS after the change, never from the width we asked for.
[[nodiscard]] int filterForWidthHz(const std::string& mode, int widthHz) noexcept;

// The widths that mode's filter slots hold, NARROWEST FIRST — deliberately the
// reverse of the radio's own FIL1/FIL2/FIL3 numbering, in which FIL1 is the
// widest. Published as RadioCapabilities::rxFilterWidthsHz so the filter buttons
// offer what the hardware actually has instead of a Flex-shaped eight, and in
// the same narrow-to-wide order as every other filter row in the app.
[[nodiscard]] std::vector<int> filterWidthsForMode(const std::string& mode);

// The passband that filter gives in that mode, in Hz relative to the carrier,
// sign carrying the sideband (SliceModel's convention). The backend needs this
// because an IC-705's IF filters cannot be read back as Hz — nothing else in
// the chain can fill the window in.
[[nodiscard]] std::pair<int, int> passbandForModeAndFilter(const std::string& mode,
                                                          int filter);

// ---------------------------------------------------------------------------
// Command builders
// ---------------------------------------------------------------------------

[[nodiscard]] std::vector<std::uint8_t> cmdSetFrequency(std::uint8_t to, std::uint64_t hz);
[[nodiscard]] std::vector<std::uint8_t> cmdReadFrequency(std::uint8_t to);
[[nodiscard]] std::vector<std::uint8_t> cmdSetMode(std::uint8_t to, CivMode mode, int filter);
[[nodiscard]] std::vector<std::uint8_t> cmdReadMode(std::uint8_t to);
// Command 0x26 — the selected VFO's mode, DATA state and filter, read and
// written as one unit. See cmd::kVfoMode for why the three travel together.
[[nodiscard]] std::vector<std::uint8_t> cmdReadVfoMode(std::uint8_t to);
[[nodiscard]] std::vector<std::uint8_t> cmdSetVfoMode(std::uint8_t to, CivMode mode,
                                                      bool dataMode, int filter);
// Decode the PAYLOAD of a 0x26 frame — the bytes after the VFO subcommand.
// Returns nullopt for anything the radio could not have meant: short of three
// bytes, or a DATA byte that is neither 00 nor 01. The frames this parses
// arrive from the network, so it validates rather than indexes (Constitution
// VII).
[[nodiscard]] std::optional<VfoModeState> decodeVfoMode(std::span<const std::uint8_t> payload);
[[nodiscard]] std::vector<std::uint8_t> cmdSetLevel(std::uint8_t to, std::uint8_t which, int value);
[[nodiscard]] std::vector<std::uint8_t> cmdReadMeter(std::uint8_t to, std::uint8_t which);
// The READ forms of 0x14 and 0x16 — same subcommand, no payload. The radio
// answers with the current value, which is the only way to open a control at
// the position the radio is actually in rather than at our own default.
[[nodiscard]] std::vector<std::uint8_t> cmdReadLevel(std::uint8_t to, std::uint8_t which);
[[nodiscard]] std::vector<std::uint8_t> cmdReadFunction(std::uint8_t to, std::uint8_t which);
[[nodiscard]] std::vector<std::uint8_t> cmdSetFunction(std::uint8_t to, std::uint8_t which,
                                                        int value);
[[nodiscard]] std::vector<std::uint8_t> cmdSetPtt(std::uint8_t to, bool transmit);
// Attenuator. `db` is the dB figure the radio prints (0 or 20 on an
// IC-705), encoded as one BCD byte. The read form carries no payload.
[[nodiscard]] std::vector<std::uint8_t> cmdSetAttenuator(std::uint8_t to, int db);
[[nodiscard]] std::vector<std::uint8_t> cmdReadAttenuator(std::uint8_t to);
[[nodiscard]] std::vector<std::uint8_t> cmdSetRxAntenna(std::uint8_t to, bool rxAntenna);
// RIT / dTX read forms, and the antenna tuner. `21 xx` with no payload asks;
// `1C 01` with no payload asks whether the tuner is on, off or mid-cycle.
[[nodiscard]] std::vector<std::uint8_t> cmdReadTuneOffset(std::uint8_t to, std::uint8_t sub);
[[nodiscard]] std::vector<std::uint8_t> cmdSetTuner(std::uint8_t to, std::uint8_t value);
[[nodiscard]] std::vector<std::uint8_t> cmdReadTuner(std::uint8_t to);
[[nodiscard]] std::vector<std::uint8_t> cmdReadId(std::uint8_t to);
[[nodiscard]] std::vector<std::uint8_t> cmdScopeOnOff(std::uint8_t to, bool on);
[[nodiscard]] std::vector<std::uint8_t> cmdScopeDataOutput(std::uint8_t to, bool on);
[[nodiscard]] std::vector<std::uint8_t> cmdScopeMode(std::uint8_t to, bool fixed);
[[nodiscard]] std::vector<std::uint8_t> cmdScopeSpan(std::uint8_t to, int spanHz);
[[nodiscard]] std::vector<std::uint8_t> cmdScopeReference(std::uint8_t to, double db);

// RIT / dTX. The OFFSET is a signed magnitude: two BCD bytes little-endian
// holding 0000..9999 Hz, then a sign byte (00 plus, 01 minus) — the same
// separate-sign shape the scope reference level uses, and the same mistake is
// available (folding the sign into the magnitude tunes the wrong way).
[[nodiscard]] std::vector<std::uint8_t> cmdTuneOffsetHz(std::uint8_t to, int hz);
[[nodiscard]] std::vector<std::uint8_t> cmdRitEnable(std::uint8_t to, bool on);
[[nodiscard]] std::vector<std::uint8_t> cmdXitEnable(std::uint8_t to, bool on);

// The eight spans the IC-705 offers, in Hz. Anything else SNAPS to the nearest.
inline constexpr std::array<int, 8> kScopeSpansHz{
    2'500, 5'000, 10'000, 25'000, 50'000, 100'000, 250'000, 500'000,
};
[[nodiscard]] int nearestScopeSpanHz(int requestedHz) noexcept;

// The span one detent away from `spanHz`, in the given direction (-1 narrower,
// +1 wider). Clamps at the ends of the table rather than wrapping.
//
// WHY THIS EXISTS, because "snap to nearest" looks like it should be enough and
// is not. The spans above are spaced by ratios of 2 and 2.5, while the zoom
// buttons scale the view by 1.5. Multiplying a span by 1.5 therefore NEVER
// reaches the midpoint of the gap to the next one up, so nearest-snapping a
// zoom-out request always returns the span it started from. Measured across the
// whole table, zoom-out was inert at all eight spans and zoom-in worked at
// seven — an asymmetry that reads as "zoom is broken" rather than "zoom is
// quantised", and which no amount of clicking can escape because the view is
// re-seeded from the radio's own sweep 30 times a second.
[[nodiscard]] int adjacentScopeSpanHz(int spanHz, int direction) noexcept;

}  // namespace AetherSDR::icom
