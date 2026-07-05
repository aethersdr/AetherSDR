// Engine-level test for the adaptive-RX-filter temporal pipeline
// (AdaptiveFilterEngine::processFrame). Unlike adaptive_filter_test (which
// drives the stateless measurement measureOccupiedRegion directly), this drives
// the QObject engine through a real SliceModel in a headless QCoreApplication,
// feeding synthetic panadapter frames WITH monotonic timestamps — the path that
// the production caller (PanadapterStream -> onSpectrumReadyForAdaptiveFilter)
// exercises and that no measurement test can cover. CMake target
// `adaptive_engine_test`. Exit 0 = pass.
//
// SliceModel setters only emit signals (no radio/socket), so a real instance is
// a faithful, cheap test double; the engine's applyAdaptiveFilter writes surface
// as SliceModel::filterChanged, which we record.

#include "core/AdaptiveFilterEngine.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

using AetherSDR::AdaptiveFilterEngine;
using AetherSDR::SliceModel;

namespace {

int g_failed = 0;
void report(const char* name, bool ok, const char* detail = nullptr)
{
    std::printf("%s %-58s%s\n", ok ? "[ OK ]" : "[FAIL]", name, detail ? detail : "");
    if (!ok) ++g_failed;
}

// ── Synthetic panadapter geometry (matches adaptive_filter_test) ────────────
constexpr int    kN      = 2048;
constexpr double kBwMhz  = 0.2;       // 200 kHz pan
constexpr double kCenter = 14.200;    // MHz (carrier at centre)
constexpr int    kCarrierBin = kN / 2;
const double     kHzPerBin = kBwMhz * 1.0e6 / kN;   // ~97.66 Hz/bin
constexpr qint64 kFps60Ns = 16'666'667;             // 60 fps input step

// A USB voice hump [lowHz, highHz] at `level` dBm over a flat floor.
QVector<float> humpSpectrum(float floorDbm, double lowHz, double highHz, float level)
{
    QVector<float> bins(kN, floorDbm);
    for (int o = 0; o * kHzPerBin <= 6500.0; ++o) {
        const double f = o * kHzPerBin;
        if (f >= lowHz && f <= highHz) {
            const int bin = kCarrierBin + o;
            if (bin >= 0 && bin < kN) bins[bin] = level;
        }
    }
    return bins;
}

QVector<float> noiseSpectrum(float floorDbm) { return QVector<float>(kN, floorDbm); }

// A driver: an engine + slice + recorder of the applied (low,high) with the
// timestamp of the frame that produced each write.
struct Driver {
    AdaptiveFilterEngine engine;
    SliceModel slice{1};
    struct Write { qint64 ns; int low; int high; };
    std::vector<Write> writes;
    qint64 curNs{0};

    Driver(int baseLow, int baseHigh)
    {
        slice.setMode(QStringLiteral("USB"));
        slice.setFrequency(kCenter);
        slice.setAdaptiveMinLowCut(0);
        slice.setAdaptiveMaxHighCut(6000);
        slice.setAdaptiveMinSnr(1);      // Normal
        slice.setAdaptiveResponse(1);    // Normal
        slice.setAdaptiveSplatter(1);    // Normal
        slice.setFilterWidth(baseLow, baseHigh);   // seed baseline (bumps epoch)
        slice.setAdaptiveFilterEnabled(true);
        QObject::connect(&slice, &SliceModel::filterChanged, &slice,
                         [this](int lo, int hi) { writes.push_back({curNs, lo, hi}); });
    }

    void feed(const QVector<float>& bins, float floor, qint64 ns)
    {
        curNs = ns;
        engine.processFrame(&slice, kCenter, kBwMhz, bins, floor, ns);
    }
    // Drive n frames of the same spectrum at 60 fps starting at startNs.
    qint64 run(const QVector<float>& bins, float floor, int n, qint64 startNs)
    {
        qint64 ns = startNs;
        for (int i = 0; i < n; ++i) { feed(bins, floor, ns); ns += kFps60Ns; }
        return ns;
    }
    int lastHigh() const { return writes.empty() ? -1 : writes.back().high; }
    int lastLow()  const { return writes.empty() ? -1 : writes.back().low; }
};

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ── 1. No filt-command storm at 60 fps (the dead-clock regression lock) ──
    // With the wall-clock layer inert (emittedNs==0), a 60 fps pan sent ~15
    // filt/s; paced + wall-clock-guarded it must stay <= ~8/s with >= 125 ms
    // spacing (RFC #3878 cond. 2). Drive 2 s of 60 fps frames of a wide strong
    // signal that forces the passband to glide well away from the baseline.
    {
        Driver d(0, 2700);
        const auto sig = humpSpectrum(-110.0f, 300.0, 4000.0, -70.0f);
        d.run(sig, -110.0f, 120, 0);   // 2 s @ 60 fps
        // Count sends and the minimum spacing between consecutive sends.
        qint64 minGap = 1'000'000'000; int sends = int(d.writes.size());
        for (size_t i = 1; i < d.writes.size(); ++i)
            minGap = std::min(minGap, d.writes[i].ns - d.writes[i - 1].ns);
        const double perSec = sends / 2.0;
        char det[128];
        std::snprintf(det, sizeof det, "  sends=%d (%.1f/s) minGap=%.0fms",
                      sends, perSec, d.writes.size() > 1 ? minGap / 1e6 : 0.0);
        // Require >= 2 sends so the rate lock can't pass vacuously (a future
        // change that stops the glide sending entirely must fail, not go green).
        report("no filt storm at 60 fps: <=8/s and >=125ms spacing",
               perSec <= 8.0 && d.writes.size() >= 2 && minGap >= 124'000'000, det);
    }

    // ── 2. Engage tracks a single signal ────────────────────────────────────
    // A strong 300-3000 Hz signal from a 2700 baseline: the high-cut should
    // settle near 3000 (within margin + snap) after ~2 s.
    {
        Driver d(0, 2700);
        const auto sig = humpSpectrum(-110.0f, 300.0, 3000.0, -70.0f);
        d.run(sig, -110.0f, 120, 0);
        char det[96];
        std::snprintf(det, sizeof det, "  high=%d low=%d", d.lastHigh(), d.lastLow());
        report("engage: high-cut tracks a 3.0 kHz signal",
               d.lastHigh() >= 2800 && d.lastHigh() <= 3400, det);
    }

    // ── 3. Word-gap HOLD: a short gap must NOT collapse the fit ──────────────
    // Lock onto 300-4000, then 0.5 s of noise (below the between-overs flush
    // threshold), then signal again — the high-cut must stay wide (~4000).
    {
        Driver d(0, 2700);
        const auto sig = humpSpectrum(-110.0f, 300.0, 4000.0, -70.0f);
        qint64 ns = d.run(sig, -110.0f, 150, 0);          // establish (2.5 s)
        const int wide = d.lastHigh();
        ns = d.run(noiseSpectrum(-110.0f), -110.0f, 30, ns);  // 0.5 s gap
        d.run(sig, -110.0f, 30, ns);
        char det[96];
        std::snprintf(det, sizeof det, "  wide=%d afterGap=%d", wide, d.lastHigh());
        report("word-gap HOLD: fit not collapsed by a 0.5 s gap",
               wide >= 3800 && d.lastHigh() >= 3600, det);
    }

    // ── 4. QSO handoff: A (4 kHz) -> 1.2 s gap -> B (3.5 kHz) tracks B ───────
    // The reported bug: operator A's width stayed applied to operator B. After
    // the between-overs flush the returning operator must re-fit to its own
    // width within ~2 s, not inherit A's.
    {
        Driver d(0, 2700);
        const auto a = humpSpectrum(-110.0f, 300.0, 4000.0, -70.0f);
        const auto b = humpSpectrum(-110.0f, 300.0, 3500.0, -70.0f);
        qint64 ns = d.run(a, -110.0f, 180, 0);            // A establishes (3 s)
        const int aHigh = d.lastHigh();
        ns = d.run(noiseSpectrum(-110.0f), -110.0f, 72, ns);  // 1.2 s PTT gap
        d.run(b, -110.0f, 150, ns);                       // B for 2.5 s
        char det[112];
        std::snprintf(det, sizeof det, "  A.high=%d B.high=%d", aHigh, d.lastHigh());
        report("QSO handoff: B (3.5k) not stuck on A's (4k) width",
               aHigh >= 3800 && d.lastHigh() <= 3700, det);
    }

    // ── 5. Manual filter edit auto-disables AUTO (RFC #3878 cond. 3) ─────────
    // A genuine setFilterWidth on a settled station hands control back.
    {
        Driver d(0, 2700);
        const auto sig = humpSpectrum(-110.0f, 300.0, 3000.0, -70.0f);
        d.run(sig, -110.0f, 120, 0);
        const bool onBefore = d.slice.adaptiveFilterEnabled();
        d.slice.setFilterWidth(0, 2400);    // operator drags the filter
        d.run(sig, -110.0f, 30, 120 * kFps60Ns);
        char det[96];
        std::snprintf(det, sizeof det, "  before=%d after=%d",
                      onBefore ? 1 : 0, d.slice.adaptiveFilterEnabled() ? 1 : 0);
        report("manual edit auto-disables adaptive",
               onBefore && !d.slice.adaptiveFilterEnabled(), det);
    }

    std::printf("\n%s (%d failure%s)\n",
                g_failed ? "FAILED" : "PASSED", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
