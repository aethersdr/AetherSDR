#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/backends/icom/CivCodec.h"

// Decoder for the Icom scope — CI-V command 0x27 0x00 — which is this backend's
// PANADAPTER. There is no IQ on any networked Icom, so this cooked display
// array is the only spectrum the radio will ever give us.
//
// Qt-free; icom_scope_test drives it from synthetic frames.
//
// See ~/oracles/icom/icom-oracle.md §5.

namespace AetherSDR::icom {

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

// IC-705. Both are per-model — an IC-7610 reports 689 points — so they live in
// a model record rather than being compiled in at the use site. These are the
// defaults that record is seeded with.
inline constexpr int kScopePointsIc705 = 475;
inline constexpr int kScopeMaxAmplitude = 160;   // the published data range, 0..160

struct ScopeGeometry {
    int points       = kScopePointsIc705;
    int maxAmplitude = kScopeMaxAmplitude;
    // Divisions the radio splits the waveform into. 1 over WLAN — the whole
    // spectrum arrives in ONE packet — and 11 over USB. Stated by Icom in the
    // CI-V guide, not inferred.
    int divisionsLan = 1;
    int divisionsUsb = 11;
};

enum class ScopeMode : std::uint8_t {
    Centre = 0x00,
    Fixed  = 0x01,
    // REAL, and confirmed tier-1 on the IC-7300MK2, whose CI-V guide lists all
    // four values for 0x27 0x14. The IC-705's guide lists only 00 and 01, so
    // the mode set is per-model rather than universal.
    //
    // Geometrically the scroll modes behave like Fixed — they report lower and
    // upper edges directly rather than centre+span — which is why the decoder
    // needs no separate branch for them. Only the label differs.
    ScrollC = 0x02,
    ScrollF = 0x03,
};

// One fully-assembled sweep.
struct ScopeFrame {
    ScopeMode mode = ScopeMode::Centre;
    // Edges, in Hz, ALREADY normalised out of whichever representation the
    // radio used — see the note on Centre mode in IcomScope.cpp.
    //
    // SIGNED, and startHz really can be negative: the IC-7300MK2's guide
    // documents a sign flag on the lower edge for when a wide span sits near
    // the bottom of the tuning range and the window extends below 0 Hz.
    // Clamping it to zero here would keep the number plausible and silently
    // shrink the span, so every bin would map to the wrong frequency.
    std::int64_t startHz = 0;
    std::int64_t endHz   = 0;
    // The radio is telling us the scope cannot show this range. When set, the
    // radio OMITS the waveform data entirely and the packet is short; we still
    // produce a frame (floor-filled) rather than dropping it, because a stalled
    // waterfall reads as a broken app while a flat one reads as out of range.
    bool outOfRange = false;
    // Raw display units, 0..160. NOT dBm — see toDbm().
    std::vector<std::uint8_t> raw;

    [[nodiscard]] std::int64_t centreHz() const noexcept { return (startHz + endHz) / 2; }
    [[nodiscard]] std::int64_t bandwidthHz() const noexcept
    {
        return endHz > startHz ? endHz - startHz : 0;
    }
};

// ---------------------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------------------

// THE SCOPE IS NOT CALIBRATED. Its 0..160 are display units relative to the
// radio's own reference-level setting (0x27 0x19), and Icom publishes no
// mapping to absolute power. Anything that presents this as dBm is inventing a
// measurement.
//
// This struct is that invention, made explicit and adjustable rather than
// scattered as magic numbers — the same shape as the HL2's Hl2DbReference. The
// defaults put a nominal IC-705 sweep in a plausible place so the waterfall's
// auto-contrast has something sane to work with; they are ESTIMATES and are
// flagged as such wherever they surface.
//
// The way to make them real is the S-meter, which IS calibrated (0 = S0,
// 120 = S9, 241 = S9+60 dB): put a known signal in the passband, read 0x15 0x02,
// and compare. Until someone does that on a bench, the UI must not label this
// axis as an absolute measurement.
struct ScopeCalibration {
    // Display value 0 maps here.
    double floorDbm = -140.0;
    // Display value maxAmplitude maps to floorDbm + spanDb.
    double spanDb = 80.0;
    // The radio's own reference level (0x27 0x19), in dB. Shifts the whole
    // trace; read it back rather than assuming zero, because the operator can
    // change it from the front panel and we would otherwise track 20 dB out.
    double referenceDb = 0.0;
    // True once a bench measurement has replaced the estimates above. Nothing
    // reads this yet; it exists so the UI can eventually distinguish "estimated"
    // from "measured" rather than the distinction living only in a comment.
    bool measured = false;
};

// Convert one sweep to dBm using `cal`. Length matches frame.raw.
[[nodiscard]] std::vector<float> toDbm(const ScopeFrame& frame, const ScopeGeometry& geom,
                                        const ScopeCalibration& cal);

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

// Assembles ScopeFrames from 0x27 0x00 CI-V frames.
//
// Over WLAN this is trivial — division max is 1, so one CI-V frame is one
// complete sweep — and that is the path the IC-705 backend actually runs. The
// multi-division path exists for a future USB/local-serial transport, where the
// radio splits the sweep into 11 chunks; it is implemented here rather than
// deferred because the two share every other line, and a decoder that only
// works over one transport is the thing that makes adding the other expensive.
class ScopeDecoder {
public:
    explicit ScopeDecoder(ScopeGeometry geom = {}) : m_geom(geom) {}

    // Feed one CI-V frame. Returns a complete sweep when this frame finished
    // one; nullopt while a multi-division sweep is still assembling, or when
    // the frame is not scope data at all.
    [[nodiscard]] std::optional<ScopeFrame> feed(const CivFrame& frame);

    void reset() noexcept;
    [[nodiscard]] const ScopeGeometry& geometry() const noexcept { return m_geom; }

private:
    ScopeGeometry m_geom;
    ScopeFrame m_partial;
    bool m_assembling = false;
    int  m_expectedDivision = 0;
};

// ---------------------------------------------------------------------------
// Span, and the factor-of-two that is easy to miss
// ---------------------------------------------------------------------------

// Icom's scope SPAN is a HALF-WIDTH: the front panel shows "±100k" and the
// display covers 200 kHz. AetherSDR's setPanBandwidth() takes a TOTAL width.
//
// Getting this backwards produces a panadapter that is right about its centre
// and wrong by 2x about its extent — which renders as signals at half or double
// their true offset from centre, and is exactly the class of error that looks
// like a tuning bug rather than a geometry bug.
[[nodiscard]] int spanForBandwidthHz(int totalBandwidthHz) noexcept;
[[nodiscard]] int bandwidthForSpanHz(int spanHz) noexcept;

// The total bandwidths reachable on this radio, derived from kScopeSpansHz.
// This is what a backend reports through panBandwidthLimitsChanged, and it is
// why zoom on an Icom snaps: there are eight of them and nothing in between.
[[nodiscard]] std::vector<int> availableBandwidthsHz();

}  // namespace AetherSDR::icom
