// Unit tests for WaveformScopeModel — the WAVE-scope incremental-reduction
// core (#3955 review follow-up). Locks down the bin/column reduction math the
// scope render depends on: known-signal peak/RMS/clip, clamping of
// out-of-range and non-finite input, empty-window handling, degenerate column
// counts, and re-bin on reconfigure. Mirrors the coverage the sibling
// reduction core OccupiedRegion ships (tests/adaptive_filter_test.cpp).
//
// Core-only: links against Qt6::Core alone, no GUI/QRhi.

#include "gui/WaveformScopeModel.h"

#include <QVector>

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using AetherSDR::WaveformScopeModel;

static int g_failures = 0;

static void check(bool cond, const char* what)
{
    std::printf("[%s] %s\n", cond ? " OK " : "FAIL", what);
    if (!cond)
        ++g_failures;
}

static bool approx(float a, float b, float eps = 1e-4f)
{
    return std::abs(a - b) <= eps;
}

static void appendConst(WaveformScopeModel& m, float v, int count)
{
    std::vector<float> buf(static_cast<size_t>(count), v);
    m.append(buf.data(), count);
}

int main()
{
    const int sr = 48000;
    const int winMs = 100;                 // 4800-sample window
    const int winSamples = sr * winMs / 1000;

    // ── Constant mid-level signal: exact peak/rms, no clip ───────────────
    {
        WaveformScopeModel m;
        m.configure(sr, winMs);
        appendConst(m, 0.5f, winSamples * 2);   // overfill so the window is full
        const auto ws = m.windowStats();
        check(!ws.empty, "const: window has data");
        check(approx(ws.peak, 0.5f), "const: peak == 0.5");
        check(approx(ws.rms, 0.5f), "const: rms == 0.5");
        check(ws.clipCount == 0, "const: no clip (0.5 < 0.98)");

        QVector<WaveformScopeModel::ColumnStats> cols;
        const auto cws = m.mergeColumns(200, cols);
        check(cols.size() == 200, "const: 200 columns produced");
        check(approx(cws.peak, ws.peak) && approx(cws.rms, ws.rms),
              "const: mergeColumns WindowStats == windowStats");
        bool allHalf = true;
        for (const auto& c : cols) {
            if (!approx(c.min, 0.5f) || !approx(c.max, 0.5f)
                || !approx(c.peak, 0.5f) || !approx(c.rms, 0.5f) || c.clipped != 0)
                allHalf = false;
        }
        check(allHalf, "const: every column min=max=peak=rms=0.5, no clip");
    }

    // ── Clipping: samples at/above the clip threshold are counted ────────
    {
        WaveformScopeModel m;
        m.configure(sr, winMs);
        appendConst(m, 1.0f, winSamples * 2);   // 1.0 >= 0.98 => clipped
        const auto ws = m.windowStats();
        check(approx(ws.peak, 1.0f), "clip: peak == 1.0");
        check(approx(ws.rms, 1.0f), "clip: rms == 1.0");
        check(ws.clipCount > 0, "clip: clipCount > 0");
    }

    // ── Out-of-range and non-finite input is clamped / zeroed ────────────
    {
        WaveformScopeModel m;
        m.configure(sr, winMs);
        appendConst(m, 5.0f, winSamples);       // clamps to +1.0
        check(approx(m.windowStats().peak, 1.0f), "clamp: +5.0 clamps to peak 1.0");

        WaveformScopeModel m2;
        m2.configure(sr, winMs);
        std::vector<float> bad(static_cast<size_t>(winSamples),
                               std::numeric_limits<float>::quiet_NaN());
        bad[0] = std::numeric_limits<float>::infinity();
        m2.append(bad.data(), winSamples);
        const auto ws2 = m2.windowStats();
        check(std::isfinite(ws2.peak) && std::isfinite(ws2.rms),
              "clamp: NaN/inf -> finite stats");
        check(approx(ws2.peak, 0.0f) && approx(ws2.rms, 0.0f),
              "clamp: NaN/inf treated as 0");
    }

    // ── All-zero signal ──────────────────────────────────────────────────
    {
        WaveformScopeModel m;
        m.configure(sr, winMs);
        appendConst(m, 0.0f, winSamples);
        const auto ws = m.windowStats();
        check(approx(ws.peak, 0.0f) && approx(ws.rms, 0.0f) && ws.clipCount == 0,
              "zero: peak=rms=0, no clip");
    }

    // ── Empty model: no data, no crash ───────────────────────────────────
    {
        WaveformScopeModel m;
        m.configure(sr, winMs);
        QVector<WaveformScopeModel::ColumnStats> cols;
        check(m.mergeColumns(64, cols).empty, "empty: mergeColumns WindowStats.empty");
        check(m.windowStats().empty, "empty: windowStats().empty");
    }

    // ── Degenerate column counts are safe ────────────────────────────────
    {
        WaveformScopeModel m;
        m.configure(sr, winMs);
        appendConst(m, 0.3f, winSamples);
        QVector<WaveformScopeModel::ColumnStats> cols;
        const auto ws0 = m.mergeColumns(0, cols);
        check(ws0.empty && cols.isEmpty(), "cols=0: empty result, no crash");
        m.mergeColumns(1, cols);
        check(cols.size() == 1 && approx(cols[0].peak, 0.3f),
              "cols=1: single column captures the signal");
    }

    // ── Reconfigure (window change) re-bins without losing sanity ────────
    {
        WaveformScopeModel m;
        m.configure(sr, winMs);
        appendConst(m, 0.4f, winSamples);
        m.configure(sr, winMs * 4);             // widen the window; re-bin from raw
        appendConst(m, 0.4f, winSamples * 4);
        const auto ws = m.windowStats();
        check(approx(ws.peak, 0.4f) && approx(ws.rms, 0.4f),
              "reconfigure: stats stay correct after window change");
    }

    // ── setMaxWindowMs retains history for a later widen (#3955) ─────────
    // The raw ring is sized to the max window, not the current one, so
    // widening reveals already-captured history instead of a blank plot.
    // Capture a distinguishing early burst (0.8 for 1 s) followed by 3 s of
    // 0.2 while displaying a narrow 100 ms window, then widen to 4 s: the
    // 0.8 burst — 3 s old, far outside the current-window+1 s fallback ring —
    // must survive. Without setMaxWindowMs it would have been overwritten and
    // the widened window would peak at 0.2.
    {
        WaveformScopeModel m;
        m.setMaxWindowMs(5000);          // 5 s ceiling
        m.configure(sr, 100);            // display a narrow 100 ms window
        appendConst(m, 0.8f, sr);        // 1 s of 0.8 (the old burst)
        appendConst(m, 0.2f, sr * 3);    // then 3 s of 0.2
        m.configure(sr, 4000);           // widen to a 4 s window → re-bin from raw
        const auto ws = m.windowStats();
        check(!ws.empty, "maxwindow: widened window has data");
        check(approx(ws.peak, 0.8f),
              "maxwindow: 3 s-old 0.8 burst survives the widen");
        QVector<WaveformScopeModel::ColumnStats> cols;
        m.mergeColumns(64, cols);
        check(cols.size() == 64 && approx(cols[0].peak, 0.8f),
              "maxwindow: oldest column reflects the retained 0.8 burst");
    }

    std::printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
