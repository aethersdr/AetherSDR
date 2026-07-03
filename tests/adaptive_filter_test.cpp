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
QVector<float> buildSpectrum(bool usb, float floorDbm, float tiltDb,
                             const std::function<float(double)>& sigDbm)
{
    QVector<float> bins(kN);
    for (int i = 0; i < kN; ++i) {
        const double offHz = (i - kCarrierBin) * kHzPerBin;
        const double frac  = std::clamp(std::abs(offHz) / 6500.0, 0.0, 1.0);
        bins[i] = floorDbm + static_cast<float>(tiltDb * frac);
    }
    for (int o = 0; o * kHzPerBin <= 6500.0; ++o) {
        const float s = sigDbm(o * kHzPerBin);
        if (s <= -1000.0f) continue;
        const int bin = usb ? kCarrierBin + o : kCarrierBin - o;
        if (bin >= 0 && bin < kN) bins[bin] = std::max(bins[bin], s);
    }
    return bins;
}

OccupiedRegion measure(const QVector<float>& bins, bool usb, float noiseFloorDbm)
{
    QVector<float> avgEnv;
    return measureOccupiedRegion(bins, kCenter, kBwMhz, kCarrier,
                                 usb ? QStringLiteral("USB") : QStringLiteral("LSB"),
                                 noiseFloorDbm, avgEnv);
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
        // Peak only ~4 dB over the floor (< kMinPeakDb = 7) -> not confident.
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

    // ── 9. In-band reference is a MEDIAN, not the peak ──────────────────────
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
        report("reference is the core median, below the transient peak",
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

    std::printf("\n%s (%d failure%s)\n",
                g_failed ? "FAILED" : "PASSED",
                g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
