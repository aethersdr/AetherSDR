#include "core/backends/icom/IcomScope.h"

#include <algorithm>

namespace AetherSDR::icom {
namespace {

// Offsets inside a 0x27 0x00 frame's data (i.e. after cmd + subcommand).
//
//   data[0]      0x00, fixed
//   data[1]      division index, BCD 01..11
//   data[2]      division maximum, BCD — 01 over WLAN, 11 over USB
// then, ONLY when the division index is 1:
//   data[3]      mode: 00 centre, 01 fixed
//   data[4..8]   frequency A — centre (centre mode) or lower edge (fixed)
//   data[9..13]  frequency B — span   (centre mode) or upper edge (fixed)
//   data[14]     out-of-range flag
//   data[15..]   waveform, when this frame is also the LAST division (WLAN)
// and for every division after the first:
//   data[3..]    waveform
constexpr std::size_t kOffFixed      = 0;
constexpr std::size_t kOffDivision   = 1;
constexpr std::size_t kOffDivisionMax = 2;
constexpr std::size_t kOffMode       = 3;
constexpr std::size_t kOffFreqA      = 4;
constexpr std::size_t kOffContinuation = 3;

// Header size of a first-division frame, up to and including the out-of-range
// byte. Parameterised on the frequency width because the IC-905 uses 6-byte
// frequencies above 10 GHz and would otherwise silently misalign by two.
constexpr std::size_t firstDivisionHeader(std::size_t freqBytes)
{
    return kOffMode + 1 + freqBytes * 2 + 1;
}

}  // namespace

std::optional<ScopeFrame> ScopeDecoder::feed(const CivFrame& frame)
{
    if (frame.cmd != cmd::kScope || !frame.hasSub || frame.sub != scope::kWaveData)
        return std::nullopt;
    if (frame.data.size() <= kOffDivisionMax)
        return std::nullopt;

    // Both counters are BCD on the wire, so division 11 arrives as 0x11 rather
    // than 0x0b. Reading them as plain bytes works for 1..9 and then quietly
    // breaks at 10 — which over WLAN never happens, so it would ship.
    const int division    = decodeBcdByte(frame.data[kOffDivision]);
    const int divisionMax = decodeBcdByte(frame.data[kOffDivisionMax]);
    if (division < 1 || divisionMax < 1 || division > divisionMax)
        return std::nullopt;

    const std::size_t freqBytes = kFreqBytes;

    if (division == 1) {
        // A first division restarts assembly unconditionally. If a previous
        // sweep was left half-assembled by packet loss, this is where it gets
        // discarded — keeping it would splice two sweeps into one trace.
        m_partial = ScopeFrame{};
        m_assembling = true;
        m_expectedDivision = 1;

        if (frame.data.size() <= kOffMode)
            return std::nullopt;

        const std::uint8_t modeByte = frame.data[kOffMode];
        m_partial.mode = (modeByte <= static_cast<std::uint8_t>(ScopeMode::ScrollF))
                             ? static_cast<ScopeMode>(modeByte)
                             : ScopeMode::Fixed;

        const std::size_t header = firstDivisionHeader(freqBytes);
        if (frame.data.size() < header)
            return std::nullopt;

        // SIGNED, because a lower edge genuinely can be negative — the
        // IC-7300MK2 flags it with 0xF in the 1 GHz nibble when a wide span
        // sits near the bottom of the tuning range. The strict unsigned decode
        // would reject the byte and drop the entire sweep, so the panadapter
        // would go black at exactly the setting that produces it.
        const auto a = decodeFreqSigned(
            std::span<const std::uint8_t>(frame.data).subspan(kOffFreqA, freqBytes));
        const auto b = decodeFreqSigned(
            std::span<const std::uint8_t>(frame.data).subspan(kOffFreqA + freqBytes, freqBytes));
        if (!a || !b)
            return std::nullopt;

        if (m_partial.mode == ScopeMode::Centre) {
            // Centre mode reports CENTRE and SPAN, and Icom's span is a
            // HALF-WIDTH — the front panel reads "±100k" and the display covers
            // 200 kHz. So the edges are centre ∓ span, not centre ∓ span/2.
            m_partial.startHz = *a - *b;
            m_partial.endHz   = *a + *b;
        } else {
            // Fixed (and both scroll modes) report the edges directly.
            m_partial.startHz = *a;
            m_partial.endHz   = *b;
        }

        m_partial.outOfRange = frame.data[header - 1] != 0x00;
        if (m_partial.outOfRange) {
            // The radio omits the waveform entirely when out of range, so there
            // is nothing to accumulate. Emit a floor-filled sweep immediately:
            // a waterfall that stops scrolling reads as a hung application,
            // while one that goes flat reads as "nothing here", which is what
            // the radio is actually saying.
            m_partial.raw.assign(static_cast<std::size_t>(m_geom.points), 0);
            m_assembling = false;
            return m_partial;
        }

        if (frame.data.size() > header)
            m_partial.raw.insert(m_partial.raw.end(), frame.data.begin() + header,
                                 frame.data.end());

        if (division == divisionMax) {
            // The WLAN case: one frame, whole sweep. This is the path the
            // IC-705 backend runs, and it is why the multi-division machinery
            // below never executes over WiFi.
            m_assembling = false;
            m_partial.raw.resize(static_cast<std::size_t>(m_geom.points), 0);
            return m_partial;
        }
        return std::nullopt;
    }

    // Continuation divisions (USB transport only).
    if (!m_assembling)
        return std::nullopt;   // mid-sweep frame with no first division — dropped
    if (division != m_expectedDivision + 1) {
        // A gap means the sweep is unrecoverable: the waveform is positional and
        // there is no per-division index inside the payload to re-align against.
        // Abandon rather than concatenating a shifted trace.
        m_assembling = false;
        return std::nullopt;
    }
    m_expectedDivision = division;

    if (frame.data.size() > kOffContinuation)
        m_partial.raw.insert(m_partial.raw.end(), frame.data.begin() + kOffContinuation,
                             frame.data.end());

    if (division == divisionMax) {
        m_assembling = false;
        m_partial.raw.resize(static_cast<std::size_t>(m_geom.points), 0);
        return m_partial;
    }
    return std::nullopt;
}

void ScopeDecoder::reset() noexcept
{
    m_partial = ScopeFrame{};
    m_assembling = false;
    m_expectedDivision = 0;
}

std::vector<float> toDbm(const ScopeFrame& frame, const ScopeGeometry& geom,
                         const ScopeCalibration& cal)
{
    std::vector<float> out(frame.raw.size());
    const double denom = geom.maxAmplitude > 0 ? static_cast<double>(geom.maxAmplitude) : 1.0;
    for (std::size_t i = 0; i < frame.raw.size(); ++i) {
        // Clamp rather than trust: the published range is 0..160 but the field
        // is a full byte, and a value above the range would otherwise project
        // above the top of the scale and drag the waterfall's auto-contrast
        // with it.
        const double v = std::min<double>(frame.raw[i], denom);
        out[i] = static_cast<float>(cal.floorDbm + (v / denom) * cal.spanDb - cal.referenceDb);
    }
    return out;
}

int spanForBandwidthHz(int totalBandwidthHz) noexcept
{
    return nearestScopeSpanHz(totalBandwidthHz / 2);
}

int bandwidthForSpanHz(int spanHz) noexcept
{
    return spanHz * 2;
}

std::vector<int> availableBandwidthsHz()
{
    std::vector<int> out;
    out.reserve(kScopeSpansHz.size());
    for (int s : kScopeSpansHz)
        out.push_back(bandwidthForSpanHz(s));
    return out;
}

}  // namespace AetherSDR::icom
