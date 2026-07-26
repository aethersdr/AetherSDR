// PanDisplayRateShaper — the coalescer that keeps a raw-spectrum backend's
// render load off the IQ sample rate.
//
// Driven with a SYNTHETIC clock rather than a live radio on purpose. The
// condition being fixed is a 384 kHz HL2 producing 375 frames/second, and
// hpsdrsim does not honour a sample-rate change — it keeps delivering ~40
// frames/second whatever rate is commanded — so the simulator physically cannot
// reproduce the case. A fake clock reproduces it exactly and deterministically.

#include "models/PanDisplayRateShaper.h"

#include <cmath>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

namespace {

constexpr qint64 kNsPerMs = 1000000;

QVector<float> flat(int bins, float dbfs)
{
    QVector<float> v(bins);
    v.fill(dbfs);
    return v;
}

// Run `seconds` of a stream arriving every `inputPeriodMs` into a shaper asking
// for `intervalMs`, and return how many frames it let through per second.
double measuredFps(double inputFrameRate, int intervalMs, double seconds)
{
    PanDisplayRateShaper shaper;
    const QVector<float> bins = flat(64, -100.0f);
    QVector<float> out;
    const auto frames = static_cast<long long>(inputFrameRate * seconds);
    int emitted = 0;
    for (long long i = 0; i < frames; ++i) {
        const auto nowNs =
            static_cast<qint64>(i / inputFrameRate * 1000.0 * kNsPerMs);
        if (shaper.feed(bins, nowNs, intervalMs, out))
            ++emitted;
    }
    return emitted / seconds;
}

}  // namespace

int main()
{
    // ---- the reported bug: 375 fps in, display rate out ----
    //
    // 384000/1024 = 375 frames/second is what an HL2 produces at its widest
    // span; 48000/1024 = 47 at its narrowest. Both must leave at the requested
    // rate, because the whole complaint was that the rate tracked the ZOOM.
    {
        const double wide = measuredFps(375.0, 33, 10.0);    // ~30 fps target
        const double narrow = measuredFps(46.9, 33, 10.0);
        std::fprintf(stderr, "375 fps in -> %.1f out;  47 fps in -> %.1f out\n",
                     wide, narrow);
        CHECK(wide > 28.0 && wide < 32.0);
        CHECK(narrow > 28.0 && narrow < 32.0);

        // THE POINT: the two zoom extremes must agree with each other. A 13x
        // difference in input rate may not survive as any meaningful difference
        // in output rate.
        CHECK(std::abs(wide - narrow) < 3.0);
    }

    // ---- the waterfall's own, slower rate ----
    // 100 ms per row = 10 rows/second, independent of the pan feed. This is the
    // calibration the widget's time axis assumes, so it is correctness, not
    // taste: at 375 fps unshaped the visible history was 37x shorter than the
    // axis claimed.
    {
        const double rows = measuredFps(375.0, 100, 10.0);
        std::fprintf(stderr, "375 fps in -> %.1f waterfall rows/s\n", rows);
        CHECK(rows > 9.0 && rows < 11.0);
    }

    // ---- the rate must not quantize DOWN onto the input grid ----
    //
    // Regression pin. The first cut reset the deadline to "now" on each emit, so
    // the period rounded up by up to one input frame: a 33 ms target fed at
    // 25 ms intervals emitted every 50 ms — 20 fps, not 30. Measured on a live
    // radio before it was caught.
    {
        const double fps = measuredFps(40.0, 33, 10.0);   // 25 ms input period
        std::fprintf(stderr, "40 fps in, 33 ms target -> %.1f out "
                             "(the reset-to-now bug gave 20.0)\n", fps);
        CHECK(fps > 27.0);
    }

    // An input SLOWER than the target cannot be sped up — output tracks input.
    {
        const double fps = measuredFps(12.0, 33, 10.0);
        CHECK(fps > 11.0 && fps < 13.0);
    }

    // ---- power-domain averaging: the noise floor must not move with zoom ----
    //
    // The load-bearing property. Coalescing N frames of a constant level must
    // return that same level for ANY N, or the trace — which is calibrated to
    // dBm — would report a different signal strength at different spans.
    {
        for (const double inputRate : {47.0, 94.0, 188.0, 375.0}) {
            PanDisplayRateShaper shaper;
            const QVector<float> bins = flat(32, -97.5f);
            QVector<float> out;
            bool got = false;
            const auto frames = static_cast<long long>(inputRate * 0.5);
            for (long long i = 0; i < frames; ++i) {
                const auto nowNs =
                    static_cast<qint64>(i / inputRate * 1000.0 * kNsPerMs);
                if (shaper.feed(bins, nowNs, 33, out))
                    got = true;
            }
            CHECK(got);
            if (got) {
                std::fprintf(stderr,
                             "level at %.0f fps in: %.4f dB (input -97.5)\n",
                             inputRate, out[0]);
                CHECK(std::abs(out[0] - (-97.5f)) < 0.01f);
            }
        }
    }

    // Averaging is in POWER, not dB. Two frames at -100 and -90 dB average to
    // 10*log10((1e-10 + 1e-9)/2) = -92.6 dB, NOT the -95 a dB-domain mean would
    // give. The distinction is the whole reason the noise floor stays put.
    {
        PanDisplayRateShaper shaper;
        QVector<float> out;
        // Prime first — the first frame is always let straight through, so it
        // would otherwise consume the -100 frame on its own.
        CHECK(shaper.feed(flat(4, -100.0f), 0, 33, out));
        shaper.feed(flat(4, -100.0f), 10 * kNsPerMs, 33, out);   // not yet due
        const bool emitted = shaper.feed(flat(4, -90.0f), 40 * kNsPerMs, 33, out);
        CHECK(emitted);
        if (emitted) {
            const double expected = 10.0 * std::log10((1e-10 + 1e-9) / 2.0);
            std::fprintf(stderr, "power-mean of -100 and -90 dB: %.4f "
                                 "(expected %.4f, dB-mean would be -95)\n",
                         out[0], expected);
            CHECK(std::abs(out[0] - static_cast<float>(expected)) < 0.01f);
        }
    }

    // ---- a single frame in an interval is passed through untouched ----
    // The narrow-span case. No pow/log10 round trip, so the value is bit-exact.
    {
        PanDisplayRateShaper shaper;
        QVector<float> bins = flat(4, -123.25f);
        bins[2] = -42.75f;
        QVector<float> out;
        CHECK(shaper.feed(bins, 0, 33, out));
        CHECK(out == bins);
    }

    // ---- the first frame is always let through ----
    // The interval is the gap BETWEEN frames, not a delay before the first one:
    // holding it back would leave the panadapter blank for an interval after
    // connect for no benefit.
    {
        PanDisplayRateShaper shaper;
        QVector<float> out;
        CHECK(shaper.feed(flat(4, -100.0f), 0, 33, out));
        // ...and the one right behind it is not.
        CHECK(!shaper.feed(flat(4, -100.0f), 1 * kNsPerMs, 33, out));
    }

    // ---- a bin-count change discards the partial sum ----
    // The operator zoomed and the FFT was rebuilt. Averaging across the change
    // would blend two different frequency grids into one frame.
    {
        PanDisplayRateShaper shaper;
        QVector<float> out;
        shaper.feed(flat(8, -99.0f), 0, 33, out);          // priming emit
        shaper.feed(flat(8, -50.0f), 5 * kNsPerMs, 33, out);   // partial, not due
        const bool emitted =
            shaper.feed(flat(16, -110.0f), 40 * kNsPerMs, 33, out);
        CHECK(emitted);
        CHECK(out.size() == 16);
        // -110, not a blend with the discarded -50 frame.
        CHECK(std::abs(out[0] - (-110.0f)) < 0.01f);
    }

    // ---- intervalMs <= 0 means no shaping, not a stall or a divide by zero ----
    {
        PanDisplayRateShaper shaper;
        QVector<float> out;
        int emitted = 0;
        for (int i = 0; i < 5; ++i)
            if (shaper.feed(flat(4, -100.0f), i * kNsPerMs, 0, out))
                ++emitted;
        CHECK(emitted == 5);
    }

    // ---- the single-frame passthrough is bit-exact ----
    // Not merely close: at narrow spans one frame per interval is the norm, so a
    // pow/log10 round trip there would re-quantize every value the operator sees
    // for no reason at all.
    {
        PanDisplayRateShaper shaper;
        QVector<float> out;
        QVector<float> bins = flat(4, -111.111f);
        bins[1] = -7.3125f;
        CHECK(shaper.feed(bins, 0, 33, out));                       // priming
        CHECK(shaper.feed(bins, 100 * kNsPerMs, 33, out));          // due, n=1
        CHECK(out[0] == bins[0]);
        CHECK(out[1] == bins[1]);
    }

    // ---- a stall does not produce a catch-up burst ----
    //
    // After a gap (a rate change rebuilding the DSP, the app backgrounded) the
    // deadline sits far in the past. Unbounded catch-up would then emit every
    // arriving frame until it caught up — the render spike this exists to
    // prevent. One frame for the stall, then straight back to the target rate.
    {
        PanDisplayRateShaper shaper;
        const QVector<float> bins = flat(8, -100.0f);
        QVector<float> out;
        shaper.feed(bins, 0, 33, out);

        qint64 t = 5000 * kNsPerMs;          // five second stall
        CHECK(shaper.feed(bins, t, 33, out));

        // Now resume a 375 fps stream. If the backlog were being chased, the
        // next ~150 frames would all pass; with the clamp only the ones the
        // 33 ms interval allows do.
        int emitted = 0;
        for (int i = 1; i <= 375; ++i) {
            t += static_cast<qint64>(1000.0 / 375.0 * kNsPerMs);
            if (shaper.feed(bins, t, 33, out))
                ++emitted;
        }
        std::fprintf(stderr, "after a 5 s stall, next second emitted %d frames\n",
                     emitted);
        CHECK(emitted <= 32);
    }

    if (g_failures == 0) {
        std::printf("pan_display_rate_shaper_test: all checks passed\n");
        return 0;
    }
    std::printf("pan_display_rate_shaper_test: %d failure(s)\n", g_failures);
    return 1;
}
