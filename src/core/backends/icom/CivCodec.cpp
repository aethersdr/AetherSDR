#include "core/backends/icom/CivCodec.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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
    case cmd::kControl:
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
    case CivMode::Cw:   return "CWU";
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
    const auto bcd = settingItemBcd(item);
    const std::array<std::uint8_t, 3> body{bcd[0], bcd[1], value};
    return buildFrameSub(to, cmd::kSetting, 0x05, body);
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
