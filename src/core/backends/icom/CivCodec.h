#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// CI-V — the Icom command plane, common to every transport: over WiFi it rides
// inside the RS-BA1 serial stream (IcomProtocol.h), over USB the same bytes go
// straight to a serial port. Keep this file transport-, Qt- and socket-free so
// civ_codec_test exercises it standalone.
//
// Grounded on Icom's IC-705 CI-V Reference Guide (A7560-8EX-1, Jul 2020);
// nothing here is transcribed from wfview (GPL-3, read-only reference).

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

// Cap past which an unterminated frame is a resync artefact. Not hamlib's 80
// (the longest COMMAND frame): a 0x27 0x00 scope sweep of 475 points is ~496
// bytes on an IC-705 and ~710 on an IC-7610. 1200 covers every current model.
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
// Explicit Power ON only. Native-network framing is selected by the model profile.
[[nodiscard]] std::vector<std::uint8_t> cmdPowerOn(std::uint8_t to,
    std::size_t extraPreambleBytes = 0, std::uint8_t from = kControllerAddress);

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

// Reassembles CI-V frames from an arbitrarily-chunked byte stream (datagrams can
// split, batch, or start mid-frame). Resync rules:
//   1. Sync on the DOUBLE 0xFE preamble; a single 0xFE appears inside BCD data.
//   2. Terminate on 0xFD, and also on 0xFC (alternate terminator in some replies).
//   3. Time out a partial frame via tick(), or one truncated frame swallows the
//      stream forever.
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

// BCD byte order:
//   FREQUENCY is LITTLE-endian — least-significant digit pair first.
//       14.250000 MHz -> 00 60 25 14 00
//   everything else is BIG-endian — most-significant pair first.
//       level 0255    -> 02 55

// Frequency, in Hz, as N BCD bytes little-endian. 5 bytes on every current
// model; the IC-905 uses 6 above 10 GHz, which is why this is a parameter and
// not a hardcoded literal.
inline constexpr std::size_t kFreqBytes = 5;
[[nodiscard]] std::vector<std::uint8_t> encodeFreq(std::uint64_t hz,
                                                    std::size_t bytes = kFreqBytes);
[[nodiscard]] std::optional<std::uint64_t> decodeFreq(std::span<const std::uint8_t> bcd);
// Use at command boundaries whose protocol shape declares an exact number of
// frequency bytes. Generic decodeFreq() intentionally supports multiple Icom
// models and therefore cannot enforce a command's arity by itself.
[[nodiscard]] std::optional<std::uint64_t> decodeFreqExact(
    std::span<const std::uint8_t> bcd, std::size_t expectedBytes);

// A scope EDGE frequency, which can be NEGATIVE: the IC-7300MK2 CI-V guide uses
// 0xF in the 1 GHz digit (high nibble of the last byte) as a sign flag when a
// wide span extends below 0 Hz. decodeFreq() stays strict because an operating
// frequency is never negative and a corrupt one would retune the radio; only the
// scope decoder uses this variant.
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
inline constexpr std::uint8_t kReadRepeaterOffset = 0x0C; // 100 Hz units, LE BCD
inline constexpr std::uint8_t kSetRepeaterOffset  = 0x0D;
inline constexpr std::uint8_t kDuplex       = 0x0F;   // 10 simplex, 11 down, 12 up
inline constexpr std::uint8_t kLevel        = 0x14;   // sub-addressed levels
inline constexpr std::uint8_t kMeter        = 0x15;   // sub-addressed meters
inline constexpr std::uint8_t kFunction     = 0x16;   // sub-addressed on/off functions
inline constexpr std::uint8_t kCwMessage    = 0x17;   // up to 30 ASCII characters; FF aborts
inline constexpr std::uint8_t kPower        = 0x18;   // 00 off, 01 on
inline constexpr std::uint8_t kReadId       = 0x19;   // sub 00: read transceiver ID
inline constexpr std::uint8_t kSetting      = 0x1A;   // memory / filter / SET menu
inline constexpr std::uint8_t kTone         = 0x1B;   // sub 00: repeater CTCSS frequency
inline constexpr std::uint8_t kControl      = 0x1C;   // PTT, tuner, XFC
inline constexpr std::uint8_t kGps          = 0x23;   // position / GPS source
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
// 0x26: operating mode, DATA flag and IF filter slot in ONE frame, per VFO,
// readable and writable. It is the only command that distinguishes USB from
// USB-D (01/04/06 carry only the shared mode byte). Send the three together: as
// separate writes, an ordinary mode write can clear DATA on the radio.
inline constexpr std::uint8_t kVfoMode      = 0x26;
}  // namespace cmd

[[nodiscard]] std::vector<std::uint8_t> cmdSendCwMessage(
    std::uint8_t to, std::string_view ascii);
[[nodiscard]] std::vector<std::uint8_t> cmdAbortCwMessage(std::uint8_t to);

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
// TWIN PBT — the two ends of the IF passband, 0000..0255 with 0128 at the
// centre. NOT a width and NOT a frequency: each is a SHIFT, and how many Hz a
// step is worth depends on the IF width currently in circuit (see pbtShiftHz).
//
// The pair is what makes an Icom's passband resizable at all. 1A 03 sets the
// WIDTH symmetrically about the filter centre; moving both PBTs together slides
// that window without changing it, and moving them apart narrows it from the
// inside. Low-cut and high-cut are therefore not two commands on an Icom —
// they are one width plus one shift, which is what setSliceFilter decomposes.
inline constexpr std::uint8_t kPbtInner  = 0x07;
inline constexpr std::uint8_t kPbtOuter  = 0x08;
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
inline constexpr std::uint8_t kRepeaterTone  = 0x42;
inline constexpr std::uint8_t kRepeaterAccess = 0x5D;
inline constexpr std::uint8_t kCompressor    = 0x44;
inline constexpr std::uint8_t kMonitorFn     = 0x45;
inline constexpr std::uint8_t kVox           = 0x46;
inline constexpr std::uint8_t kManualNotch   = 0x48;
// 00 wide, 01 mid, 02 narrow. Read at connect and left alone otherwise —
// no seam verb carries a notch width, and overwriting the operator's own
// choice would be worse than not offering it.
inline constexpr std::uint8_t kManualNotchWidth = 0x57;
// 00 SHARP, 01 SOFT — the DSP filter skirt for the operating band.
inline constexpr std::uint8_t kIfFilterShape = 0x56;
// WHICH SSB TRANSMIT BANDWIDTH IS IN CIRCUIT: 00 WIDE, 01 MID, 02 NAR. It
// selects one of three SET-menu slots; the EDGES themselves live in those slots
// (see ModelTxBandwidth), and which slot the radio uses also depends on whether
// the speech compressor is on. So this alone never tells you the passband —
// it tells you which stored passband to read.
inline constexpr std::uint8_t kTxBandwidth   = 0x58;
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
inline constexpr int kNtpEnabled      = 167;   // 00 off, 01 on
inline constexpr int kNtpServer       = 168;   // up to 64 ASCII characters
inline constexpr int kGpsTimeCorrect  = 169;   // 00 off, 01 auto
}  // namespace setting

// The SUBCOMMANDS of 0x1A. 0x05 is the SET menu everything above reaches
// through; 0x03 is a command in its own right and NOT a menu item.
namespace settingSub {
inline constexpr std::uint8_t kFilterWidth = 0x03;   // the IF width, in circuit now
inline constexpr std::uint8_t kMenu        = 0x05;   // 1A 05 <item> — the SET menu
inline constexpr std::uint8_t kNtpAccess   = 0x07;   // terminate/initiate NTP access
inline constexpr std::uint8_t kNtpResult   = 0x08;   // accessing / succeeded / failed
}  // namespace settingSub

// Read or write a 1A 05 SET-menu item. `item` is the DECIMAL menu number as
// printed in the guide (118, 119, ...); it is encoded as two BCD bytes.
[[nodiscard]] std::vector<std::uint8_t> cmdReadSetting(std::uint8_t to, int item);
[[nodiscard]] std::vector<std::uint8_t> cmdWriteSetting(std::uint8_t to, int item,
                                                        std::uint8_t value);
// SET-menu levels use the same two-byte 0000..0255 BCD payload as command 14,
// unlike the one-byte enums written by cmdWriteSetting().
[[nodiscard]] std::vector<std::uint8_t> cmdWriteSettingLevel(std::uint8_t to, int item,
                                                             int value);
[[nodiscard]] std::vector<std::uint8_t> cmdWriteSettingData(
    std::uint8_t to, int item, std::span<const std::uint8_t> value);

// IC-705 GPS and clock control. The position wire shape is documented by
// Icom's IC-705 CI-V Reference Guide pp. 21 and 25. It is intentionally kept
// in this transport-free codec so a future local-serial Icom path gets exactly
// the same validation as the RS-BA1 network path.
namespace gps {
inline constexpr std::uint8_t kPosition = 0x00;
inline constexpr std::uint8_t kSource   = 0x01;
inline constexpr std::uint8_t kManual   = 0x02;
}  // namespace gps

struct GpsPosition {
    double latitude = 0.0;
    double longitude = 0.0;
    std::optional<double> altitudeMetres;
    std::optional<int> courseDegrees;
    std::optional<double> speedKmh;
    std::optional<std::string> utcIso8601;
};

[[nodiscard]] std::optional<GpsPosition>
decodeGpsPosition(std::span<const std::uint8_t> data);
[[nodiscard]] std::vector<std::uint8_t> cmdReadGpsPosition(std::uint8_t to);
[[nodiscard]] std::vector<std::uint8_t> cmdReadGpsSource(std::uint8_t to);
[[nodiscard]] std::vector<std::uint8_t> cmdNtpAccess(std::uint8_t to, bool initiate);
[[nodiscard]] std::vector<std::uint8_t> cmdReadNtpAccessResult(std::uint8_t to);

// Four zero-padded decimal IPv4 octets, each encoded in two BCD bytes, as used
// by the Icom SET-menu network registers. Invalid BCD and octets >255 fail.
[[nodiscard]] std::optional<std::array<std::uint8_t, 4>>
decodeNetworkAddress(std::span<const std::uint8_t> data);
[[nodiscard]] std::optional<std::array<std::uint8_t, 4>>
subnetMaskFromBcdPrefix(std::uint8_t raw);
[[nodiscard]] std::optional<std::string>
decodeNetworkName(std::span<const std::uint8_t> data);

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

namespace repeaterAccess {
inline constexpr std::uint8_t kFunction = 0x5D;
}  // namespace repeaterAccess

namespace repeaterTone {
inline constexpr std::uint8_t kTxCtcss = 0x00;
inline constexpr std::uint8_t kRxCtcss = 0x01;
inline constexpr std::uint8_t kDtcs    = 0x02;
}  // namespace repeaterTone

struct RepeaterToneRegister {
    int value = 0;
    bool txReverse = false;
    bool rxReverse = false;
};

namespace scope {
inline constexpr std::uint8_t kWaveData    = 0x00;
// BOTH of these must be ON before 0x27 0x00 produces anything. Enabling only
// kOnOff turns the scope on the radio's own screen and sends us nothing — the
// number-one "my panadapter is black" cause.
inline constexpr std::uint8_t kOnOff       = 0x10;
inline constexpr std::uint8_t kDataOutput  = 0x11;
inline constexpr std::uint8_t kMainSub     = 0x12;   // 00 MAIN / 01 SUB (IC-9700); fixed 00 elsewhere
inline constexpr std::uint8_t kSingleDual  = 0x13;   // IC-705: fixed 00
inline constexpr std::uint8_t kMode        = 0x14;   // 00 centre, 01 fixed, 02 scroll-c, 03 scroll-f
inline constexpr std::uint8_t kSpan        = 0x15;   // centre and scroll-c modes
inline constexpr std::uint8_t kEdgeNumber  = 0x16;   // fixed and scroll-f modes, 01..04
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

// Filter slot (1, 2 or 3) for a requested width; the radio has three fixed IF
// slots, so a request in Hz can only SNAP. Mode-dependent: FIL1 is 3.0 kHz in
// SSB, 1.2 kHz in CW, 9 kHz in AM, 15 kHz in FM. Ladders are the IC-705 defaults
// (hamlib rigs/icom/ic7300.c, RIG_MODEL_IC705); the operator can redefine them
// and there is no way to read that back, so the UI passband must come from what
// the radio REPORTS after the change, never from the width requested.
[[nodiscard]] int filterForWidthHz(const std::string& mode, int widthHz) noexcept;

// The widths that mode's filter slots hold, NARROWEST FIRST — deliberately the
// reverse of the radio's own FIL1/FIL2/FIL3 numbering, in which FIL1 is the
// widest. Published as RadioCapabilities::rxFilterWidthsHz so the filter buttons
// offer what the hardware actually has instead of a Flex-shaped eight, and in
// the same narrow-to-wide order as every other filter row in the app.
[[nodiscard]] std::vector<int> filterWidthsForMode(const std::string& mode);

struct FilterPresetState {
    int id = 0;
    int widthHz = 0;

    bool operator==(const FilterPresetState&) const = default;
};

// FIL1/FIL2/FIL3 in radio order. The selected slot may carry a custom width;
// replacing its content must never reorder or rename the slot.
[[nodiscard]] std::vector<FilterPresetState> filterPresetsForMode(
    const std::string& mode, int selectedPresetId = 0, int selectedWidthHz = 0);

// The passband that filter gives in that mode, in Hz relative to the carrier,
// sign carrying the sideband (SliceModel's convention). The backend needs this
// because an IC-705's IF filters cannot be read back as Hz — nothing else in
// the chain can fill the window in.
[[nodiscard]] std::pair<int, int> passbandForModeAndFilter(const std::string& mode,
                                                          int filter);

// Complete wire plan for recalling a FIL button. The select command comes
// first, followed (where the mode permits it) by the slot's factory width and
// centred Twin-PBT writes. Keeping this Qt- and transport-free lets the exact
// operator command sequence be mutation-tested without a fake radio socket.
struct FilterPresetRecallPlan {
    std::vector<std::vector<std::uint8_t>> commands;
    int widthHz = 0;
    int lowHz = 0;
    int highHz = 0;
    int pbtCode = 128;
};
[[nodiscard]] std::optional<FilterPresetRecallPlan> filterPresetRecallPlan(
    std::uint8_t to, const std::string& ladderMode, CivMode wireMode,
    bool dataMode, int presetId, bool useVfoMode);

// IF filter WIDTH (1A 03). Three distinct things, never conflate them:
//   * SLOT      — FIL1/2/3, chosen with 0x26 (or 0x06).
//   * WIDTH     — 1A 03, the Hz the selected slot is currently defined as
//                 (operator-redefinable, per mode, remembered by the radio).
//   * PBT SHIFT — 14 07 / 14 08, position and inside cut of that width.
// Width codes (identical on IC-705, IC-7300MK2 and current Icoms):
//   SSB / CW        00..09 -> 50..500 Hz (50 Hz),  10..40 -> 600..3600 Hz (100 Hz)
//   RTTY            00..09 -> 50..500 Hz (50 Hz),  10..31 -> 600..2700 Hz (100 Hz)
//   AM              00..49 -> 200..10000 Hz (200 Hz)
//   FM / DV / WFM   no settable width; 1A 03 does not apply
// There is no 550 Hz code (wfview's `code <= 10` branch gets this wrong), so the
// tables come from ONE function and the encoder searches it instead of inverting.

// Which of the three code tables a mode uses. Fixed means the mode has no
// settable width at all.
enum class WidthClass : std::uint8_t { Ssb, Rtty, Am, Fixed };
[[nodiscard]] WidthClass widthClassFor(const std::string& mode) noexcept;

// The Hz that width code means in that mode, or nullopt when the code is
// outside the mode's table (or the mode has no settable width). VALIDATES
// rather than clamps: these bytes arrive from the network, and a code the
// table does not define is a frame we have mis-parsed, not a narrow filter.
[[nodiscard]] std::optional<int> filterWidthHzFromCode(const std::string& mode,
                                                        std::uint8_t code) noexcept;

// The code whose width is NEAREST to `hz`. Returns nullopt for a mode with no
// settable width. Searches the decode table, so encode and decode cannot drift.
[[nodiscard]] std::optional<std::uint8_t> filterWidthCodeFor(const std::string& mode,
                                                              int hz) noexcept;

// The narrowest and widest this mode can reach, and the step at the wide end.
// {0, 0, 0} means the mode has no settable width — the caller must not offer a
// resize control for it.
struct FilterWidthLimits {
    int minHz = 0;
    int maxHz = 0;
    int coarseStepHz = 0;   // the step above 500 Hz; below that it is 50 Hz
};
[[nodiscard]] FilterWidthLimits filterWidthLimitsFor(const std::string& mode) noexcept;

[[nodiscard]] std::vector<std::uint8_t> cmdReadFilterWidth(std::uint8_t to);
[[nodiscard]] std::vector<std::uint8_t> cmdSetFilterWidth(std::uint8_t to, std::uint8_t code);

// ---------------------------------------------------------------------------
// Twin PBT (14 07 / 14 08)
// ---------------------------------------------------------------------------

inline constexpr int kPbtCentreCode = 128;
// Codes either side of centre. 0000..0255 with 0128 centred is 128 below and
// 127 above, so the usable symmetric span is 127 — using 128 would make the
// two directions disagree by one step at full deflection.
inline constexpr int kPbtSpanCodes  = 127;

// How many Hz that PBT code is worth. SCALED BY THE WIDTH IN CIRCUIT: full
// deflection moves the passband by one whole width, so the same code means
// 3.6 kHz in wide SSB and 250 Hz in narrow CW. A converter that assumed a
// fixed Hz-per-step would be right in exactly one filter.
[[nodiscard]] int pbtShiftHz(int code, int widthHz) noexcept;

// The inverse, clamped to 0..255. Returns kPbtCentreCode for a zero or
// unusable width rather than dividing by it.
[[nodiscard]] int pbtCodeForShiftHz(int shiftHz, int widthHz) noexcept;

// What the operator actually hears, given the width in circuit and both PBT
// positions. Moving the pair TOGETHER slides the passband; moving them APART
// narrows it from the inside — which is the only way an Icom produces an
// asymmetric response, and the reason width alone can never describe one.
//
// `centreHz` is where this mode puts the passband centre relative to the
// carrier, signed in SliceModel's convention (see passbandCentreHz).
struct PassbandEdges {
    int lowHz = 0;
    int highHz = 0;
};
[[nodiscard]] PassbandEdges passbandFromWidthAndPbt(int centreHz, int widthHz,
                                                     int innerCode, int outerCode) noexcept;

// Signed CENTRE of this mode's passband relative to the carrier — not the low
// edge: Icom SSB filters narrow and widen symmetrically about a fixed centre
// (cf. "Filter Center" vs "Carrier Point", 1A 05 01 39 on the IC-7300MK2).
// SSB centre is 1500 Hz, as wfview uses for every Icom (receiverwidget.cpp).
// CW is 0 because the slice frequency in CW already is the pitch. RTTY keeps a
// 150 Hz carrier-side edge (needs `widthHz`); the mark frequency (SET 0050) is
// configurable and not modeled yet.
[[nodiscard]] int passbandCentreHz(const std::string& mode, int widthHz) noexcept;

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
[[nodiscard]] std::vector<std::uint8_t> cmdSetTransmitFrequencyCheck(std::uint8_t to,
                                                                     bool on);
[[nodiscard]] std::vector<std::uint8_t> cmdReadTransmitFrequencyCheck(std::uint8_t to);
// Attenuator. `db` is the dB figure the radio prints (0 or 20 on an
// IC-705), encoded as one BCD byte. The read form carries no payload.
[[nodiscard]] std::vector<std::uint8_t> cmdSetAttenuator(std::uint8_t to, int db);
[[nodiscard]] std::vector<std::uint8_t> cmdReadAttenuator(std::uint8_t to);
[[nodiscard]] std::vector<std::uint8_t> cmdReadRxAntenna(std::uint8_t to);
[[nodiscard]] std::vector<std::uint8_t> cmdSetRxAntenna(std::uint8_t to, bool rxAntenna);
enum class RepeaterOffsetDirection : std::uint8_t {
    Simplex = 0x10,
    Down = 0x11,
    Up = 0x12,
};
[[nodiscard]] std::vector<std::uint8_t> cmdReadRepeaterOffsetDirection(std::uint8_t to);
[[nodiscard]] std::vector<std::uint8_t> cmdSetRepeaterOffsetDirection(
    std::uint8_t to, RepeaterOffsetDirection direction);
[[nodiscard]] std::optional<RepeaterOffsetDirection> decodeRepeaterOffsetDirection(
    std::span<const std::uint8_t> payload);
[[nodiscard]] std::vector<std::uint8_t> cmdReadRepeaterOffset(std::uint8_t to);
[[nodiscard]] std::vector<std::uint8_t> cmdSetRepeaterOffset(std::uint8_t to,
                                                             int offsetHz);
[[nodiscard]] std::optional<int> decodeRepeaterOffsetHz(
    std::span<const std::uint8_t> payload);
[[nodiscard]] std::vector<std::uint8_t> cmdReadRepeaterTone(std::uint8_t to);
[[nodiscard]] std::vector<std::uint8_t> cmdSetRepeaterTone(std::uint8_t to,
                                                           double toneHz);
[[nodiscard]] std::optional<double> decodeRepeaterToneHz(
    std::span<const std::uint8_t> payload);
[[nodiscard]] std::vector<std::uint8_t> cmdReadRepeaterAccess(std::uint8_t to);
[[nodiscard]] std::optional<std::uint8_t> decodeRepeaterAccess(
    std::span<const std::uint8_t> payload);
// Normalized radio-state token for each documented IC-9700 16 5D value.
// Empty means reserved/unknown. Kept here with the wire decoder so socket-free
// protocol tests can pin every value without a fake radio session.
[[nodiscard]] std::string_view repeaterAccessModeName(std::uint8_t value) noexcept;
[[nodiscard]] std::optional<std::uint8_t> repeaterAccessModeValue(
    std::string_view name) noexcept;
[[nodiscard]] std::vector<std::uint8_t> cmdReadRepeaterToneRegister(
    std::uint8_t to, std::uint8_t which);
[[nodiscard]] std::vector<std::uint8_t> cmdSetRepeaterToneRegister(
    std::uint8_t to, std::uint8_t which, int value,
    bool txReverse = false, bool rxReverse = false);
[[nodiscard]] std::optional<std::vector<std::uint8_t>>
repeaterToneConfirmationForWrite(std::uint8_t to, const CivFrame& write);
[[nodiscard]] std::optional<RepeaterToneRegister> decodeRepeaterToneRegister(
    std::span<const std::uint8_t> payload);
[[nodiscard]] std::vector<std::uint8_t> cmdReadTransmitFrequency(std::uint8_t to);
[[nodiscard]] std::vector<std::uint8_t> cmdSetCtcssTone(std::uint8_t to,
                                                        std::uint8_t which,
                                                        double toneHz);
[[nodiscard]] std::vector<std::uint8_t> cmdSetDtcsTone(
    std::uint8_t to, int code, bool txReverse, bool rxReverse);
[[nodiscard]] std::vector<std::uint8_t> cmdSetRepeaterAccess(std::uint8_t to,
                                                             std::uint8_t mode);
// RIT / dTX read forms, and the antenna tuner. `21 xx` with no payload asks;
// `1C 01` with no payload asks whether the tuner is on, off or mid-cycle.
[[nodiscard]] std::vector<std::uint8_t> cmdReadTuneOffset(std::uint8_t to, std::uint8_t sub);
[[nodiscard]] std::vector<std::uint8_t> cmdSetTuner(std::uint8_t to, std::uint8_t value);
[[nodiscard]] std::vector<std::uint8_t> cmdReadTuner(std::uint8_t to);
[[nodiscard]] std::vector<std::uint8_t> cmdReadId(std::uint8_t to);
[[nodiscard]] std::vector<std::uint8_t> cmdScopeOnOff(std::uint8_t to, bool on);
[[nodiscard]] std::vector<std::uint8_t> cmdScopeDataOutput(std::uint8_t to, bool on);
// 0x27 0x14 / 0x16: `mode` is 00 centre, 01 fixed, 02 SCROLL-C, 03 SCROLL-F.
// `selector` is the scope these address: always 00 on single-scope radios,
// 00 MAIN / 01 SUB on the IC-9700 (guide ed. 4 p. 25). Which modes a radio
// accepts depends on firmware; the caller gates on the profile and on FA.
[[nodiscard]] std::vector<std::uint8_t> cmdScopeMode(std::uint8_t to, std::uint8_t mode,
                                                      std::uint8_t selector = 0x00);
[[nodiscard]] std::vector<std::uint8_t> cmdScopeEdgeNumber(std::uint8_t to, int edge,
                                                            std::uint8_t selector = 0x00);
[[nodiscard]] std::vector<std::uint8_t> cmdReadScopeEdgeNumber(std::uint8_t to,
                                                                std::uint8_t selector = 0x00);
// 0x27 0x1E: range number, edge number, lower and upper edge (5-byte LE BCD).
// No selector byte on any model. This writes the stored Fixed Edges preset the
// operator edits on the radio, so write only an owned slot, and throttle it.
[[nodiscard]] std::vector<std::uint8_t> cmdScopeFixedEdge(std::uint8_t to, int rangeNumber,
                                                           int edge, std::uint64_t lowerHz,
                                                           std::uint64_t upperHz);

// One Fixed Edges range: the edge preset is addressed by (range, edge number),
// and a window may not cross the range's bounds (the radio answers FA).
struct ScopeEdgeRange {
    int number = 0;              // the wire value
    std::uint64_t lowerHz = 0;   // settable lower bound, inclusive
    std::uint64_t upperHz = 0;   // settable upper bound, inclusive
};
// IC-7300MK2 / IC-7300 / IC-705 share ranges 01-13 (MK2 guide p. 26).
inline constexpr std::array<ScopeEdgeRange, 13> kScopeEdgeRangesHf{{
    {1,         30'000,  1'600'000},
    {2,      1'600'000,  2'000'000},
    {3,      2'000'000,  6'000'000},
    {4,      6'000'000,  8'000'000},
    {5,      8'000'000, 11'000'000},
    {6,     11'000'000, 15'000'000},
    {7,     15'000'000, 20'000'000},
    {8,     20'000'000, 22'000'000},
    {9,     22'000'000, 26'000'000},
    {10,    26'000'000, 30'000'000},
    {11,    30'000'000, 45'000'000},
    {12,    45'000'000, 60'000'000},
    {13,    60'000'000, 74'800'000},
}};
// IC-705 adds 14-17 (guide ed. 6 p. 29); there is no range over 200-400 MHz.
inline constexpr std::array<ScopeEdgeRange, 17> kScopeEdgeRangesIc705{{
    {1,         30'000,  1'600'000},
    {2,      1'600'000,  2'000'000},
    {3,      2'000'000,  6'000'000},
    {4,      6'000'000,  8'000'000},
    {5,      8'000'000, 11'000'000},
    {6,     11'000'000, 15'000'000},
    {7,     15'000'000, 20'000'000},
    {8,     20'000'000, 22'000'000},
    {9,     22'000'000, 26'000'000},
    {10,    26'000'000, 30'000'000},
    {11,    30'000'000, 45'000'000},
    {12,    45'000'000, 60'000'000},
    {13,    60'000'000, 74'800'000},
    {14,    74'800'000, 108'000'000},
    {15,   108'000'000, 137'000'000},
    {16,   137'000'000, 200'000'000},
    {17,   400'000'000, 470'000'000},
}};
// IC-9700: one range per band (guide ed. 4 p. 26).
inline constexpr std::array<ScopeEdgeRange, 3> kScopeEdgeRangesIc9700{{
    {1,   144'000'000,   148'000'000},
    {2,   430'000'000,   450'000'000},
    {3, 1'240'000'000, 1'300'000'000},
}};
// "Set the upper Edge frequency within 1 MHz of the lower frequency" (MK2
// Advanced Manual, Fixed Edges). Icom publishes no minimum; 5 kHz is the
// narrowest Center span.
inline constexpr std::uint64_t kScopeFixedEdgeMaxWidthHz = 1'000'000;
inline constexpr std::uint64_t kScopeFixedEdgeMinWidthHz = 5'000;

// The range `hz` falls in, or nullopt. A shared boundary resolves upward to the
// range it starts, except at the top of the last range.
[[nodiscard]] std::optional<ScopeEdgeRange> scopeEdgeRangeFor(std::span<const ScopeEdgeRange> ranges,
                                                              std::uint64_t hz) noexcept;

// A requested window made legal for `range`: width clamped to
// [kScopeFixedEdgeMinWidthHz, min(1 MHz, range width)], edges on whole kHz
// (the radio ignores sub-kHz digits), then shifted, not shrunk, into the range.
struct ScopeEdgeWindow {
    std::uint64_t lowerHz = 0;
    std::uint64_t upperHz = 0;
};
[[nodiscard]] ScopeEdgeWindow clampScopeEdgeWindow(const ScopeEdgeRange& range,
                                                   double centreHz,
                                                   double widthHz) noexcept;
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

// The span one detent away from `spanHz` (-1 narrower, +1 wider), clamped at the
// table ends. Needed because the spans step by ratios of 2 and 2.5 while zoom
// scales by 1.5, so nearest-snapping a zoom-out always returns the starting span.
[[nodiscard]] int adjacentScopeSpanHz(int spanHz, int direction) noexcept;

}  // namespace AetherSDR::icom
