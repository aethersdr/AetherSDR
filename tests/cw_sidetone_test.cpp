#include "core/CwSidetoneGenerator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace MasterSDR;

namespace {

bool expect(bool cond, const char* label)
{
    std::printf("%s %s\n", cond ? "[ OK ]" : "[FAIL]", label);
    return cond;
}

double rms(const std::vector<float>& buf)
{
    if (buf.empty()) return 0.0;
    double s = 0.0;
    for (float v : buf) s += static_cast<double>(v) * v;
    return std::sqrt(s / buf.size());
}

float maxAbs(const std::vector<float>& buf)
{
    float m = 0.0f;
    for (float v : buf) m = std::max(m, std::abs(v));
    return m;
}

// Run the generator for `frames` samples, returning the L channel only.
std::vector<float> runFrames(CwSidetoneGenerator& gen, int frames)
{
    std::vector<float> stereo(frames * 2, 0.0f);
    gen.process(stereo.data(), frames);
    std::vector<float> mono(frames);
    for (int i = 0; i < frames; ++i) mono[i] = stereo[2 * i];
    return mono;
}

} // namespace

int main()
{
    bool ok = true;

    // ── 1. Disabled generator emits nothing ─────────────────────────────────
    {
        CwSidetoneGenerator gen(48000);
        gen.setEnabled(false);
        gen.setKeyDown(true);
        auto mono = runFrames(gen, 480);  // 10 ms
        ok &= expect(maxAbs(mono) == 0.0f, "disabled generator emits silence");
    }

    // ── 2. Enabled, key down → produces a tone ──────────────────────────────
    {
        CwSidetoneGenerator gen(48000);
        gen.setEnabled(true);
        gen.setVolume(1.0f);
        gen.setShapingMs(0.0f);  // hard keying for this test
        gen.setKeyDown(true);
        auto mono = runFrames(gen, 480);
        ok &= expect(rms(mono) > 0.3, "key-down at full volume produces tone");
        ok &= expect(maxAbs(mono) <= 1.0f, "tone never clips");
    }

    // ── 3. Pitch is approximately correct ───────────────────────────────────
    {
        CwSidetoneGenerator gen(48000);
        gen.setEnabled(true);
        gen.setVolume(1.0f);
        gen.setShapingMs(0.0f);
        gen.setPitchHz(600.0f);
        gen.setKeyDown(true);
        auto mono = runFrames(gen, 4800);  // 100 ms

        // Count zero crossings — for a 600 Hz tone over 100 ms, expect 120
        // crossings (60 cycles × 2).  Allow ±2 for boundary effects.
        int crossings = 0;
        for (size_t i = 1; i < mono.size(); ++i) {
            if ((mono[i - 1] < 0.0f && mono[i] >= 0.0f) ||
                (mono[i - 1] >= 0.0f && mono[i] < 0.0f))
                ++crossings;
        }
        ok &= expect(std::abs(crossings - 120) <= 2,
                     "600 Hz pitch yields ~120 zero-crossings in 100 ms");
    }

    // ── 4. Volume scaling ──────────────────────────────────────────────────
    {
        CwSidetoneGenerator gen(48000);
        gen.setEnabled(true);
        gen.setShapingMs(0.0f);
        gen.setKeyDown(true);

        gen.setVolume(1.0f);
        const double rmsFull = rms(runFrames(gen, 480));

        gen.setVolume(0.5f);
        const double rmsHalf = rms(runFrames(gen, 480));

        // Half volume → roughly half RMS (within 5% tolerance for phase
        // continuity across the runs).
        const double ratio = rmsHalf / rmsFull;
        ok &= expect(ratio > 0.45 && ratio < 0.55,
                     "volume 0.5 produces ~half RMS of volume 1.0");
    }

    // ── 5. Raised-cosine envelope: no instant jump from 0 to peak ──────────
    {
        CwSidetoneGenerator gen(48000);
        gen.setEnabled(true);
        gen.setVolume(1.0f);
        gen.setPan(0.0f);        // test reads L channel only
        gen.setShapingMs(5.0f);  // 5 ms ramp
        gen.setKeyDown(true);
        auto mono = runFrames(gen, 480);  // 10 ms total — first 5 ms is ramp

        // Check peak over a small window — single-sample checks land
        // arbitrarily on the sine wave so use windowed maxAbs instead.
        const auto peakIn = [&](int begin, int end) {
            float m = 0.0f;
            for (int i = begin; i < end && i < (int)mono.size(); ++i)
                m = std::max(m, std::abs(mono[i]));
            return m;
        };
        const float startPeak = peakIn(0, 24);   // first 0.5 ms of ramp
        const float endPeak   = peakIn(240, 480); // post-ramp sustain
        ok &= expect(startPeak < 0.05f,
                     "raised-cosine ramp starts near zero");
        ok &= expect(endPeak > 0.9f,
                     "envelope reaches full amplitude after 5 ms ramp");
    }

    // ── 6. Key-up ramps down to silence ────────────────────────────────────
    {
        CwSidetoneGenerator gen(48000);
        gen.setEnabled(true);
        gen.setVolume(1.0f);
        gen.setShapingMs(5.0f);
        gen.setKeyDown(true);
        runFrames(gen, 4800);  // sustain for 100 ms

        gen.setKeyDown(false);
        auto rampDown = runFrames(gen, 480);  // 10 ms

        // First samples loud (mid-tone), last samples near zero.
        const float startAmp = maxAbs(std::vector<float>(
            rampDown.begin(), rampDown.begin() + 24));  // first 0.5 ms
        const float endAmp = maxAbs(std::vector<float>(
            rampDown.end() - 24, rampDown.end()));     // last 0.5 ms
        ok &= expect(startAmp > 0.5f, "ramp-down starts at full amplitude");
        ok &= expect(endAmp < 0.05f, "ramp-down ends near silence");
    }

    // ── 7. Reset returns to idle state ─────────────────────────────────────
    {
        CwSidetoneGenerator gen(48000);
        gen.setEnabled(true);
        gen.setVolume(1.0f);
        gen.setShapingMs(0.0f);
        gen.setKeyDown(true);
        runFrames(gen, 480);
        gen.reset();
        gen.setKeyDown(false);
        auto silent = runFrames(gen, 480);
        ok &= expect(maxAbs(silent) == 0.0f, "reset + key-up produces silence");
    }

    return ok ? 0 : 1;
}
