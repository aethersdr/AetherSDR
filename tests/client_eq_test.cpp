// Standalone test harness for ClientEq DSP.
// Build: produced by CMake as `client_eq_test` target (Debug or RelWithDebInfo).
// Run:   ./build/client_eq_test
// Exit code 0 on all pass, 1 on any failure.

#include "core/ClientEq.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using AetherSDR::ClientEq;

namespace {

constexpr double kSampleRate = 24000.0;
constexpr int    kBlockSize  = 128;
constexpr int    kTestLenSec = 1;

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-48s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail.c_str());
    if (!ok) ++g_failed;
}

// Generate a unit-amplitude stereo sine tone at `freq` Hz.
std::vector<float> makeTone(double freq, int frames, float amplitude = 0.5f)
{
    std::vector<float> buf(frames * 2);
    const double twoPi = 6.283185307179586476;
    for (int i = 0; i < frames; ++i) {
        const float s = amplitude * static_cast<float>(
            std::sin(twoPi * freq * i / kSampleRate));
        buf[i * 2]     = s;
        buf[i * 2 + 1] = s;
    }
    return buf;
}

float rmsStereo(const float* data, int frames)
{
    double sumSq = 0.0;
    const int samples = frames * 2;
    for (int i = 0; i < samples; ++i) {
        sumSq += static_cast<double>(data[i]) * data[i];
    }
    return static_cast<float>(std::sqrt(sumSq / samples));
}

float peakAbsStereo(const float* data, int frames)
{
    float peak = 0.0f;
    const int samples = frames * 2;
    for (int i = 0; i < samples; ++i) {
        peak = std::max(peak, std::fabs(data[i]));
    }
    return peak;
}

// Process a buffer block-by-block, so we exercise the same block-scheduling
// that the AudioEngine will use.
void processBlocks(ClientEq& eq, float* buf, int frames)
{
    int remaining = frames;
    float* p = buf;
    while (remaining > 0) {
        const int n = std::min(kBlockSize, remaining);
        eq.process(p, n, 2);
        p += n * 2;
        remaining -= n;
    }
}

bool anyNaNorInf(const float* data, int frames)
{
    const int samples = frames * 2;
    for (int i = 0; i < samples; ++i) {
        if (!std::isfinite(data[i])) return true;
    }
    return false;
}

// ── Tests ──────────────────────────────────────────────────────────────

void testBypass()
{
    ClientEq eq;
    eq.prepare(kSampleRate);
    // Enabled but no active bands -> must be a pure pass-through
    eq.setEnabled(true);
    eq.setActiveBandCount(0);

    const int frames = static_cast<int>(kSampleRate) * kTestLenSec;
    auto buf = makeTone(1000.0, frames);
    const float rmsBefore = rmsStereo(buf.data(), frames);
    processBlocks(eq, buf.data(), frames);
    const float rmsAfter  = rmsStereo(buf.data(), frames);

    const float drift = std::fabs(rmsAfter - rmsBefore);
    report("bypass with 0 active bands = pass-through",
           drift < 1e-6f,
           "drift=" + std::to_string(drift));
}

void testDisabledFlag()
{
    ClientEq eq;
    eq.prepare(kSampleRate);
    eq.setEnabled(false);  // global bypass
    eq.setActiveBandCount(1);

    ClientEq::BandParams b;
    b.type   = ClientEq::FilterType::Peak;
    b.freqHz = 1000.0f;
    b.gainDb = 12.0f;
    b.q      = 1.0f;
    b.enabled = true;
    eq.setBand(0, b);

    const int frames = static_cast<int>(kSampleRate);
    auto buf = makeTone(1000.0, frames);
    const float rmsBefore = rmsStereo(buf.data(), frames);
    processBlocks(eq, buf.data(), frames);
    const float rmsAfter  = rmsStereo(buf.data(), frames);

    const float drift = std::fabs(rmsAfter - rmsBefore);
    report("global disable = bypass regardless of bands",
           drift < 1e-6f,
           "drift=" + std::to_string(drift));
}

// Drive a 1 kHz tone through a +12 dB peak at 1 kHz, Q=1. Expected gain
// should approach 10^(12/20) = 3.981 (actual depends on Q; we allow a
// wider tolerance because the filter's gain at its centre freq is exactly
// the specified dB for peaking EQ, but only after the filter settles).
void testPeakGain()
{
    ClientEq eq;
    eq.prepare(kSampleRate);
    eq.setEnabled(true);
    eq.setActiveBandCount(1);

    ClientEq::BandParams b;
    b.type    = ClientEq::FilterType::Peak;
    b.freqHz  = 1000.0f;
    b.gainDb  = 12.0f;
    b.q       = 1.0f;
    b.enabled = true;
    eq.setBand(0, b);

    // Run warmup first so the smoother + filter state settle before we
    // measure. 2s of warmup covers the 15ms smoother × 100× safety factor.
    const int warmFrames = static_cast<int>(kSampleRate) * 2;
    auto warm = makeTone(1000.0, warmFrames);
    processBlocks(eq, warm.data(), warmFrames);

    const int frames = static_cast<int>(kSampleRate);
    auto buf = makeTone(1000.0, frames);
    const float rmsBefore = rmsStereo(buf.data(), frames);
    processBlocks(eq, buf.data(), frames);
    const float rmsAfter  = rmsStereo(buf.data(), frames);

    const float expectedGain = std::pow(10.0f, 12.0f / 20.0f);   // ≈ 3.981
    const float measuredGain = rmsAfter / rmsBefore;
    const float errDb        = 20.0f * std::log10(measuredGain / expectedGain);

    report("+12 dB peak at centre freq matches target within 0.5 dB",
           std::fabs(errDb) < 0.5f,
           "measured=" + std::to_string(20.0f * std::log10(measuredGain))
           + " dB, error=" + std::to_string(errDb) + " dB");
}

// Cut (-12 dB) should attenuate symmetrically.
void testPeakCut()
{
    ClientEq eq;
    eq.prepare(kSampleRate);
    eq.setEnabled(true);
    eq.setActiveBandCount(1);

    ClientEq::BandParams b;
    b.type    = ClientEq::FilterType::Peak;
    b.freqHz  = 1000.0f;
    b.gainDb  = -12.0f;
    b.q       = 1.0f;
    b.enabled = true;
    eq.setBand(0, b);

    const int warmFrames = static_cast<int>(kSampleRate) * 2;
    auto warm = makeTone(1000.0, warmFrames);
    processBlocks(eq, warm.data(), warmFrames);

    const int frames = static_cast<int>(kSampleRate);
    auto buf = makeTone(1000.0, frames);
    const float rmsBefore = rmsStereo(buf.data(), frames);
    processBlocks(eq, buf.data(), frames);
    const float rmsAfter  = rmsStereo(buf.data(), frames);

    const float expectedGain = std::pow(10.0f, -12.0f / 20.0f);  // ≈ 0.251
    const float measuredGain = rmsAfter / rmsBefore;
    const float errDb        = 20.0f * std::log10(measuredGain / expectedGain);

    report("-12 dB peak at centre freq matches target within 0.5 dB",
           std::fabs(errDb) < 0.5f,
           "measured=" + std::to_string(20.0f * std::log10(measuredGain))
           + " dB, error=" + std::to_string(errDb) + " dB");
}

// A low-pass well below the stimulus freq should strongly attenuate.
void testLowPassAttenuation()
{
    ClientEq eq;
    eq.prepare(kSampleRate);
    eq.setEnabled(true);
    eq.setActiveBandCount(1);

    ClientEq::BandParams b;
    b.type    = ClientEq::FilterType::LowPass;
    b.freqHz  = 500.0f;
    b.q       = 0.707f;
    b.enabled = true;
    eq.setBand(0, b);

    const int warmFrames = static_cast<int>(kSampleRate) * 2;
    auto warm = makeTone(4000.0, warmFrames);
    processBlocks(eq, warm.data(), warmFrames);

    const int frames = static_cast<int>(kSampleRate);
    auto buf = makeTone(4000.0, frames);
    const float rmsBefore = rmsStereo(buf.data(), frames);
    processBlocks(eq, buf.data(), frames);
    const float rmsAfter  = rmsStereo(buf.data(), frames);

    const float attenDb = 20.0f * std::log10(rmsAfter / rmsBefore);
    // 4 kHz is 3 octaves above 500 Hz cutoff. A 2nd-order LP gives
    // -12 dB/oct = -36 dB ideal, with some Q rise near cutoff. Expect
    // at least -30 dB.
    report("2nd-order LP at 500Hz attenuates 4kHz by >30 dB",
           attenDb < -30.0f,
           "attenuation=" + std::to_string(attenDb) + " dB");
}

void testHighPassAttenuation()
{
    ClientEq eq;
    eq.prepare(kSampleRate);
    eq.setEnabled(true);
    eq.setActiveBandCount(1);

    ClientEq::BandParams b;
    b.type    = ClientEq::FilterType::HighPass;
    b.freqHz  = 1000.0f;
    b.q       = 0.707f;
    b.enabled = true;
    eq.setBand(0, b);

    const int warmFrames = static_cast<int>(kSampleRate) * 2;
    auto warm = makeTone(125.0, warmFrames);
    processBlocks(eq, warm.data(), warmFrames);

    const int frames = static_cast<int>(kSampleRate);
    auto buf = makeTone(125.0, frames);
    const float rmsBefore = rmsStereo(buf.data(), frames);
    processBlocks(eq, buf.data(), frames);
    const float rmsAfter  = rmsStereo(buf.data(), frames);

    const float attenDb = 20.0f * std::log10(rmsAfter / rmsBefore);
    // 125 Hz is 3 octaves below 1 kHz cutoff → ≈ -36 dB ideal.
    report("2nd-order HP at 1kHz attenuates 125Hz by >30 dB",
           attenDb < -30.0f,
           "attenuation=" + std::to_string(attenDb) + " dB");
}

// Zipper-noise smoke test: sweep centre frequency during playback and
// make sure no NaN/inf escapes and peak doesn't explode.
void testSweepNoClick()
{
    ClientEq eq;
    eq.prepare(kSampleRate);
    eq.setEnabled(true);
    eq.setActiveBandCount(1);

    ClientEq::BandParams b;
    b.type    = ClientEq::FilterType::Peak;
    b.freqHz  = 500.0f;
    b.gainDb  = 6.0f;
    b.q       = 2.0f;
    b.enabled = true;
    eq.setBand(0, b);

    const int frames = static_cast<int>(kSampleRate);
    auto buf = makeTone(1000.0, frames);

    // Sweep 100 → 10000 Hz logarithmically over the 1-second buffer,
    // stepping between blocks. This is the worst case a user slamming
    // a slider around would produce.
    int pos = 0;
    int steps = 0;
    while (pos < frames) {
        const int n = std::min(kBlockSize, frames - pos);
        const float t = static_cast<float>(pos) / frames;  // 0..1
        const float newFreq = 100.0f * std::pow(100.0f, t);
        b.freqHz = newFreq;
        eq.setBand(0, b);
        eq.process(buf.data() + pos * 2, n, 2);
        pos += n;
        ++steps;
    }

    const bool finite = !anyNaNorInf(buf.data(), frames);
    const float peak = peakAbsStereo(buf.data(), frames);
    // +6 dB peaking with Q=2 and 0.5 amplitude input should produce at
    // most ~1.5× peak. Anything beyond 2.0 is a runaway.
    report("sweep 100→10kHz produces finite output",
           finite,
           "steps=" + std::to_string(steps));
    report("sweep 100→10kHz bounded peak (<2.0)",
           peak < 2.0f,
           "peak=" + std::to_string(peak));
}

// Chaining: multiple peaks at different freqs should produce (approximately)
// additive dB gain at frequencies where they overlap.
void testBandCascade()
{
    ClientEq eq;
    eq.prepare(kSampleRate);
    eq.setEnabled(true);
    eq.setActiveBandCount(3);

    ClientEq::BandParams b{};
    b.type = ClientEq::FilterType::Peak;
    b.q    = 1.0f;
    b.enabled = true;
    b.freqHz = 300.0f;  b.gainDb = 3.0f;  eq.setBand(0, b);
    b.freqHz = 1000.0f; b.gainDb = 3.0f;  eq.setBand(1, b);
    b.freqHz = 3000.0f; b.gainDb = 3.0f;  eq.setBand(2, b);

    const int warmFrames = static_cast<int>(kSampleRate) * 2;
    auto warm = makeTone(1000.0, warmFrames);
    processBlocks(eq, warm.data(), warmFrames);

    const int frames = static_cast<int>(kSampleRate);
    auto buf = makeTone(1000.0, frames);
    const float rmsBefore = rmsStereo(buf.data(), frames);
    processBlocks(eq, buf.data(), frames);
    const float rmsAfter  = rmsStereo(buf.data(), frames);
    const float gainDb    = 20.0f * std::log10(rmsAfter / rmsBefore);

    // At 1 kHz the middle band contributes ~+3 dB. The 300 Hz and 3 kHz
    // bands overlap only marginally at Q=1, so total should be ≈ 3 dB
    // + small side contribution. Expect 3.0 – 4.5 dB.
    report("three peaks at 300/1000/3000, probe @ 1kHz, ~+3 dB",
           gainDb > 2.5f && gainDb < 4.5f,
           "measured=" + std::to_string(gainDb) + " dB");
}

// Disabled band in an active slot = pass-through (not a processor error).
void testPerBandEnable()
{
    ClientEq eq;
    eq.prepare(kSampleRate);
    eq.setEnabled(true);
    eq.setActiveBandCount(1);

    ClientEq::BandParams b;
    b.type    = ClientEq::FilterType::Peak;
    b.freqHz  = 1000.0f;
    b.gainDb  = 12.0f;
    b.q       = 1.0f;
    b.enabled = false;   // ← per-band disable
    eq.setBand(0, b);

    // Warmup even though the band is disabled, just to surface any issues
    // with version tracking.
    const int warmFrames = static_cast<int>(kSampleRate) * 1;
    auto warm = makeTone(1000.0, warmFrames);
    processBlocks(eq, warm.data(), warmFrames);

    const int frames = static_cast<int>(kSampleRate);
    auto buf = makeTone(1000.0, frames);
    const float rmsBefore = rmsStereo(buf.data(), frames);
    processBlocks(eq, buf.data(), frames);
    const float rmsAfter  = rmsStereo(buf.data(), frames);

    const float drift = std::fabs(rmsAfter - rmsBefore);
    report("per-band disabled = pass-through",
           drift < 1e-4f,
           "drift=" + std::to_string(drift));
}

} // namespace

int main()
{
    std::printf("ClientEq DSP test harness (fs=%.0f Hz, block=%d)\n\n",
                kSampleRate, kBlockSize);

    testBypass();
    testDisabledFlag();
    testPeakGain();
    testPeakCut();
    testLowPassAttenuation();
    testHighPassAttenuation();
    testSweepNoClick();
    testBandCascade();
    testPerBandEnable();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : (std::to_string(g_failed) + " test(s) failed.").c_str());
    return g_failed == 0 ? 0 : 1;
}
