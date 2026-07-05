// Standalone test harness for the adaptive-RX-filter occupied-bandwidth
// edge-finder (measureOccupiedRegion, src/core/OccupiedRegion.cpp).
// CMake target `adaptive_filter_test`. Exit 0 = pass.
//
// Covers the four spec-driven behaviours added on top of the RFC #3878 core:
//   * per-frequency noise-floor curve (correct edges on a TILTED floor),
//   * in-band reference + reference-relative splatter cap,
//   * sharp-vs-soft per-edge placement,
//   * and the original guarantees (weak-signal gate, gap bridging, neighbour
//     rejection), each exercised for BOTH sidebands.
//
// Spectra are synthetic and deterministic (no noise): measureOccupiedRegion's
// temporal EMA initialises avgEnv to the instantaneous envelope on the first
// frame, so a single call is fully reproducible.

#include "core/OccupiedRegion.h"

#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

using AetherSDR::OccupiedRegion;
using AetherSDR::measureOccupiedRegion;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const char* detail = nullptr)
{
    std::printf("%s %-58s%s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail ? detail : "");
    if (!ok) ++g_failed;
}

// ── Synthetic panadapter geometry ───────────────────────────────────────────
constexpr int    kN       = 2048;
constexpr double kBwMhz   = 0.2;            // 200 kHz pan
constexpr double kCenter  = 14.200;         // MHz
constexpr double kCarrier = 14.200;         // MHz  -> carrierBin = 1024 (centre)
constexpr int    kCarrierBin = 1024;
const double     kHzPerBin = kBwMhz * 1.0e6 / kN;   // ~97.66 Hz/bin

// Build a spectrum: a flat (optionally tilted) noise floor, then a signal whose
// level (dBm, or <= -1000 = "no energy here") is given per AUDIO offset Hz and
// laid onto the correct energy side (USB above the carrier, LSB below).
// The generalized form takes an arbitrary geometry (bin count + span) so the
// decimation path (fine zoomed-in pans) can be exercised against the same
// signal shapes as the canonical coarse grid.
QVector<float> buildSpectrumAt(int n, double bwMhz, bool usb, float floorDbm,
                               float tiltDb,
                               const std::function<float(double)>& sigDbm)
{
    const int    carrierBin = n / 2;
    const double hzPerBin   = bwMhz * 1.0e6 / n;
    QVector<float> bins(n);
    for (int i = 0; i < n; ++i) {
        const double offHz = (i - carrierBin) * hzPerBin;
        const double frac  = std::clamp(std::abs(offHz) / 6500.0, 0.0, 1.0);
        bins[i] = floorDbm + static_cast<float>(tiltDb * frac);
    }
    for (int o = 0; o * hzPerBin <= 6500.0; ++o) {
        const float s = sigDbm(o * hzPerBin);
        if (s <= -1000.0f) continue;
        const int bin = usb ? carrierBin + o : carrierBin - o;
        if (bin >= 0 && bin < n) bins[bin] = std::max(bins[bin], s);
    }
    return bins;
}

QVector<float> buildSpectrum(bool usb, float floorDbm, float tiltDb,
                             const std::function<float(double)>& sigDbm)
{
    return buildSpectrumAt(kN, kBwMhz, usb, floorDbm, tiltDb, sigDbm);
}

OccupiedRegion measureAt(int n, double bwMhz, const QVector<float>& bins,
                         bool usb, float noiseFloorDbm)
{
    Q_UNUSED(n);
    QVector<float> avgEnv;
    return measureOccupiedRegion(bins, kCenter, bwMhz, kCarrier,
                                 usb ? QStringLiteral("USB") : QStringLiteral("LSB"),
                                 noiseFloorDbm, avgEnv);
}

OccupiedRegion measure(const QVector<float>& bins, bool usb, float noiseFloorDbm)
{
    return measureAt(kN, kBwMhz, bins, usb, noiseFloorDbm);
}

// A flat-topped voice "hump" between [lowHz, highHz] at `level`, else floor.
std::function<float(double)> hump(double lowHz, double highHz, float level)
{
    return [=](double f) -> float {
        return (f >= lowHz && f <= highHz) ? level : -1000.0f;
    };
}

const char* tag(bool usb) { return usb ? "USB" : "LSB"; }

// Run one closure for both sidebands.
void forEachMode(const std::function<void(bool usb)>& fn) { fn(true); fn(false); }

} // namespace

int main()
{
    // ── 1. Clean sharp SSB — edges hug the cliffs ───────────────────────────
    forEachMode([](bool usb) {
        const auto bins = buildSpectrum(usb, -110.0f, 0.0f, hump(300, 2700, -80.0f));
        const OccupiedRegion r = measure(bins, usb, -110.0f);
        char d[96];
        std::snprintf(d, sizeof d, "  [%s] low=%d high=%d", tag(usb), r.lowHz, r.highHz);
        report("clean sharp SSB: valid + plausible edges",
               r.valid && r.lowHz >= 0 && r.lowHz <= 500 &&
               r.highHz >= 2400 && r.highHz <= 3200, d);
    });

    // ── 2. Narrow het on top — does not pull the edges ──────────────────────
    forEachMode([](bool usb) {
        const auto sig = [](double f) -> float {
            if (f >= 1950 && f <= 2050) return -50.0f;      // a loud narrow het
            if (f >= 300 && f <= 2700)  return -80.0f;
            return -1000.0f;
        };
        const auto bins = buildSpectrum(usb, -110.0f, 0.0f, sig);
        const OccupiedRegion r = measure(bins, usb, -110.0f);
        char d[96];
        std::snprintf(d, sizeof d, "  [%s] low=%d high=%d", tag(usb), r.lowHz, r.highHz);
        report("narrow het: edges unmoved",
               r.valid && r.lowHz <= 500 && r.highHz >= 2400 && r.highHz <= 3200, d);
    });

    // ── 3. QSB internal gap — bridged, not chopped ──────────────────────────
    forEachMode([](bool usb) {
        const auto sig = [](double f) -> float {
            if (f >= 1200 && f <= 1600) return -1000.0f;    // deep internal notch
            if (f >= 300 && f <= 2700)  return -80.0f;
            return -1000.0f;
        };
        const auto bins = buildSpectrum(usb, -110.0f, 0.0f, sig);
        const OccupiedRegion r = measure(bins, usb, -110.0f);
        char d[96];
        std::snprintf(d, sizeof d, "  [%s] low=%d high=%d", tag(usb), r.lowHz, r.highHz);
        report("QSB internal gap: bridged (high-cut spans the notch)",
               r.valid && r.highHz >= 2400 && r.highHz <= 3200, d);
    });

    // ── 4. Slowly-decaying splatter tail — capped, not chased ───────────────
    forEachMode([](bool usb) {
        // Strong signal so the reference sits well above the floor and the
        // reference-relative cap (ref - 25 dB) bites: core -60, then a -90 shelf
        // (30 dB down) running far out. Without the cap the high-cut would chase
        // the shelf to ~6000 Hz.
        const auto sig = [](double f) -> float {
            if (f >= 300 && f <= 2400)  return -60.0f;
            if (f > 2400 && f <= 6000)  return -90.0f;       // splatter shelf
            return -1000.0f;
        };
        const auto bins = buildSpectrum(usb, -120.0f, 0.0f, sig);
        const OccupiedRegion r = measure(bins, usb, -120.0f);
        char d[96];
        std::snprintf(d, sizeof d, "  [%s] low=%d high=%d", tag(usb), r.lowHz, r.highHz);
        report("splatter tail: high-cut capped near the core edge",
               r.valid && r.highHz < 3500 && r.highHz > 1900, d);
    });

    // ── 5. Adjacent stronger neighbour — excluded at the valley ─────────────
    forEachMode([](bool usb) {
        const auto sig = [](double f) -> float {
            if (f >= 300 && f <= 2200)  return -80.0f;       // wanted signal
            if (f >= 2200 && f <= 2600) return -1000.0f;     // valley
            if (f > 2600 && f <= 4000)  return -65.0f;       // STRONGER neighbour
            return -1000.0f;
        };
        const auto bins = buildSpectrum(usb, -110.0f, 0.0f, sig);
        const OccupiedRegion r = measure(bins, usb, -110.0f);
        char d[96];
        std::snprintf(d, sizeof d, "  [%s] low=%d high=%d", tag(usb), r.lowHz, r.highHz);
        report("stronger neighbour: cut at the valley",
               r.valid && r.highHz < 2700 && r.highHz > 1800, d);
    });

    // ── 6. Soft analog roll-off — useful extent captured ────────────────────
    forEachMode([](bool usb) {
        // Flat to 1500 Hz at -60, then a gentle ~24 dB/kHz roll-off to the floor
        // at ~4000 Hz (no steep cliff -> the soft criterion governs).
        const auto sig = [](double f) -> float {
            if (f < 300)  return -1000.0f;
            if (f <= 1500) return -60.0f;
            if (f <= 4000) return static_cast<float>(-60.0 - 24.0 * (f - 1500.0) / 1000.0);
            return -1000.0f;
        };
        const auto bins = buildSpectrum(usb, -120.0f, 0.0f, sig);
        const OccupiedRegion r = measure(bins, usb, -120.0f);
        char d[96];
        std::snprintf(d, sizeof d, "  [%s] low=%d high=%d", tag(usb), r.lowHz, r.highHz);
        // Captured within the useful band, and well inside the floor crossing
        // (~3800 Hz) — the soft criterion pins it to the reference, not the floor.
        report("soft roll-off: edge pinned to useful extent",
               r.valid && r.highHz >= 1900 && r.highHz <= 3400, d);
    });

    // ── 7. Weak signal below the presence gate — no fit ─────────────────────
    forEachMode([](bool usb) {
        // Peak only ~4 dB over the floor (< the Normal minPeakDb gate, 9 dB) -> not confident.
        const auto bins = buildSpectrum(usb, -110.0f, 0.0f, hump(300, 2700, -106.0f));
        const OccupiedRegion r = measure(bins, usb, -110.0f);
        char d[64];
        std::snprintf(d, sizeof d, "  [%s] valid=%d", tag(usb), r.valid ? 1 : 0);
        report("weak signal: presence gate rejects (no fit)", !r.valid, d);
    });

    // ── 8. Tilted noise floor — floor curve places the edge correctly ───────
    forEachMode([](bool usb) {
        // Floor rises 17 dB across the scan. A single scalar floor (the global
        // low value) would read the elevated noise past the signal as "occupied"
        // and run the high-cut out to the scan edge; the per-frequency curve cuts
        // at the true signal edge instead.
        const auto bins = buildSpectrum(usb, -120.0f, 17.0f, hump(300, 2700, -70.0f));
        const OccupiedRegion r = measure(bins, usb, -120.0f);
        char d[96];
        std::snprintf(d, sizeof d, "  [%s] low=%d high=%d", tag(usb), r.lowHz, r.highHz);
        report("tilted floor: high-cut at the signal edge (no runaway)",
               r.valid && r.highHz >= 2400 && r.highHz <= 3500, d);
    });

    // ── 9. In-band reference is the 75th percentile, below the peak ─────────
    forEachMode([](bool usb) {
        // A loud transient bin inside the core must not drag the reference up.
        const auto sig = [](double f) -> float {
            if (f >= 750 && f <= 850)  return -50.0f;        // loud transient
            if (f >= 300 && f <= 2700) return -80.0f;
            return -1000.0f;
        };
        const auto bins = buildSpectrum(usb, -110.0f, 0.0f, sig);
        const OccupiedRegion r = measure(bins, usb, -110.0f);
        char d[112];
        std::snprintf(d, sizeof d, "  [%s] ref=%.1f peak=%.1f",
                      tag(usb), r.referenceDbm, r.peakDbm);
        report("reference is the core 75th percentile, below the transient peak",
               r.valid && r.referenceDbm <= -74.0f && r.referenceDbm >= -88.0f &&
               r.referenceDbm < r.peakDbm - 3.0f, d);
    });

    // ── 10. Declining voice to the floor — NOT over-cut (on-air bug) ────────
    forEachMode([](bool usb) {
        // Loud near-carrier core, then a gradual roll-off that reaches the noise
        // floor near 3000 Hz. The core inflates the in-band reference, so a bare
        // reference-relative cap chopped this ~3 kHz signal to ~1.8 kHz on air.
        // The floor crossing (~3000) is the correct high-cut and must be trusted.
        const auto sig = [](double f) -> float {
            if (f < 300)   return -1000.0f;
            if (f <= 900)  return -55.0f;                                  // loud core
            if (f <= 3100) return static_cast<float>(-78.0 - 14.3 * (f - 900.0) / 1000.0);
            return -1000.0f;
        };
        const auto bins = buildSpectrum(usb, -112.0f, 0.0f, sig);
        const OccupiedRegion r = measure(bins, usb, -112.0f);
        char d[96];
        std::snprintf(d, sizeof d, "  [%s] low=%d high=%d", tag(usb), r.lowHz, r.highHz);
        report("declining voice: high-cut follows the floor crossing",
               r.valid && r.highHz >= 2700 && r.highHz <= 3300, d);
    });

    // ── 11. Treble-dominant signal — dominant high hump captured ────────────
    forEachMode([](bool usb) {
        // Weak near-carrier energy, a dip below the occupied gate, then a STRONG
        // high-frequency hump (the wanted signal's energy peaks well above the
        // carrier). The hump must be captured — not cut off as if it were a louder
        // neighbour (which chopped a ~3 kHz signal to the 1.8 kHz floor on air).
        const auto sig = [](double f) -> float {
            if (f < 300)   return -1000.0f;
            if (f <= 1100) return -92.0f;     // weak near-carrier
            if (f <= 1500) return -116.0f;    // relative dip (near, not at, floor)
            if (f <= 2900) return -74.0f;     // dominant high hump
            return -1000.0f;
        };
        const auto bins = buildSpectrum(usb, -120.0f, 0.0f, sig);
        const OccupiedRegion r = measure(bins, usb, -120.0f);
        char d[96];
        std::snprintf(d, sizeof d, "  [%s] low=%d high=%d ref=%.1f",
                      tag(usb), r.lowHz, r.highHz, r.referenceDbm);
        report("treble-dominant: high hump captured (not cut before it)",
               r.valid && r.highHz > 2500, d);
    });

    // ── 12. Intermittent upper voice — high edge HELD across gaps (temporal) ─
    forEachMode([](bool usb) {
        // A ~2900 Hz signal, only ~12 dB over the floor, whose upper band (>1000
        // Hz) empties during speech gaps — exactly how SSB voice behaves. The
        // measured high edge must reflect the SUSTAINED reach (~2900), not collapse
        // to the low formants every gap. The fast-attack / slow-release envelope
        // holds it open; a symmetric average would sag below the gate and pinch.
        const auto full = buildSpectrum(usb, -118.0f, 0.0f, hump(300, 2900, -106.0f));
        const auto gap  = buildSpectrum(usb, -118.0f, 0.0f, hump(300, 1000, -106.0f));
        QVector<float> avgEnv;   // shared across frames -> temporal behaviour
        const auto call = [&](const QVector<float>& b) {
            return measureOccupiedRegion(
                b, kCenter, kBwMhz, kCarrier,
                usb ? QStringLiteral("USB") : QStringLiteral("LSB"),
                -118.0f, avgEnv);
        };
        for (int i = 0; i < 20; ++i) call(full);     // establish the wide edge
        OccupiedRegion r;
        for (int i = 0; i < 20; ++i) r = call(gap);  // ~0.7 s upper-band gap
        char d[96];
        std::snprintf(d, sizeof d, "  [%s] high after gap=%d", tag(usb), r.highHz);
        report("intermittent upper voice: high edge held across the gap",
               r.valid && r.highHz >= 2500, d);
    });

    // ── 13. Fine-grid decimation — zoomed-in pans match the coarse fit ──────
    // A 50 kHz span across 8192 bins is ~6.1 Hz/bin (deep zoom on a Retina
    // pan); the resolution cap decimates it to ~30.5 Hz/bin. The measured fit
    // must match the canonical coarse grid (~98 Hz/bin) for the same signal
    // shapes within decimation tolerance.
    {
        struct Shape { const char* name; std::function<float(double)> sig; };
        const Shape shapes[] = {
            { "clean sharp",  hump(300, 2700, -80.0f) },
            { "soft rolloff", [](double f) -> float {
                  if (f < 300)   return -1000.0f;
                  if (f <= 1500) return -60.0f;
                  if (f <= 4000) return static_cast<float>(-60.0 - 24.0 * (f - 1500.0) / 1000.0);
                  return -1000.0f; } },
            { "declining",    [](double f) -> float {
                  if (f < 300)   return -1000.0f;
                  if (f <= 900)  return -55.0f;
                  if (f <= 3100) return static_cast<float>(-78.0 - 14.3 * (f - 900.0) / 1000.0);
                  return -1000.0f; } },
        };
        constexpr int    kFineN  = 8192;
        constexpr double kFineBw = 0.05;   // MHz -> ~6.1 Hz/bin -> D=5
        forEachMode([&](bool usb) {
            for (const auto& s : shapes) {
                const OccupiedRegion rc = measure(
                    buildSpectrum(usb, -120.0f, 0.0f, s.sig), usb, -120.0f);
                const OccupiedRegion rf = measureAt(kFineN, kFineBw,
                    buildSpectrumAt(kFineN, kFineBw, usb, -120.0f, 0.0f, s.sig),
                    usb, -120.0f);
                char d[128];
                std::snprintf(d, sizeof d,
                              "  [%s %s] coarse=%d..%d fine=%d..%d",
                              tag(usb), s.name, rc.lowHz, rc.highHz, rf.lowHz, rf.highHz);
                report("fine-grid decimation: fit matches coarse grid",
                       rc.valid && rf.valid &&
                       std::abs(rf.lowHz  - rc.lowHz)  <= 150 &&
                       std::abs(rf.highHz - rc.highHz) <= 150 &&
                       std::abs(rf.referenceDbm - rc.referenceDbm) <= 2.0f, d);
            }
        });
    }

    // ── 14. Two-hump spectra (smiley EQ / tilted ESSB) — extent decisions ───
    // The extent pass adjudicates a resume across a floor-reaching valley by
    // rebound level AND run confidence: same-signal recoveries bridge, a
    // stronger lobe past a CONFIDENT run is a neighbour (cut), a stronger lobe
    // past an UNCONFIDENT run is the tuned signal's dominant hump (re-anchor,
    // if it starts by kReanchorMaxStartHz). Floor -120 => floorGate -117,
    // occThr -115, Normal confidence = floor+9 = -111.
    forEachMode([](bool usb) {
        // (a) 150 Hz valley — below the 250 Hz disconnection standard: bridged.
        {
            const auto sig = [](double f) -> float {
                if (f >= 300 && f <= 1000)  return -100.0f;
                if (f >= 1150 && f <= 2800) return -95.0f;
                return -1000.0f;
            };
            const OccupiedRegion r = measure(
                buildSpectrum(usb, -120.0f, 0.0f, sig), usb, -120.0f);
            char d[96];
            std::snprintf(d, sizeof d, "  [%s] low=%d high=%d", tag(usb), r.lowHz, r.highHz);
            report("two-hump: sub-standard 150 Hz valley bridged",
                   r.valid && r.highHz >= 2500, d);
        }
        // (b) 500 Hz floor valley, treble only +6 dB (<= kReboundDb): a
        // same-signal recovery — bridged.
        {
            const auto sig = [](double f) -> float {
                if (f >= 300 && f <= 1000)  return -100.0f;
                if (f >= 1500 && f <= 2800) return -94.0f;
                return -1000.0f;
            };
            const OccupiedRegion r = measure(
                buildSpectrum(usb, -120.0f, 0.0f, sig), usb, -120.0f);
            char d[96];
            std::snprintf(d, sizeof d, "  [%s] low=%d high=%d", tag(usb), r.lowHz, r.highHz);
            report("two-hump: +6 dB resume across 500 Hz valley bridged",
                   r.valid && r.highHz >= 2500, d);
        }
        // (c) 500 Hz floor valley, +12 dB lobe past a CONFIDENT (floor+30) run:
        // indistinguishable from a stronger neighbour — cut at the valley.
        {
            const auto sig = [](double f) -> float {
                if (f >= 300 && f <= 1000)  return -90.0f;
                if (f >= 1500 && f <= 2800) return -78.0f;
                return -1000.0f;
            };
            const OccupiedRegion r = measure(
                buildSpectrum(usb, -120.0f, 0.0f, sig), usb, -120.0f);
            char d[96];
            std::snprintf(d, sizeof d, "  [%s] low=%d high=%d", tag(usb), r.lowHz, r.highHz);
            report("two-hump: +12 dB lobe past confident run cut",
                   r.valid && r.highHz > 900 && r.highHz < 1500, d);
        }
        // (d) Weak bass (floor+7, below the Normal presence preset) + dominant
        // treble (floor+25) across a 500 Hz floor valley — the smiley-EQ case
        // that used to be amputated at the scoop (or rejected outright, since
        // presence was judged on the bass-only run): re-anchored + valid.
        {
            const auto sig = [](double f) -> float {
                if (f >= 300 && f <= 1000)  return -113.0f;
                if (f >= 1500 && f <= 2900) return -95.0f;
                return -1000.0f;
            };
            const OccupiedRegion r = measure(
                buildSpectrum(usb, -120.0f, 0.0f, sig), usb, -120.0f);
            char d[96];
            std::snprintf(d, sizeof d, "  [%s] low=%d high=%d", tag(usb), r.lowHz, r.highHz);
            report("two-hump: weak bass + dominant treble re-anchored",
                   r.valid && r.lowHz <= 500 && r.highHz >= 2600, d);
        }
        // (e) 1000 Hz at-floor mid scoop (wider than kSilenceHz) on an
        // unconfident bass — the silence-stop no longer truncates: the scan
        // looks ahead and captures the treble hump (was: high ~1029, AUTO
        // confidently wrong).
        {
            const auto sig = [](double f) -> float {
                if (f >= 300 && f <= 900)   return -113.0f;
                if (f >= 1900 && f <= 3100) return -95.0f;
                return -1000.0f;
            };
            const OccupiedRegion r = measure(
                buildSpectrum(usb, -120.0f, 0.0f, sig), usb, -120.0f);
            char d[96];
            std::snprintf(d, sizeof d, "  [%s] low=%d high=%d", tag(usb), r.lowHz, r.highHz);
            report("two-hump: wide at-floor scoop looked past (silence-stop)",
                   r.valid && r.highHz >= 2800, d);
        }
        // (g) Re-anchor guard: a sub-confidence blip near the carrier + a
        // strong lobe first appearing at 2600 Hz (past kReanchorMaxStartHz) is
        // an adjacent station above a silent low band — NOT re-anchored, and
        // the blip alone fails the presence gate -> invalid.
        {
            const auto sig = [](double f) -> float {
                if (f >= 300 && f <= 600)   return -112.0f;
                if (f >= 2600 && f <= 4000) return -80.0f;
                return -1000.0f;
            };
            const OccupiedRegion r = measure(
                buildSpectrum(usb, -120.0f, 0.0f, sig), usb, -120.0f);
            char d[64];
            std::snprintf(d, sizeof d, "  [%s] valid=%d", tag(usb), r.valid ? 1 : 0);
            report("two-hump: distant lobe not re-anchored (reject)", !r.valid, d);
        }
    });

    // ── 15. Level-invariant outer edge — width must not track SNR ───────────
    // The same signal shape (flat core 300-2400, then a soft 30 dB/kHz skirt)
    // at three levels 20 dB apart: a floor-relative edge would move the
    // crossing ~333 Hz per 10 dB, but the in-guard reference cap pins the edge
    // at a fixed depth below the in-band reference — the same Hz whatever the
    // level. This is the QSB case: the passband must not breathe with fades.
    forEachMode([](bool usb) {
        const auto shapeAt = [](float coreDbm) {
            return [coreDbm](double f) -> float {
                if (f < 300)    return -1000.0f;
                if (f <= 2400)  return coreDbm;
                return static_cast<float>(coreDbm - 30.0 * (f - 2400.0) / 1000.0);
            };
        };
        int highs[3] = {0, 0, 0};
        bool allValid = true;
        const float levels[3] = {-60.0f, -70.0f, -80.0f};
        for (int i = 0; i < 3; ++i) {
            const OccupiedRegion r = measure(
                buildSpectrum(usb, -120.0f, 0.0f, shapeAt(levels[i])), usb, -120.0f);
            allValid = allValid && r.valid;
            highs[i] = r.highHz;
        }
        const int spread = std::max({highs[0], highs[1], highs[2]}) -
                           std::min({highs[0], highs[1], highs[2]});
        char d[112];
        std::snprintf(d, sizeof d, "  [%s] high=%d/%d/%d spread=%d",
                      tag(usb), highs[0], highs[1], highs[2], spread);
        report("level sweep: soft-skirt high-cut level-invariant",
               allValid && spread <= 250 &&
               highs[0] >= 3000 && highs[0] <= 3700, d);
    });

    // ── 16. FCC two-tone occupied-bandwidth vectors (47 CFR 2.1049) ──────────
    // The FCC's type-acceptance excitations for SSB: two tones whose spacing
    // defines the occupied bandwidth (400/1800 -> 3.0 kHz class, 500/2100 ->
    // 3.5 kHz, 500/2400 -> 4.0 kHz). With realistic steep TX skirts the
    // measured band must end just past the upper tone and start at or below
    // the lower tone — regulator-defined ground truth for the whole chain.
    forEachMode([](bool usb) {
        const struct { double f1, f2; } pairs[] = {
            {400.0, 1800.0}, {500.0, 2100.0}, {500.0, 2400.0} };
        for (const auto& p : pairs) {
            const auto sig = [&p](double f) -> float {
                if (std::abs(f - p.f1) <= 50.0 || std::abs(f - p.f2) <= 50.0)
                    return -60.0f;                        // the two tones
                if (f > p.f1 && f < p.f2) return -70.0f;  // intermod fill
                if (f > p.f2)                              // steep TX skirt
                    return static_cast<float>(-70.0 - 120.0 * (f - p.f2) / 1000.0);
                if (f < p.f1 && f >= 100.0)
                    return static_cast<float>(-70.0 - 120.0 * (p.f1 - f) / 1000.0);
                return -1000.0f;
            };
            const OccupiedRegion r = measure(
                buildSpectrum(usb, -120.0f, 0.0f, sig), usb, -120.0f);
            char d[112];
            std::snprintf(d, sizeof d, "  [%s %.0f/%.0f] low=%d high=%d",
                          tag(usb), p.f1, p.f2, r.lowHz, r.highHz);
            report("FCC two-tone: band ends just past the upper tone",
                   r.valid && r.lowHz <= static_cast<int>(p.f1) &&
                   r.highHz >= static_cast<int>(p.f2) - 100 &&
                   r.highHz <= static_cast<int>(p.f2) + 500, d);
        }
    });

    // ── 17. floorDbm export — the scalar floor the measurement used ─────────
    forEachMode([](bool usb) {
        const auto bins = buildSpectrum(usb, -110.0f, 0.0f, hump(300, 2700, -80.0f));
        const OccupiedRegion rs = measure(bins, usb, -110.0f);    // supplied
        const OccupiedRegion rf = measure(bins, usb, -1000.0f);   // sentinel
        char d[96];
        std::snprintf(d, sizeof d, "  [%s] supplied=%.1f fallback=%.1f",
                      tag(usb), rs.floorDbm, rf.floorDbm);
        report("floorDbm export: supplied passthrough + local fallback",
               rs.valid && rf.valid &&
               std::abs(rs.floorDbm - (-110.0f)) < 0.1f &&
               rf.floorDbm > -114.0f && rf.floorDbm < -105.0f, d);
    });

    // ── 18. Carrier near the pan edge — scan clipped, no out-of-range access ─
    // A slice tuned within kScanHz of the pan edge, WITH signal energy on the
    // energy side, makes the extent pass walk offsets whose bins lie past the
    // pan; every access must clamp (this crashed on air: QList assert in the
    // extent pass's raw-bin arming, slice near the top of the pan). Exercised
    // on the canonical grid and on the decimation path.
    forEachMode([](bool usb) {
        // Paint floor + a voice hump RELATIVE TO the edge carrier (clipped at
        // the pan boundary), then measure at that carrier.
        const auto edgeCase = [usb](int n, double bwMhz) -> OccupiedRegion {
            const double hzPerBin = bwMhz * 1.0e6 / n;
            const double startMhz = kCenter - bwMhz / 2.0;
            const double edgeCarrier = usb ? kCenter + bwMhz / 2.0 - 0.002
                                           : kCenter - bwMhz / 2.0 + 0.002;
            const int carrierBin = static_cast<int>(
                std::lround((edgeCarrier - startMhz) / bwMhz * n));
            QVector<float> bins(n, -110.0f);
            for (int o = 0; o * hzPerBin <= 2700.0; ++o) {
                if (o * hzPerBin < 300.0) continue;
                const int bin = usb ? carrierBin + o : carrierBin - o;
                if (bin >= 0 && bin < n) bins[bin] = -80.0f;
            }
            QVector<float> avgEnv;
            return measureOccupiedRegion(
                bins, kCenter, bwMhz, edgeCarrier,
                usb ? QStringLiteral("USB") : QStringLiteral("LSB"),
                -110.0f, avgEnv);
        };
        const OccupiedRegion r  = edgeCase(kN, kBwMhz);   // canonical grid
        const OccupiedRegion rf = edgeCase(8192, 0.05);   // decimated path
        char d[96];
        std::snprintf(d, sizeof d, "  [%s] valid=%d fineValid=%d",
                      tag(usb), r.valid ? 1 : 0, rf.valid ? 1 : 0);
        // Reaching here without an assert IS the test; both grids must also
        // still produce a fit from the clipped-but-present energy.
        report("edge carrier: clipped scan measures without OOB access",
               r.valid && rf.valid, d);
    });

    std::printf("\n%s (%d failure%s)\n",
                g_failed ? "FAILED" : "PASSED",
                g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
