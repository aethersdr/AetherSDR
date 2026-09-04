#include "core/backends/icom/CivCodec.h"
#include "core/DtcsCodes.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace AetherSDR::icom {
namespace {

std::string upper(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> buildFrame(std::uint8_t to, std::uint8_t cmd,
                                     std::span<const std::uint8_t> payload)
{
    std::vector<std::uint8_t> f;
    f.reserve(6 + payload.size());
    f.push_back(kCivPreamble);
    f.push_back(kCivPreamble);
    f.push_back(to);
    f.push_back(kControllerAddress);
    f.push_back(cmd);
    f.insert(f.end(), payload.begin(), payload.end());
    f.push_back(kCivEom);
    return f;
}

std::vector<std::uint8_t> buildFrameSub(std::uint8_t to, std::uint8_t cmd, std::uint8_t sub,
                                        std::span<const std::uint8_t> payload)
{
    std::vector<std::uint8_t> body;
    body.reserve(1 + payload.size());
    body.push_back(sub);
    body.insert(body.end(), payload.begin(), payload.end());
    return buildFrame(to, cmd, body);
}

// Which commands carry a subcommand is a per-command fact, not a positional
// one. Treating every second byte as a subcommand would turn command 0x05's
// first frequency digit into a "subcommand"; treating none of them as one
// would collapse every 0x27 scope reply into a single undifferentiated blob.
// So the set is enumerated — ONCE, here, because a second copy of it can drift
// silently and a drift produces exactly the wrong-but-plausible decode this
// enumeration exists to prevent. parseFrame() and the CI-V trace both read it.
bool commandHasSubcommand(std::uint8_t command)
{
    switch (command) {
    case cmd::kLevel:
    case cmd::kMeter:
    case cmd::kFunction:
    case cmd::kPower:
    case cmd::kReadId:
    case cmd::kSetting:
    case cmd::kTone:
    case cmd::kControl:
    case cmd::kGps:
    case cmd::kScope:
    // 0x21 WAS MISSING, and it is sub-addressed like the rest: 21 00 is the
    // offset, 21 01 the RIT enable, 21 02 the dTX enable. Without it every RIT
    // reply parsed with the subcommand sitting in the payload, so the decode
    // could not tell an enable from an offset and dropped all three.
    case cmd::kTuneOffset:
    case cmd::kRxAntenna:
    // 0x26 is sub-addressed by VFO: 26 00 is the selected VFO, 26 01 the
    // unselected one. Without this the selector stays in the payload and every
    // mode/DATA/filter field is decoded one byte off.
    case cmd::kVfoMode:
        return true;
    default:
        return false;
    }
}

std::optional<CivFrame> parseFrame(std::span<const std::uint8_t> frame)
{
    // FE FE <to> <from> <cmd> ... <FD>  — the shortest legal frame is 6 bytes
    // (an FB/FA acknowledgement with no payload).
    if (frame.size() < 6)
        return std::nullopt;
    if (frame[0] != kCivPreamble || frame[1] != kCivPreamble)
        return std::nullopt;
    const std::uint8_t terminator = frame.back();
    if (terminator != kCivEom && terminator != 0xFC)
        return std::nullopt;

    CivFrame f;
    f.to   = frame[2];
    f.from = frame[3];
    f.cmd  = frame[4];

    const std::size_t bodyBegin = 5;
    const std::size_t bodyEnd   = frame.size() - 1;   // exclusive of the terminator
    if (bodyEnd <= bodyBegin)
        return f;   // bare command or FB/FA with no data

    // See commandHasSubcommand() above for why this is an enumeration and not a
    // positional rule.
    if (commandHasSubcommand(f.cmd)) {
        f.hasSub = true;
        f.sub    = frame[bodyBegin];
        f.data.assign(frame.begin() + bodyBegin + 1, frame.begin() + bodyEnd);
    } else {
        f.data.assign(frame.begin() + bodyBegin, frame.begin() + bodyEnd);
    }
    return f;
}

std::vector<std::vector<std::uint8_t>> CivReassembler::feed(std::span<const std::uint8_t> bytes)
{
    std::vector<std::vector<std::uint8_t>> frames;

    for (std::uint8_t b : bytes) {
        if (!m_started) {
            // Rule 1: sync on the DOUBLE preamble. A lone 0xFE appears inside
            // BCD payloads, and accepting one leaves this decoder permanently
            // one frame behind, emitting garbage that happens to parse.
            if (b == kCivPreamble) {
                if (m_sawOne) {
                    m_started = true;
                    m_sawOne  = false;
                    m_buf.clear();
                    m_buf.push_back(kCivPreamble);
                    m_buf.push_back(kCivPreamble);
                } else {
                    m_sawOne = true;
                }
            } else {
                m_sawOne = false;
            }
            continue;
        }

        m_buf.push_back(b);

        // Rule 2: 0xFD is the documented terminator; 0xFC also ends a frame in
        // some replies. Both reference implementations accept either.
        if (b == kCivEom || b == 0xFC) {
            frames.push_back(m_buf);
            m_buf.clear();
            m_started = false;
            continue;
        }

        if (m_buf.size() >= kMaxFrameBytes) {
            // Past the cap this is a resync artefact, not a frame. Dropping it
            // silently is correct: the alternative is emitting 80 bytes of
            // arbitrary data to a decoder that will act on it.
            m_buf.clear();
            m_started = false;
        }
    }
    return frames;
}

void CivReassembler::timeout() noexcept
{
    // Rule 3. Without this one truncated frame swallows every subsequent byte
    // forever — the stream keeps flowing and the radio appears to have stopped
    // answering, which is a maddening symptom to chase.
    m_buf.clear();
    m_started = false;
    m_sawOne  = false;
}

void CivReassembler::reset() noexcept { timeout(); }

// ---------------------------------------------------------------------------
// BCD
// ---------------------------------------------------------------------------

std::uint8_t encodeBcdByte(int value)
{
    const int v = std::clamp(value, 0, 99);
    return static_cast<std::uint8_t>(((v / 10) << 4) | (v % 10));
}

int decodeBcdByte(std::uint8_t b)
{
    return ((b >> 4) & 0x0f) * 10 + (b & 0x0f);
}

std::vector<std::uint8_t> encodeFreq(std::uint64_t hz, std::size_t bytes)
{
    // LITTLE-endian digit pairs: the 1 Hz / 10 Hz pair goes FIRST.
    //   14.195000 MHz -> 00 50 19 14 00
    std::vector<std::uint8_t> out(bytes, 0);
    std::uint64_t v = hz;
    for (std::size_t i = 0; i < bytes; ++i) {
        const int lo = static_cast<int>(v % 10);
        v /= 10;
        const int hi = static_cast<int>(v % 10);
        v /= 10;
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return out;
}

std::optional<std::uint64_t> decodeFreq(std::span<const std::uint8_t> bcd)
{
    if (bcd.empty() || bcd.size() > 8)
        return std::nullopt;
    std::uint64_t hz = 0;
    std::uint64_t scale = 1;
    for (std::size_t i = 0; i < bcd.size(); ++i) {
        const int lo = bcd[i] & 0x0f;
        const int hi = (bcd[i] >> 4) & 0x0f;
        // A nibble above 9 is not BCD. Rejecting rather than clamping matters:
        // a corrupt frequency that decodes to *something* retunes the radio,
        // and on transmit that is an out-of-band emission.
        if (lo > 9 || hi > 9)
            return std::nullopt;
        hz += static_cast<std::uint64_t>(lo) * scale;
        scale *= 10;
        hz += static_cast<std::uint64_t>(hi) * scale;
        scale *= 10;
    }
    return hz;
}

std::optional<std::uint64_t> decodeFreqExact(
    std::span<const std::uint8_t> bcd, std::size_t expectedBytes)
{
    if (bcd.size() != expectedBytes) {
        return std::nullopt;
    }
    return decodeFreq(bcd);
}

std::optional<std::int64_t> decodeFreqSigned(std::span<const std::uint8_t> bcd)
{
    if (bcd.empty() || bcd.size() > 8)
        return std::nullopt;

    // The sign lives in the HIGH nibble of the LAST byte (the 1 GHz digit).
    // 0xF means negative; 0x0 means positive. Anything else is not BCD.
    std::vector<std::uint8_t> magnitude(bcd.begin(), bcd.end());
    const std::uint8_t topNibble = (magnitude.back() >> 4) & 0x0f;
    bool negative = false;
    if (topNibble == 0x0f) {
        negative = true;
        magnitude.back() &= 0x0f;   // strip the flag before decoding the digits
    }

    auto value = decodeFreq(magnitude);
    if (!value)
        return std::nullopt;
    const auto signedValue = static_cast<std::int64_t>(*value);
    return negative ? -signedValue : signedValue;
}

std::array<std::uint8_t, 2> encodeLevel(int value)
{
    const int v = std::clamp(value, 0, 9999);
    return {static_cast<std::uint8_t>((((v / 1000) % 10) << 4) | ((v / 100) % 10)),
            static_cast<std::uint8_t>((((v / 10) % 10) << 4) | (v % 10))};
}

std::optional<int> decodeLevel(std::span<const std::uint8_t> bcd)
{
    if (bcd.size() < 2)
        return std::nullopt;
    const int t = (bcd[0] >> 4) & 0x0f;
    const int h = bcd[0] & 0x0f;
    const int d = (bcd[1] >> 4) & 0x0f;
    const int u = bcd[1] & 0x0f;
    if (t > 9 || h > 9 || d > 9 || u > 9)
        return std::nullopt;
    return t * 1000 + h * 100 + d * 10 + u;
}

int percentToLevelRaw(int percent)
{
    const int pct = std::clamp(percent, 0, 100);
    return (pct * 255 + 99) / 100; // ceil(pct * 255 / 100)
}

int levelRawToPercent(int raw)
{
    return std::clamp(raw, 0, 255) * 100 / 255;
}

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

std::optional<CivMode> modeFromNeutral(const std::string& neutral, bool& dataModeOut)
{
    const std::string u = upper(neutral);
    dataModeOut = false;

    if (u == "LSB")  return CivMode::Lsb;
    if (u == "USB")  return CivMode::Usb;
    if (u == "AM")   return CivMode::Am;
    if (u == "CW" || u == "CWU") return CivMode::Cw;
    if (u == "CWL")  return CivMode::CwR;
    if (u == "FM" || u == "NFM") return CivMode::Fm;
    if (u == "WFM" || u == "WBFM") return CivMode::Wfm;
    // The data modes are the SAME sideband with the DATA flag set — that flag
    // is what routes audio to the WLAN/USB modulator instead of the mic, so
    // sending plain USB for DIGU produces a radio that transmits from the
    // microphone while the operator is running FT8.
    if (u == "DIGU") { dataModeOut = true; return CivMode::Usb; }
    if (u == "DIGL") { dataModeOut = true; return CivMode::Lsb; }
    // DFM is FM with the same DATA flag, and it was missing here. Two costs,
    // and the second is the expensive one:
    //
    //   1. DFM fell through to nullopt, so setSliceMode() took the "no
    //      equivalent" path and re-asserted the radio's current mode. Selecting
    //      DFM in the UI simply reverted — indistinguishable from the control
    //      being ignored.
    //   2. Worse, a radio the operator had put into FM-D from the front panel
    //      reported back as plain FM (modeToNeutral was lossy the same way), so
    //      the next mode write cleared the DATA flag and transmit audio came
    //      from the MICROPHONE rather than the WLAN modulator — exactly the
    //      failure the DIGU/DIGL comment above describes, but for packet
    //      instead of FT8. A 2 m AX.25 frame keyed the radio and put room noise
    //      on the air.
    if (u == "DFM")  { dataModeOut = true; return CivMode::Fm; }
    if (u == "RTTY") return CivMode::Rtty;

    // DSB, SAM and DRM have no IC-705 equivalent. Returning nullopt rather than
    // substituting USB is deliberate: a slice that asked for SAM and silently
    // got USB is a mode indicator that lies about what is being demodulated.
    return std::nullopt;
}

std::string modeToNeutral(CivMode mode, bool dataMode)
{
    switch (mode) {
    case CivMode::Lsb:  return dataMode ? "DIGL" : "LSB";
    case CivMode::Usb:  return dataMode ? "DIGU" : "USB";
    case CivMode::Am:   return "AM";
    case CivMode::Cw:   return "CW";
    case CivMode::CwR:  return "CWL";
    // Symmetric with Lsb/Usb above: the DATA flag is what distinguishes FM-D
    // from FM, and returning plain "FM" for both made the round trip lossy. A
    // radio sitting in FM-D reported as FM, so the UI showed FM, and the next
    // mode write sent FM with the flag clear — silently taking the radio OUT of
    // data mode and back onto the microphone.
    case CivMode::Fm:   return dataMode ? "DFM" : "FM";
    case CivMode::Wfm:  return "WFM";
    // AetherSDR has no RTTY neutral mode. Mapping to the data mode on the
    // matching sideband is LOSSY IN NAME but correct in the two things that
    // actually drive behaviour — the sideband and the passband — so a decoder
    // reading this gets a usable stream. The mode label is the part that lies,
    // and it lies conservatively.
    case CivMode::Rtty:  return "DIGL";
    case CivMode::RttyR: return "DIGU";
    // D-STAR is a whole waveform, not a demodulator setting. There is nothing
    // honest to map it to, so the caller must handle the empty string.
    case CivMode::Dv:   return {};
    }
    return {};
}

namespace {

// The three filter slots per mode class, WIDEST FIRST — FIL1, FIL2, FIL3, the
// order the radio numbers them in. From the IC-705's own defaults, transcribed
// from hamlib rigs/icom/ic7300.c (RIG_MODEL_IC705).
//
// A mode class, not a mode: LSB and USB share one ladder, and so do the data
// modes that ride on SSB. DV and WFM have no selectable filters at all, so they
// are absent and fall through to the SSB ladder rather than being given three
// buttons that do nothing.
struct FilterLadder {
    int fil1;
    int fil2;
    int fil3;
};

std::string upperMode(const std::string& mode)
{
    std::string u = mode;
    for (char& c : u)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return u;
}

FilterLadder ladderFor(const std::string& mode)
{
    const std::string u = upperMode(mode);
    if (u == "CW" || u == "CWU" || u == "CWL")
        return {1200, 500, 250};
    if (u == "RTTY" || u == "RTTYR")
        return {2400, 500, 250};
    // SAM belongs here, and its absence was the visible bug: it fell through to
    // the SSB ladder, so a broadcast station in synchronous AM was received
    // through a 2.4 kHz filter and sounded like a bad SSB signal.
    if (u == "AM" || u == "SAM")
        return {9000, 6000, 3000};
    if (u == "FM" || u == "NFM" || u == "DFM")
        return {15000, 10000, 7000};
    if (u == "WFM")
        return {200000, 200000, 200000};   // one fixed filter; see filterWidthsForMode
    return {3000, 2400, 1800};             // SSB, DIGL/DIGU and everything else
}

// Is the mode single-sideband — i.e. does its passband sit entirely on one side
// of the carrier, or straddle it?
bool isSingleSideband(const std::string& mode)
{
    const std::string u = upperMode(mode);
    return u == "LSB" || u == "USB" || u == "DIGL" || u == "DIGU"
        || u == "RTTY" || u == "RTTYR";
}

bool isLowerSideband(const std::string& mode)
{
    const std::string u = upperMode(mode);
    return u == "LSB" || u == "DIGL" || u == "RTTY" || u == "RTTYR";
}

}  // namespace

int filterForWidthHz(const std::string& mode, int widthHz) noexcept
{
    // NEAREST WITHIN THIS MODE'S LADDER, not a fixed threshold ladder. The old
    // form compared against the SSB numbers whatever the mode, so in AM every
    // width was >= 2700 and landed on FIL1, and in CW every width was < 2100 and
    // landed on FIL3 — three buttons, one filter, in both directions.
    const FilterLadder l = ladderFor(mode);
    const int d1 = std::abs(widthHz - l.fil1);
    const int d2 = std::abs(widthHz - l.fil2);
    const int d3 = std::abs(widthHz - l.fil3);
    if (d1 <= d2 && d1 <= d3) return 1;
    if (d2 <= d3) return 2;
    return 3;
}

std::vector<int> filterWidthsForMode(const std::string& mode)
{
    const FilterLadder l = ladderFor(mode);
    // WFM has ONE filter. Publishing it three times would give the operator
    // three identical buttons, which reads as two of them being broken.
    if (l.fil1 == l.fil2 && l.fil2 == l.fil3)
        return {l.fil1};

    // NARROWEST FIRST, which is the OPPOSITE of the radio's own FIL numbering.
    //
    // FIL1 is the widest slot on an Icom, so returning the ladder in FIL order
    // put 3.0k / 2.4k / 1.8k on screen — reading wide-to-narrow, while every
    // other filter row in AetherSDR (filterPresetsFor) runs narrow-to-wide.
    // The buttons looked reversed because against the rest of the app they
    // were. The FIL mapping is unaffected: filterForWidthHz picks the nearest
    // width in this mode's ladder and does not care what order it is listed in.
    return {l.fil3, l.fil2, l.fil1};
}

std::pair<int, int> passbandForModeAndFilter(const std::string& mode, int filter)
{
    const FilterLadder l = ladderFor(mode);
    const int width = filter == 1 ? l.fil1 : filter == 2 ? l.fil2 : l.fil3;

    // CW is symmetric about the PITCH, not the carrier — the radio centres its
    // filter on the tone, and the slice frequency already is the tone.
    const std::string u = upperMode(mode);
    if (u == "CW" || u == "CWU" || u == "CWL")
        return {-width / 2, width / 2};

    if (!isSingleSideband(mode))
        return {-width / 2, width / 2};   // AM, SAM, FM, WFM straddle the carrier

    // A sideband passband starts just off the carrier rather than at it: the
    // radio's own low edge is 300 Hz in voice and 150 Hz in the data modes,
    // and reporting 0 there would draw the filter overlapping the carrier line.
    const int low = (u == "DIGL" || u == "DIGU" || u == "RTTY" || u == "RTTYR")
                        ? 150 : 300;
    const int high = low + width;
    return isLowerSideband(mode) ? std::pair<int, int>{-high, -low}
                                 : std::pair<int, int>{low, high};
}

// ---------------------------------------------------------------------------
// IF filter width (1A 03), Twin PBT (14 07 / 14 08)
// ---------------------------------------------------------------------------

WidthClass widthClassFor(const std::string& mode) noexcept
{
    const std::string u = upperMode(mode);
    // FM AND ITS RELATIVES HAVE NO SETTABLE WIDTH. 1A 03 simply does not apply
    // there — the three FM slots are fixed at 15/10/7 kHz in the radio, and
    // sending a width code in FM either does nothing or lands in whatever mode
    // the radio was in last. Offering a resize control for FM would be a slider
    // wired to nothing, which is exactly what this whole change is removing.
    if (u == "FM" || u == "NFM" || u == "DFM" || u == "WFM" || u == "DV" || u == "DSTAR")
        return WidthClass::Fixed;
    if (u == "AM" || u == "SAM")
        return WidthClass::Am;
    if (u == "RTTY" || u == "RTTYR")
        return WidthClass::Rtty;
    return WidthClass::Ssb;   // LSB/USB/DIGL/DIGU/CW/CWU/CWL
}

std::optional<int> filterWidthHzFromCode(const std::string& mode, std::uint8_t code) noexcept
{
    switch (widthClassFor(mode)) {
    case WidthClass::Fixed:
        return std::nullopt;
    case WidthClass::Am:
        if (code > 49)
            return std::nullopt;
        return 200 + static_cast<int>(code) * 200;
    case WidthClass::Rtty:
        if (code <= 9)
            return 50 + static_cast<int>(code) * 50;
        if (code <= 31)
            return 600 + (static_cast<int>(code) - 10) * 100;
        return std::nullopt;
    case WidthClass::Ssb:
        if (code <= 9)
            return 50 + static_cast<int>(code) * 50;
        if (code <= 40)
            return 600 + (static_cast<int>(code) - 10) * 100;
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::uint8_t> filterWidthCodeFor(const std::string& mode, int hz) noexcept
{
    if (widthClassFor(mode) == WidthClass::Fixed)
        return std::nullopt;
    // SEARCH THE DECODER, do not invert it. Two arithmetic branches that must
    // agree across a discontinuity (the 500 Hz -> 600 Hz gap) is precisely the
    // shape wfview got wrong, and the table is 50 entries. Nearest wins; a tie
    // takes the WIDER code, because the request that lands exactly between two
    // widths came from a drag and losing audio is more noticeable than keeping
    // 50 Hz of it.
    std::optional<std::uint8_t> best;
    int bestDelta = 0;
    for (int code = 0; code <= 49; ++code) {
        const auto w = filterWidthHzFromCode(mode, static_cast<std::uint8_t>(code));
        if (!w)
            continue;
        const int delta = std::abs(*w - hz);
        if (!best || delta <= bestDelta) {
            best = static_cast<std::uint8_t>(code);
            bestDelta = delta;
        }
    }
    return best;
}

FilterWidthLimits filterWidthLimitsFor(const std::string& mode) noexcept
{
    switch (widthClassFor(mode)) {
    case WidthClass::Fixed: return {0, 0, 0};
    case WidthClass::Am:    return {200, 10000, 200};
    case WidthClass::Rtty:  return {50, 2700, 100};
    case WidthClass::Ssb:   return {50, 3600, 100};
    }
    return {0, 0, 0};
}

std::vector<std::uint8_t> cmdReadFilterWidth(std::uint8_t to)
{
    return buildFrameSub(to, cmd::kSetting, settingSub::kFilterWidth);
}

std::vector<std::uint8_t> cmdSetFilterWidth(std::uint8_t to, std::uint8_t code)
{
    // ONE BCD BYTE carrying the code as its own decimal value: code 40 goes on
    // the wire as 0x40, not 0x28. Sending the binary value instead reaches a
    // different, valid width and the radio accepts it without complaint.
    const std::array<std::uint8_t, 1> body{encodeBcdByte(std::clamp<int>(code, 0, 49))};
    return buildFrameSub(to, cmd::kSetting, settingSub::kFilterWidth, body);
}

int pbtShiftHz(int code, int widthHz) noexcept
{
    if (widthHz <= 0)
        return 0;
    const int offset = std::clamp(code, 0, 255) - kPbtCentreCode;
    return static_cast<int>(std::lround(static_cast<double>(offset) * widthHz
                                        / static_cast<double>(kPbtSpanCodes)));
}

int pbtCodeForShiftHz(int shiftHz, int widthHz) noexcept
{
    if (widthHz <= 0)
        return kPbtCentreCode;
    const double steps = static_cast<double>(shiftHz) * kPbtSpanCodes
                       / static_cast<double>(widthHz);
    return std::clamp(kPbtCentreCode + static_cast<int>(std::lround(steps)), 0, 255);
}

PassbandEdges passbandFromWidthAndPbt(int centreHz, int widthHz, int innerCode,
                                       int outerCode) noexcept
{
    if (widthHz <= 0)
        return {centreHz, centreHz};
    const int inner = pbtShiftHz(innerCode, widthHz);
    const int outer = pbtShiftHz(outerCode, widthHz);
    // TOGETHER SLIDES, APART NARROWS. The mean is where the window ended up;
    // the separation is how much of it the two edges have eaten from inside.
    const int shift = (inner + outer) / 2;
    const int effective = std::max(0, widthHz - std::abs(inner - outer));
    const int centre = centreHz + shift;
    return {centre - effective / 2, centre + effective / 2};
}

int passbandCentreHz(const std::string& mode, int widthHz) noexcept
{
    const std::string u = upperMode(mode);
    // CW's slice frequency IS the tone (see passbandForModeAndFilter), so the
    // passband is centred on it and the offset here is zero.
    if (u == "CW" || u == "CWU" || u == "CWL")
        return 0;
    if (!isSingleSideband(mode))
        return 0;   // AM, SAM, FM, WFM straddle the carrier
    // The RTTY mark frequency is configurable (SET 0050: 1275/1615/2125 Hz)
    // and has not been read into this model. Preserve the previous conservative
    // geometry with its carrier-side edge at 150 Hz; 1000 Hz is not a valid
    // mark value and would present an invented passband as radio truth.
    const int centre = (u == "RTTY" || u == "RTTYR")
        ? 150 + std::max(0, widthHz) / 2
        : 1500;
    return isLowerSideband(mode) ? -centre : centre;
}

// ---------------------------------------------------------------------------
// Command builders
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> cmdSetFrequency(std::uint8_t to, std::uint64_t hz)
{
    const auto bcd = encodeFreq(hz);
    return buildFrame(to, cmd::kSetFreq, bcd);
}

std::vector<std::uint8_t> cmdReadFrequency(std::uint8_t to)
{
    return buildFrame(to, cmd::kReadFreq);
}

std::vector<std::uint8_t> cmdSetMode(std::uint8_t to, CivMode mode, int filter)
{
    const std::array<std::uint8_t, 2> body{static_cast<std::uint8_t>(mode),
                                           static_cast<std::uint8_t>(std::clamp(filter, 1, 3))};
    return buildFrame(to, cmd::kSetMode, body);
}

std::vector<std::uint8_t> cmdReadMode(std::uint8_t to)
{
    return buildFrame(to, cmd::kReadMode);
}

std::vector<std::uint8_t> cmdReadVfoMode(std::uint8_t to)
{
    return buildFrameSub(to, cmd::kVfoMode, vfoMode::kSelected);
}

std::vector<std::uint8_t> cmdSetVfoMode(std::uint8_t to, CivMode mode, bool dataMode,
                                        int filter)
{
    const std::array<std::uint8_t, 3> body{
        static_cast<std::uint8_t>(mode),
        static_cast<std::uint8_t>(dataMode ? 0x01 : 0x00),
        static_cast<std::uint8_t>(std::clamp(filter, 1, 3)),
    };
    return buildFrameSub(to, cmd::kVfoMode, vfoMode::kSelected, body);
}

std::optional<VfoModeState> decodeVfoMode(std::span<const std::uint8_t> payload)
{
    if (payload.size() != 3)
        return std::nullopt;
    // The DATA byte is a two-valued flag. Anything else is a frame we have
    // mis-parsed — a resync artefact, or a model whose payload is not this
    // shape — and guessing "on" from it would put the radio's modulation source
    // somewhere the operator did not ask for.
    if (payload[1] > 0x01)
        return std::nullopt;
    VfoModeState s;
    s.mode = static_cast<CivMode>(payload[0]);
    s.dataMode = payload[1] != 0;
    if (payload[2] >= 1 && payload[2] <= 3)
        s.filter = payload[2];
    return s;
}

std::vector<std::uint8_t> cmdSetLevel(std::uint8_t to, std::uint8_t which, int value)
{
    const auto bcd = encodeLevel(std::clamp(value, 0, 255));
    return buildFrameSub(to, cmd::kLevel, which, bcd);
}

std::vector<std::uint8_t> cmdSendCwMessage(std::uint8_t to, std::string_view ascii)
{
    const std::size_t count = std::min<std::size_t>(ascii.size(), 30);
    const std::span<const std::uint8_t> body{
        reinterpret_cast<const std::uint8_t*>(ascii.data()), count};
    return buildFrame(to, cmd::kCwMessage, body);
}

std::vector<std::uint8_t> cmdAbortCwMessage(std::uint8_t to)
{
    const std::array<std::uint8_t, 1> body{0xFF};
    return buildFrame(to, cmd::kCwMessage, body);
}

std::vector<std::uint8_t> cmdReadMeter(std::uint8_t to, std::uint8_t which)
{
    return buildFrameSub(to, cmd::kMeter, which);
}

std::vector<std::uint8_t> cmdSetFunction(std::uint8_t to, std::uint8_t which, int value)
{
    const std::array<std::uint8_t, 1> body{static_cast<std::uint8_t>(value)};
    return buildFrameSub(to, cmd::kFunction, which, body);
}

std::vector<std::uint8_t> cmdSetAttenuator(std::uint8_t to, int db)
{
    // BCD, not binary: 20 dB goes on the wire as 0x20. A plain cast would send
    // 0x14, which is not a value the radio's attenuator table has.
    const int v = std::clamp(db, 0, 99);
    const std::array<std::uint8_t, 1> body{
        static_cast<std::uint8_t>(((v / 10) << 4) | (v % 10))};
    return buildFrame(to, cmd::kAttenuator, body);
}

std::vector<std::uint8_t> cmdReadAttenuator(std::uint8_t to)
{
    return buildFrame(to, cmd::kAttenuator);
}

std::vector<std::uint8_t> cmdSetRxAntenna(std::uint8_t to, bool rxAntenna)
{
    const std::array<std::uint8_t, 1> body{
        static_cast<std::uint8_t>(rxAntenna ? 1 : 0)};
    return buildFrameSub(to, cmd::kRxAntenna, 0x00, body);
}

std::vector<std::uint8_t> cmdReadRepeaterOffsetDirection(std::uint8_t to)
{
    return buildFrame(to, cmd::kDuplex);
}

std::vector<std::uint8_t> cmdSetRepeaterOffsetDirection(
    std::uint8_t to, RepeaterOffsetDirection direction)
{
    const std::array<std::uint8_t, 1> body{static_cast<std::uint8_t>(direction)};
    return buildFrame(to, cmd::kDuplex, body);
}

std::optional<RepeaterOffsetDirection> decodeRepeaterOffsetDirection(
    std::span<const std::uint8_t> payload)
{
    if (payload.size() != 1) {
        return std::nullopt;
    }
    switch (payload.front()) {
    case 0x10: return RepeaterOffsetDirection::Simplex;
    case 0x11: return RepeaterOffsetDirection::Down;
    case 0x12: return RepeaterOffsetDirection::Up;
    default: return std::nullopt;
    }
}

std::vector<std::uint8_t> cmdReadRepeaterOffset(std::uint8_t to)
{
    return buildFrame(to, cmd::kReadRepeaterOffset);
}

std::vector<std::uint8_t> cmdSetRepeaterOffset(std::uint8_t to, int offsetHz)
{
    // Three little-endian BCD bytes in 100 Hz units.  600 kHz is 6000 units,
    // and therefore 00 60 00 on the wire — the same pair ordering as a
    // frequency, with two fewer bytes and coarser resolution.
    const int units = std::clamp(static_cast<int>(std::lround(offsetHz / 100.0)),
                                 0, 999999);
    const std::array<std::uint8_t, 3> body{
        encodeBcdByte(units % 100),
        encodeBcdByte((units / 100) % 100),
        encodeBcdByte((units / 10000) % 100),
    };
    return buildFrame(to, cmd::kSetRepeaterOffset, body);
}

std::optional<int> decodeRepeaterOffsetHz(std::span<const std::uint8_t> payload)
{
    if (payload.size() != 3) {
        return std::nullopt;
    }
    for (std::uint8_t byte : payload) {
        if ((byte & 0x0F) > 9 || ((byte >> 4) & 0x0F) > 9) {
            return std::nullopt;
        }
    }
    const int units = decodeBcdByte(payload[0])
        + decodeBcdByte(payload[1]) * 100
        + decodeBcdByte(payload[2]) * 10000;
    return units * 100;
}

std::vector<std::uint8_t> cmdReadRepeaterTone(std::uint8_t to)
{
    return cmdReadRepeaterToneRegister(to, repeaterTone::kTxCtcss);
}

std::vector<std::uint8_t> cmdSetRepeaterTone(std::uint8_t to, double toneHz)
{
    return cmdSetCtcssTone(to, repeaterTone::kTxCtcss, toneHz);
}

std::vector<std::uint8_t> cmdSetCtcssTone(std::uint8_t to, std::uint8_t which,
                                          double toneHz)
{
    // The guide fixes the first two digits at zero and allows 000.0..299.9 Hz.
    // Carry tenths of a hertz as six big-endian BCD digits: 88.5 -> 00 08 85.
    const int tenths = std::clamp(static_cast<int>(std::lround(toneHz * 10.0)),
                                  0, 2999);
    const std::array<std::uint8_t, 3> body{
        0x00,
        encodeBcdByte((tenths / 100) % 100),
        encodeBcdByte(tenths % 100),
    };
    return buildFrameSub(to, cmd::kTone, which, body);
}

std::vector<std::uint8_t> cmdSetDtcsTone(
    std::uint8_t to, int code, bool txReverse, bool rxReverse)
{
    if (!isCanonicalDtcsCode(code)) {
        return {};
    }
    const std::array<std::uint8_t, 3> body{
        static_cast<std::uint8_t>((txReverse ? 0x10 : 0x00)
                                  | (rxReverse ? 0x01 : 0x00)),
        encodeBcdByte(code / 100),
        encodeBcdByte(code % 100),
    };
    return buildFrameSub(to, cmd::kTone, repeaterTone::kDtcs, body);
}

std::vector<std::uint8_t> cmdReadRepeaterAccess(std::uint8_t to)
{
    return cmdReadFunction(to, func::kRepeaterAccess);
}

std::vector<std::uint8_t> cmdSetRepeaterAccess(std::uint8_t to, std::uint8_t mode)
{
    return cmdSetFunction(to, func::kRepeaterAccess, mode);
}

std::optional<double> decodeRepeaterToneHz(std::span<const std::uint8_t> payload)
{
    if (payload.size() != 3 || payload[0] != 0x00) {
        return std::nullopt;
    }
    for (std::uint8_t byte : payload.subspan(1)) {
        if ((byte & 0x0F) > 9 || ((byte >> 4) & 0x0F) > 9) {
            return std::nullopt;
        }
    }
    const int tenths = decodeBcdByte(payload[1]) * 100
        + decodeBcdByte(payload[2]);
    if (tenths > 2999) {
        return std::nullopt;
    }
    return static_cast<double>(tenths) / 10.0;
}

std::optional<std::uint8_t> decodeRepeaterAccess(
    std::span<const std::uint8_t> payload)
{
    if (payload.size() != 1) {
        return std::nullopt;
    }
    switch (payload[0]) {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x03:
    case 0x06:
    case 0x07:
    case 0x08:
    case 0x09:
        return payload[0];
    default:
        return std::nullopt;
    }
}

std::string_view repeaterAccessModeName(std::uint8_t value) noexcept
{
    switch (value) {
    case 0x00: return "off";
    case 0x01: return "ctcss_tx";
    case 0x02: return "ctcss_rx";
    case 0x03: return "dtcs_txrx";
    case 0x06: return "dtcs_tx";
    case 0x07: return "ctcss_tx_dtcs_rx";
    case 0x08: return "dtcs_tx_ctcss_rx";
    case 0x09: return "ctcss_txrx";
    default:   return {};
    }
}

std::optional<std::uint8_t> repeaterAccessModeValue(std::string_view name) noexcept
{
    constexpr std::array<std::uint8_t, 8> kValues{
        0x00, 0x01, 0x02, 0x03, 0x06, 0x07, 0x08, 0x09};
    for (const std::uint8_t value : kValues) {
        if (repeaterAccessModeName(value) == name) {
            return value;
        }
    }
    return std::nullopt;
}

std::vector<std::uint8_t> cmdReadRepeaterToneRegister(
    std::uint8_t to, std::uint8_t which)
{
    return buildFrameSub(to, cmd::kTone, which);
}

std::vector<std::uint8_t> cmdSetRepeaterToneRegister(
    std::uint8_t to, std::uint8_t which, int value,
    bool txReverse, bool rxReverse)
{
    const int bounded = std::clamp(value, 0, 9999);
    const std::array<std::uint8_t, 3> body{
        static_cast<std::uint8_t>((txReverse ? 0x10 : 0x00)
                                  | (rxReverse ? 0x01 : 0x00)),
        encodeBcdByte((bounded / 100) % 100),
        encodeBcdByte(bounded % 100),
    };
    return buildFrameSub(to, cmd::kTone, which, body);
}

std::optional<std::vector<std::uint8_t>> repeaterToneConfirmationForWrite(
    std::uint8_t to, const CivFrame& write)
{
    if (write.cmd != cmd::kTone || !write.hasSub || write.data.empty()
        || (write.sub != repeaterTone::kTxCtcss
            && write.sub != repeaterTone::kRxCtcss
            && write.sub != repeaterTone::kDtcs)) {
        return std::nullopt;
    }
    return cmdReadRepeaterToneRegister(to, write.sub);
}

std::optional<RepeaterToneRegister> decodeRepeaterToneRegister(
    std::span<const std::uint8_t> payload)
{
    const auto validBcd = [](std::uint8_t byte) {
        return (byte & 0x0F) <= 9 && ((byte >> 4) & 0x0F) <= 9;
    };
    if (payload.size() != 3 || (payload[0] & 0xEE) != 0
        || !validBcd(payload[1]) || !validBcd(payload[2])) {
        return std::nullopt;
    }
    return RepeaterToneRegister{
        decodeBcdByte(payload[1]) * 100 + decodeBcdByte(payload[2]),
        (payload[0] & 0x10) != 0,
        (payload[0] & 0x01) != 0,
    };
}

std::vector<std::uint8_t> cmdReadTransmitFrequency(std::uint8_t to)
{
    return buildFrameSub(to, cmd::kControl, control::kReadTxFreq);
}

std::vector<std::uint8_t> cmdReadTuneOffset(std::uint8_t to, std::uint8_t sub)
{
    return buildFrameSub(to, cmd::kTuneOffset, sub);
}

std::vector<std::uint8_t> cmdSetTuner(std::uint8_t to, std::uint8_t value)
{
    // 00 off, 01 on, 02 START A MATCHING CYCLE. The third is not a tune
    // carrier — see control::kTuner — and it is the only one that keys.
    const std::array<std::uint8_t, 1> body{value};
    return buildFrameSub(to, cmd::kControl, control::kTuner, body);
}

std::vector<std::uint8_t> cmdReadTuner(std::uint8_t to)
{
    return buildFrameSub(to, cmd::kControl, control::kTuner);
}

std::vector<std::uint8_t> cmdSetPtt(std::uint8_t to, bool transmit)
{
    const std::array<std::uint8_t, 1> body{static_cast<std::uint8_t>(transmit ? 0x01 : 0x00)};
    return buildFrameSub(to, cmd::kControl, control::kPtt, body);
}

std::vector<std::uint8_t> cmdSetTransmitFrequencyCheck(std::uint8_t to, bool on)
{
    const std::array<std::uint8_t, 1> body{static_cast<std::uint8_t>(on ? 0x01 : 0x00)};
    return buildFrameSub(to, cmd::kControl, control::kXfc, body);
}

std::vector<std::uint8_t> cmdReadTransmitFrequencyCheck(std::uint8_t to)
{
    return buildFrameSub(to, cmd::kControl, control::kXfc);
}

std::vector<std::uint8_t> cmdReadId(std::uint8_t to)
{
    return buildFrameSub(to, cmd::kReadId, 0x00);
}

std::vector<std::uint8_t> cmdScopeOnOff(std::uint8_t to, bool on)
{
    const std::array<std::uint8_t, 1> body{static_cast<std::uint8_t>(on ? 0x01 : 0x00)};
    return buildFrameSub(to, cmd::kScope, scope::kOnOff, body);
}

std::vector<std::uint8_t> cmdScopeDataOutput(std::uint8_t to, bool on)
{
    const std::array<std::uint8_t, 1> body{static_cast<std::uint8_t>(on ? 0x01 : 0x00)};
    return buildFrameSub(to, cmd::kScope, scope::kDataOutput, body);
}

std::vector<std::uint8_t> cmdScopeMode(std::uint8_t to, bool fixed)
{
    // 0000 = centre, 0001 = fixed. Two BCD bytes, not one.
    const auto bcd = encodeLevel(fixed ? 1 : 0);
    return buildFrameSub(to, cmd::kScope, scope::kMode, bcd);
}

int nearestScopeSpanHz(int requestedHz) noexcept
{
    int best = kScopeSpansHz.front();
    long bestErr = std::labs(static_cast<long>(requestedHz) - best);
    for (int s : kScopeSpansHz) {
        const long err = std::labs(static_cast<long>(requestedHz) - s);
        if (err < bestErr) {
            bestErr = err;
            best    = s;
        }
    }
    return best;
}

int adjacentScopeSpanHz(int spanHz, int direction) noexcept
{
    // Anchor on the nearest table entry first: the caller's "current" span comes
    // from a decoded sweep, and an off-by-a-few-Hz value must not fall through
    // to the front of the table.
    const int cur = nearestScopeSpanHz(spanHz);
    for (std::size_t i = 0; i < kScopeSpansHz.size(); ++i) {
        if (kScopeSpansHz[i] != cur)
            continue;
        if (direction < 0)
            return i == 0 ? kScopeSpansHz.front() : kScopeSpansHz[i - 1];
        return i + 1 >= kScopeSpansHz.size() ? kScopeSpansHz.back() : kScopeSpansHz[i + 1];
    }
    return cur;
}

std::vector<std::uint8_t> cmdScopeSpan(std::uint8_t to, int spanHz)
{
    // SIX data bytes: a LEADING FIXED 0x00, then the span as a 5-byte
    // little-endian BCD frequency.
    //
    // That leading byte is the whole command. Without it the radio simply
    // IGNORES the frame — no NG, no error, the scope just stays where it was —
    // which reads as "zoom does nothing" and sends you hunting through the UI.
    // Icom's own diagram for 27 15 numbers six boxes and labels the first two
    // digits "0 (Fixed)"; the same leading byte appears on 27 00 (where this
    // decoder already honours it when PARSING) and on 27 19. Building without
    // it while parsing with it is the asymmetry that hid this.
    std::vector<std::uint8_t> body;
    body.reserve(1 + kFreqBytes);
    body.push_back(0x00);
    const auto bcd = encodeFreq(static_cast<std::uint64_t>(nearestScopeSpanHz(spanHz)));
    body.insert(body.end(), bcd.begin(), bcd.end());
    return buildFrameSub(to, cmd::kScope, scope::kSpan, body);
}

namespace {
// The menu number as two BCD bytes, big-endian: item 119 -> 01 19.
std::array<std::uint8_t, 2> settingItemBcd(int item)
{
    const int v = std::clamp(item, 0, 9999);
    return {static_cast<std::uint8_t>((((v / 1000) % 10) << 4) | ((v / 100) % 10)),
            static_cast<std::uint8_t>((((v / 10) % 10) << 4) | (v % 10))};
}
}  // namespace

std::vector<std::uint8_t> cmdReadLevel(std::uint8_t to, std::uint8_t which)
{
    return buildFrameSub(to, cmd::kLevel, which);
}

std::vector<std::uint8_t> cmdReadFunction(std::uint8_t to, std::uint8_t which)
{
    return buildFrameSub(to, cmd::kFunction, which);
}

std::vector<std::uint8_t> cmdReadSetting(std::uint8_t to, int item)
{
    const auto bcd = settingItemBcd(item);
    return buildFrameSub(to, cmd::kSetting, 0x05, bcd);
}

std::vector<std::uint8_t> cmdWriteSetting(std::uint8_t to, int item, std::uint8_t value)
{
    return cmdWriteSettingData(to, item, std::span(&value, 1));
}

std::vector<std::uint8_t> cmdWriteSettingData(std::uint8_t to, int item,
                                               std::span<const std::uint8_t> value)
{
    const auto bcd = settingItemBcd(item);
    std::vector<std::uint8_t> body;
    body.reserve(2 + value.size());
    body.push_back(bcd[0]);
    body.push_back(bcd[1]);
    body.insert(body.end(), value.begin(), value.end());
    return buildFrameSub(to, cmd::kSetting, settingSub::kMenu, body);
}

namespace {

bool validBcd(std::uint8_t value)
{
    return ((value >> 4) & 0x0f) <= 9 && (value & 0x0f) <= 9;
}

bool allFf(std::span<const std::uint8_t> data)
{
    return !data.empty()
        && std::all_of(data.begin(), data.end(), [](std::uint8_t value) {
               return value == 0xff;
           });
}

bool allValidBcd(std::span<const std::uint8_t> data)
{
    return !data.empty()
        && std::all_of(data.begin(), data.end(), validBcd);
}

bool validDate(int year, int month, int day)
{
    static constexpr std::array<int, 12> kDaysPerMonth{
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1) {
        return false;
    }
    int days = kDaysPerMonth[static_cast<std::size_t>(month - 1)];
    const bool leapYear = year % 4 == 0;
    if (month == 2 && leapYear) {
        ++days;
    }
    return day <= days;
}

std::optional<int> decodeBcdSpan(std::span<const std::uint8_t> data)
{
    if (data.empty() || allFf(data)) {
        return std::nullopt;
    }
    int value = 0;
    for (std::uint8_t byte : data) {
        if (!validBcd(byte)) {
            return std::nullopt;
        }
        value = value * 100 + decodeBcdByte(byte);
    }
    return value;
}

}  // namespace

std::optional<GpsPosition> decodeGpsPosition(std::span<const std::uint8_t> data)
{
    constexpr std::size_t kPositionBytes = 27;
    if (data.size() != kPositionBytes || allFf(data)) {
        return std::nullopt;
    }

    // Latitude: DD MM mm m0 0H, H=0 south / 1 north.
    if (!validBcd(data[0]) || !validBcd(data[1]) || !validBcd(data[2])
        || (data[3] & 0x0f) != 0 || ((data[3] >> 4) & 0x0f) > 9
        || (data[4] & 0xf0) != 0 || (data[4] & 0x0f) > 1) {
        return std::nullopt;
    }
    const int latDegrees = decodeBcdByte(data[0]);
    const double latMinutes = decodeBcdByte(data[1])
        + decodeBcdByte(data[2]) / 100.0
        + ((data[3] >> 4) & 0x0f) / 1000.0;
    if (latDegrees > 90 || latMinutes >= 60.0
        || (latDegrees == 90 && latMinutes > 0.0)) {
        return std::nullopt;
    }

    // Longitude: 0D DD MM mm m0 0H, H=0 west / 1 east.
    //
    // Unlike latitude, the three degree digits straddle two bytes. A real
    // IC-705 reporting 118 deg 03.534 min sends `01 18 03 53 40 00`.
    // Treating the second byte's low nibble as the first minutes digit turns
    // that into the impossible 11 deg 95.390 min and rejects an otherwise
    // valid fix.
    for (std::size_t i = 5; i <= 9; ++i) {
        if (!validBcd(data[i])) {
            return std::nullopt;
        }
    }
    if ((data[5] & 0xf0) != 0 || (data[9] & 0x0f) != 0
        || (data[10] & 0xf0) != 0 || (data[10] & 0x0f) > 1) {
        return std::nullopt;
    }
    const int lonDegrees = (data[5] & 0x0f) * 100 + decodeBcdByte(data[6]);
    const double lonMinutes = decodeBcdByte(data[7])
        + decodeBcdByte(data[8]) / 100.0
        + ((data[9] >> 4) & 0x0f) / 1000.0;
    if (lonDegrees > 180 || lonMinutes >= 60.0
        || (lonDegrees == 180 && lonMinutes > 0.0)) {
        return std::nullopt;
    }

    GpsPosition out;
    out.latitude = latDegrees + latMinutes / 60.0;
    if ((data[4] & 0x0f) == 0) {
        out.latitude = -out.latitude;
    }
    out.longitude = lonDegrees + lonMinutes / 60.0;
    if ((data[10] & 0x0f) == 0) {
        out.longitude = -out.longitude;
    }

    const std::span altitude = data.subspan(11, 4);
    if (!allFf(altitude)) {
        if (!validBcd(data[11]) || !validBcd(data[12]) || !validBcd(data[13])
            || (data[14] & 0xf0) != 0 || (data[14] & 0x0f) > 1) {
            return std::nullopt;
        }
        double metres = (decodeBcdByte(data[11]) * 10000
                         + decodeBcdByte(data[12]) * 100
                         + decodeBcdByte(data[13])) / 10.0;
        if ((data[14] & 0x0f) != 0) {
            metres = -metres;
        }
        out.altitudeMetres = metres;
    }

    if (const auto course = decodeBcdSpan(data.subspan(15, 2)); course && *course <= 360) {
        out.courseDegrees = *course;
    }
    if (const auto speed = decodeBcdSpan(data.subspan(17, 3)); speed) {
        out.speedKmh = *speed / 10.0;
    }
    const std::span dateTime = data.subspan(20, 7);
    if (!allFf(dateTime) && allValidBcd(dateTime)) {
        const int year = decodeBcdByte(data[20]) * 100 + decodeBcdByte(data[21]);
        const int month = decodeBcdByte(data[22]);
        const int day = decodeBcdByte(data[23]);
        const int hour = decodeBcdByte(data[24]);
        const int minute = decodeBcdByte(data[25]);
        const int second = decodeBcdByte(data[26]);
        if (validDate(year, month, day)
            && hour <= 23 && minute <= 59 && second <= 60) {
            char iso[21]{};
            std::snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                          year, month, day, hour, minute, second);
            out.utcIso8601 = iso;
        }
    }
    return out;
}

std::vector<std::uint8_t> cmdReadGpsPosition(std::uint8_t to)
{
    return buildFrameSub(to, cmd::kGps, gps::kPosition);
}

std::vector<std::uint8_t> cmdReadGpsSource(std::uint8_t to)
{
    return buildFrameSub(to, cmd::kGps, gps::kSource);
}

std::vector<std::uint8_t> cmdNtpAccess(std::uint8_t to, bool initiate)
{
    const std::array<std::uint8_t, 1> body{
        static_cast<std::uint8_t>(initiate ? 0x01 : 0x00)};
    return buildFrameSub(to, cmd::kSetting, settingSub::kNtpAccess, body);
}

std::vector<std::uint8_t> cmdReadNtpAccessResult(std::uint8_t to)
{
    return buildFrameSub(to, cmd::kSetting, settingSub::kNtpResult);
}

std::vector<std::uint8_t> cmdWriteSettingLevel(std::uint8_t to, int item, int value)
{
    const auto itemBcd = settingItemBcd(item);
    const auto levelBcd = encodeLevel(std::clamp(value, 0, 255));
    const std::array<std::uint8_t, 4> body{
        itemBcd[0], itemBcd[1], levelBcd[0], levelBcd[1]};
    return buildFrameSub(to, cmd::kSetting, 0x05, body);
}

std::optional<std::array<std::uint8_t, 4>>
decodeNetworkAddress(std::span<const std::uint8_t> data)
{
    if (data.size() != 8) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 4> octets{};
    for (std::size_t i = 0; i < data.size(); i += 2) {
        const std::uint8_t high = data[i];
        const std::uint8_t low = data[i + 1];
        if ((high & 0x0f) > 9 || ((high >> 4) & 0x0f) > 9
            || (low & 0x0f) > 9 || ((low >> 4) & 0x0f) > 9) {
            return std::nullopt;
        }
        const int value = decodeBcdByte(high) * 100 + decodeBcdByte(low);
        if (value > 255) {
            return std::nullopt;
        }
        octets[i / 2] = static_cast<std::uint8_t>(value);
    }
    return octets;
}

std::optional<std::array<std::uint8_t, 4>>
subnetMaskFromBcdPrefix(std::uint8_t raw)
{
    if ((raw & 0x0f) > 9 || ((raw >> 4) & 0x0f) > 9) {
        return std::nullopt;
    }
    const int prefix = decodeBcdByte(raw);
    if (prefix < 1 || prefix > 30) {
        return std::nullopt;
    }
    const std::uint32_t mask = 0xffffffffU << (32 - prefix);
    return std::array<std::uint8_t, 4>{
        static_cast<std::uint8_t>((mask >> 24) & 0xffU),
        static_cast<std::uint8_t>((mask >> 16) & 0xffU),
        static_cast<std::uint8_t>((mask >> 8) & 0xffU),
        static_cast<std::uint8_t>(mask & 0xffU)};
}

std::optional<std::string> decodeNetworkName(std::span<const std::uint8_t> data)
{
    if (data.size() > 15) {
        return std::nullopt;
    }
    std::string name;
    name.reserve(data.size());
    for (std::uint8_t byte : data) {
        if (byte < 0x20 || byte > 0x7e) {
            return std::nullopt;
        }
        name.push_back(static_cast<char>(byte));
    }
    while (!name.empty() && name.back() == ' ') {
        name.pop_back();
    }
    return name;
}

std::vector<std::uint8_t> cmdTuneOffsetHz(std::uint8_t to, int hz)
{
    // +/- 9.99 kHz, and the radio takes a MAGNITUDE plus a separate sign byte.
    const int clamped   = std::clamp(hz, -9990, 9990);
    const int magnitude = std::abs(clamped);
    // Two BCD bytes, LITTLE-endian like a frequency: 1/10 Hz pair first.
    const std::array<std::uint8_t, 3> body{
        static_cast<std::uint8_t>((((magnitude / 10) % 10) << 4) | (magnitude % 10)),
        static_cast<std::uint8_t>((((magnitude / 1000) % 10) << 4) | ((magnitude / 100) % 10)),
        static_cast<std::uint8_t>(clamped < 0 ? 0x01 : 0x00),
    };
    return buildFrameSub(to, cmd::kTuneOffset, tuneOffset::kFrequency, body);
}

std::vector<std::uint8_t> cmdRitEnable(std::uint8_t to, bool on)
{
    const std::array<std::uint8_t, 1> body{static_cast<std::uint8_t>(on ? 0x01 : 0x00)};
    return buildFrameSub(to, cmd::kTuneOffset, tuneOffset::kRitOnOff, body);
}

std::vector<std::uint8_t> cmdXitEnable(std::uint8_t to, bool on)
{
    const std::array<std::uint8_t, 1> body{static_cast<std::uint8_t>(on ? 0x01 : 0x00)};
    return buildFrameSub(to, cmd::kTuneOffset, tuneOffset::kXitOnOff, body);
}

std::vector<std::uint8_t> cmdScopeReference(std::uint8_t to, double db)
{
    // -20.0 .. +20.0 dB in 0.5 dB steps. Magnitude is two BCD bytes holding
    // four digits (10 / 1 / 0.1 / 0.01, the last fixed at 0), and the SIGN is a
    // separate third byte — 0x00 plus, 0x01 minus. Encoding the sign into the
    // magnitude is the obvious mistake and produces a reference level 20 dB out.
    const double clamped = std::clamp(db, -20.0, 20.0);
    const bool negative  = clamped < 0.0;
    // Round to the nearest half-dB the radio can actually represent.
    const int halfSteps = static_cast<int>(std::lround(std::abs(clamped) * 2.0));
    const int tenths    = halfSteps * 5;            // e.g. 20.5 dB -> 205
    // FOUR bytes, and the first is the 0x27 family's leading fixed 0x00 — the
    // same one 27 15 needs. See cmdScopeSpan for what omitting it costs.
    const std::array<std::uint8_t, 4> body{
        0x00,
        static_cast<std::uint8_t>((((tenths / 100) % 10) << 4) | ((tenths / 10) % 10)),
        static_cast<std::uint8_t>(((tenths % 10) << 4) | 0),
        static_cast<std::uint8_t>(negative ? 0x01 : 0x00),
    };
    return buildFrameSub(to, cmd::kScope, scope::kReference, body);
}

}  // namespace AetherSDR::icom
