// Standalone test harness for ClientComp DSP.
// Build: produced by CMake as `client_comp_test` target.
// Run:   ./build/client_comp_test
// Exit code 0 on all pass, 1 on any failure.

#include "core/ClientComp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using AetherSDR::ClientComp;

namespace {

constexpr double kSampleRate = 24000.0;
constexpr int    kBlockSize  = 128;

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-52s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail.c_str());
    if (!ok) ++g_failed;
}

std::vector<float> makeTone(double freq, int frames, float amplitude)
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

void processBlocks(ClientComp& comp, float* buf, int frames)
{
    int remaining = frames;
    float* p = buf;
    while (remaining > 0) {
        const int n = std::min(kBlockSize, remaining);
        comp.process(p, n, 2);
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

float linToDb(float lin)
{
    return (lin > 1e-9f) ? 20.0f * std::log10(lin) : -180.0f;
}

// ── Tests ──────────────────────────────────────────────────────────────

void testBypassDisabled()
{
    ClientComp comp;
    comp.prepare(kSampleRate);
    comp.setEnabled(false);
    comp.setLimiterEnabled(false);

    const int frames = static_cast<int>(kSampleRate);
    auto buf = makeTone(1000.0, frames, 0.5f);
    const float rmsBefore = rmsStereo(buf.data(), frames);
    processBlocks(comp, buf.data(), frames);
    const float rmsAfter  = rmsStereo(buf.data(), frames);

    const float drift = std::fabs(rmsAfter - rmsBefore);
    report("disabled comp + disabled limiter = pass-through",
           drift < 1e-6f,
           "drift=" + std::to_string(drift));
}

// With input well below threshold, comp+limiter should leave signal alone.
void testBelowThreshold()
{
    ClientComp comp;
    comp.prepare(kSampleRate);
    comp.setEnabled(true);
    comp.setThresholdDb(-20.0f);
    comp.setRatio(4.0f);
    comp.setKneeDb(0.0f);  // hard knee so below-threshold is pure passthrough
    comp.setMakeupDb(0.0f);
    comp.setLimiterEnabled(true);
    comp.setLimiterCeilingDb(-1.0f);

    // -40 dBFS input (amplitude 0.01), well below -20 dBFS threshold.
    const int frames = static_cast<int>(kSampleRate);
    auto buf = makeTone(1000.0, frames, 0.01f);

    // Warmup — the envelope starts at -120 dBFS and has to climb, but
    // with the signal already below threshold we don't get compression.
    const int warm = static_cast<int>(kSampleRate) * 2;
    auto warmBuf = makeTone(1000.0, warm, 0.01f);
    processBlocks(comp, warmBuf.data(), warm);

    const float rmsBefore = rmsStereo(buf.data(), frames);
    processBlocks(comp, buf.data(), frames);
    const float rmsAfter  = rmsStereo(buf.data(), frames);

    const float drift = std::fabs(rmsAfter - rmsBefore) / rmsBefore;
    report("below-threshold signal is pass-through",
           drift < 0.01f,
           "relative drift=" + std::to_string(drift));
}

// 4:1 ratio, hard knee, threshold -20. Drive at -10 dBFS peak. Expected
// output peak: -20 + (10/4) = -17.5 dBFS steady-state.
// Uses long attack/release so the log-domain envelope settles to the
// peak instead of riding the 1kHz sine waveform.
void testStaticRatio()
{
    ClientComp comp;
    comp.prepare(kSampleRate);
    comp.setEnabled(true);
    comp.setThresholdDb(-20.0f);
    comp.setRatio(4.0f);
    comp.setKneeDb(0.0f);
    comp.setMakeupDb(0.0f);
    comp.setAttackMs(50.0f);
    comp.setReleaseMs(500.0f);
    comp.setLimiterEnabled(false);

    const float amp = std::pow(10.0f, -10.0f / 20.0f);  // -10 dBFS peak

    // Warmup: envelope τ = 500ms, settle over 3s.
    const int warm = static_cast<int>(kSampleRate) * 3;
    auto warmBuf = makeTone(1000.0, warm, amp);
    processBlocks(comp, warmBuf.data(), warm);

    const int frames = static_cast<int>(kSampleRate);
    auto buf = makeTone(1000.0, frames, amp);
    processBlocks(comp, buf.data(), frames);

    const float measPeak  = peakAbsStereo(buf.data(), frames);
    const float measDbfs  = linToDb(measPeak);
    const float errDb     = measDbfs - (-17.5f);
    report("4:1 ratio above threshold: -10 dBFS in -> -17.5 dBFS out",
           std::fabs(errDb) < 1.0f,
           "measured=" + std::to_string(measDbfs) + " dBFS, err="
               + std::to_string(errDb) + " dB");
}

// High ratio (20:1) clamps near threshold.
void testLimitRatio()
{
    ClientComp comp;
    comp.prepare(kSampleRate);
    comp.setEnabled(true);
    comp.setThresholdDb(-12.0f);
    comp.setRatio(20.0f);
    comp.setKneeDb(0.0f);
    comp.setMakeupDb(0.0f);
    comp.setAttackMs(50.0f);
    comp.setReleaseMs(500.0f);
    comp.setLimiterEnabled(false);

    const float amp = std::pow(10.0f, -3.0f / 20.0f);  // -3 dBFS peak
    const int warm = static_cast<int>(kSampleRate) * 3;
    auto warmBuf = makeTone(1000.0, warm, amp);
    processBlocks(comp, warmBuf.data(), warm);

    const int frames = static_cast<int>(kSampleRate);
    auto buf = makeTone(1000.0, frames, amp);
    processBlocks(comp, buf.data(), frames);

    const float measPeak = peakAbsStereo(buf.data(), frames);
    const float measDbfs = linToDb(measPeak);
    const float expected = -12.0f + 9.0f / 20.0f;
    const float errDb    = measDbfs - expected;
    report("20:1 ratio: -3 dBFS in -> near threshold out",
           std::fabs(errDb) < 1.5f,
           "measured=" + std::to_string(measDbfs) + " dBFS, expected="
               + std::to_string(expected));
}

// Makeup gain applied to below-threshold signal = clean linear boost.
void testMakeupGain()
{
    ClientComp comp;
    comp.prepare(kSampleRate);
    comp.setEnabled(true);
    comp.setThresholdDb(-6.0f);   // high threshold: signal stays below it
    comp.setRatio(4.0f);
    comp.setKneeDb(0.0f);
    comp.setMakeupDb(6.0f);        // +6 dB makeup
    comp.setLimiterEnabled(false);

    const float amp = 0.1f;  // low signal
    const int warm = static_cast<int>(kSampleRate) * 1;
    auto warmBuf = makeTone(1000.0, warm, amp);
    processBlocks(comp, warmBuf.data(), warm);

    const int frames = static_cast<int>(kSampleRate);
    auto buf = makeTone(1000.0, frames, amp);
    const float rmsBefore = rmsStereo(buf.data(), frames);
    processBlocks(comp, buf.data(), frames);
    const float rmsAfter  = rmsStereo(buf.data(), frames);

    const float gainDb = 20.0f * std::log10(rmsAfter / rmsBefore);
    report("+6 dB makeup on below-threshold signal",
           std::fabs(gainDb - 6.0f) < 0.5f,
           "gain=" + std::to_string(gainDb) + " dB");
}

// Brickwall limiter must clamp peaks to ceiling even with comp off.
void testLimiterCeiling()
{
    ClientComp comp;
    comp.prepare(kSampleRate);
    comp.setEnabled(false);  // comp off, only limiter active
    comp.setLimiterEnabled(true);
    comp.setLimiterCeilingDb(-6.0f);  // ceiling at 0.5012 linear

    const float ceilingLin = std::pow(10.0f, -6.0f / 20.0f);
    const int frames = static_cast<int>(kSampleRate);
    auto buf = makeTone(1000.0, frames, 0.9f);  // well above ceiling

    // Warmup pass so limiter envelope is primed
    auto warm = makeTone(1000.0, frames, 0.9f);
    processBlocks(comp, warm.data(), frames);

    processBlocks(comp, buf.data(), frames);
    const float peak = peakAbsStereo(buf.data(), frames);

    // Allow a tiny overshoot (≤ +0.5 dB) for limiter settling between sine peaks.
    const float peakDb    = 20.0f * std::log10(peak);
    const float ceilingDb = 20.0f * std::log10(ceilingLin);
    report("limiter clamps 0.9 peak to -6 dBFS ceiling",
           peakDb - ceilingDb < 0.5f,
           "peak=" + std::to_string(peakDb) + " dBFS (ceiling="
               + std::to_string(ceilingDb) + ")");
}

// Stereo linking: max(|L|,|R|) drives envelope, and the same gain is
// applied to both. Feed L-only signal and check R remains silent (no
// cross-channel leakage) but the L channel is still compressed.
void testStereoLinked()
{
    ClientComp comp;
    comp.prepare(kSampleRate);
    comp.setEnabled(true);
    comp.setThresholdDb(-20.0f);
    comp.setRatio(4.0f);
    comp.setKneeDb(0.0f);
    comp.setMakeupDb(0.0f);
    comp.setLimiterEnabled(false);

    const int frames = static_cast<int>(kSampleRate);
    std::vector<float> buf(frames * 2, 0.0f);
    const float amp = 0.5f;
    const double twoPi = 6.283185307179586476;
    for (int i = 0; i < frames; ++i) {
        buf[i * 2]     = amp * static_cast<float>(
            std::sin(twoPi * 1000.0 * i / kSampleRate));
        buf[i * 2 + 1] = 0.0f;  // R silent
    }

    // Warmup
    std::vector<float> warm(frames * 2, 0.0f);
    for (int i = 0; i < frames; ++i) {
        warm[i * 2] = amp * static_cast<float>(
            std::sin(twoPi * 1000.0 * i / kSampleRate));
    }
    processBlocks(comp, warm.data(), frames);

    processBlocks(comp, buf.data(), frames);

    // R channel should stay exactly zero (no cross-talk).
    float rPeak = 0.0f;
    for (int i = 0; i < frames; ++i) {
        rPeak = std::max(rPeak, std::fabs(buf[i * 2 + 1]));
    }
    // L channel should show compression (< input amplitude).
    float lPeak = 0.0f;
    for (int i = 0; i < frames; ++i) {
        lPeak = std::max(lPeak, std::fabs(buf[i * 2]));
    }

    report("stereo-linked: R stays silent when only L is active",
           rPeak < 1e-6f,
           "R peak=" + std::to_string(rPeak));
    report("stereo-linked: L channel still compressed",
           lPeak < amp * 0.95f,
           "L peak=" + std::to_string(lPeak)
               + " (input=" + std::to_string(amp) + ")");
}

// Attack timing: envelope should reach ~63 % of step in τ ms.
// Drive silence → 0.5 amp step, sample GR shortly after the step.
void testAttackTiming()
{
    ClientComp comp;
    comp.prepare(kSampleRate);
    comp.setEnabled(true);
    comp.setThresholdDb(-20.0f);
    comp.setRatio(10.0f);
    comp.setKneeDb(0.0f);
    comp.setMakeupDb(0.0f);
    comp.setAttackMs(20.0f);
    comp.setReleaseMs(500.0f);
    comp.setLimiterEnabled(false);
    comp.reset();

    // 20ms of silence, then loud tone burst.
    const int preFrames = static_cast<int>(kSampleRate * 0.020);
    const int burstFrames = static_cast<int>(kSampleRate * 0.200);

    std::vector<float> pre(preFrames * 2, 0.0f);
    processBlocks(comp, pre.data(), preFrames);

    auto burst = makeTone(1000.0, burstFrames, 0.5f);
    processBlocks(comp, burst.data(), burstFrames);

    // After 200ms (10× attack time constant) gain reduction should be
    // close to the steady-state value. For 0.5 amp = -6 dBFS with
    // threshold -20, ratio 10: overshoot 14 dB / ratio 10 = 1.4 dB
    // above threshold, so GR = 14 - 1.4 = 12.6 dB.
    const float gr = comp.gainReductionDb();
    report("attack reaches steady-state after 10τ",
           gr < -10.0f && gr > -14.0f,
           "GR=" + std::to_string(gr) + " dB");
}

// Soft-knee continuity: sweep the static curve and verify no discontinuity.
void testSoftKneeMonotonic()
{
    ClientComp comp;
    comp.prepare(kSampleRate);
    comp.setEnabled(true);
    comp.setThresholdDb(-18.0f);
    comp.setRatio(4.0f);
    comp.setKneeDb(12.0f);  // wide knee
    comp.setMakeupDb(0.0f);
    comp.setAttackMs(0.1f);
    comp.setReleaseMs(5.0f);
    comp.setLimiterEnabled(false);

    // Sweep amplitude from -40 dBFS to 0 dBFS in fine steps, measure RMS
    // gain at each. Must be monotonically decreasing (compression grows).
    float lastGainDb = 999.0f;
    bool monotonic = true;
    float worstJump = 0.0f;

    for (int dbfs = -40; dbfs <= 0; dbfs += 2) {
        comp.reset();
        const float amp = std::pow(10.0f, dbfs / 20.0f);
        const int warm = static_cast<int>(kSampleRate * 0.5);
        auto warmBuf = makeTone(1000.0, warm, amp);
        processBlocks(comp, warmBuf.data(), warm);

        const int frames = static_cast<int>(kSampleRate * 0.1);
        auto buf = makeTone(1000.0, frames, amp);
        const float rmsBefore = rmsStereo(buf.data(), frames);
        processBlocks(comp, buf.data(), frames);
        const float rmsAfter  = rmsStereo(buf.data(), frames);

        const float gainDb = 20.0f * std::log10(rmsAfter / rmsBefore);
        if (gainDb > lastGainDb + 0.1f) {
            monotonic = false;
            const float jump = gainDb - lastGainDb;
            if (jump > worstJump) worstJump = jump;
        }
        lastGainDb = gainDb;
    }
    report("soft-knee: gain curve is monotonically decreasing",
           monotonic,
           "worst jump=" + std::to_string(worstJump) + " dB");
}

// Sanity: no NaN/inf under a nasty transient sequence.
void testTransientSanity()
{
    ClientComp comp;
    comp.prepare(kSampleRate);
    comp.setEnabled(true);
    comp.setThresholdDb(-24.0f);
    comp.setRatio(10.0f);
    comp.setKneeDb(6.0f);
    comp.setMakeupDb(12.0f);
    comp.setAttackMs(5.0f);
    comp.setReleaseMs(100.0f);
    comp.setLimiterEnabled(true);
    comp.setLimiterCeilingDb(-1.0f);

    const int frames = static_cast<int>(kSampleRate) * 2;
    std::vector<float> buf(frames * 2);
    const double twoPi = 6.283185307179586476;
    for (int i = 0; i < frames; ++i) {
        // Amplitude-modulated tone + spikes: stress envelope+limiter.
        const float env = 0.1f + 0.8f
            * static_cast<float>(std::fabs(std::sin(twoPi * 3.0 * i / kSampleRate)));
        const float s = env * static_cast<float>(
            std::sin(twoPi * 1000.0 * i / kSampleRate));
        buf[i * 2]     = s;
        buf[i * 2 + 1] = s;
    }

    processBlocks(comp, buf.data(), frames);

    const bool finite = !anyNaNorInf(buf.data(), frames);
    const float peak = peakAbsStereo(buf.data(), frames);
    report("stress: AM tone output is finite",
           finite,
           "");
    report("stress: output peak respects limiter ceiling (+1 dB headroom)",
           peak < 1.0f,
           "peak=" + std::to_string(peak));
}

// reset() must clear envelope so a new burst starts from silence.
void testResetClearsEnvelope()
{
    ClientComp comp;
    comp.prepare(kSampleRate);
    comp.setEnabled(true);
    comp.setThresholdDb(-20.0f);
    comp.setRatio(4.0f);
    comp.setKneeDb(0.0f);
    comp.setMakeupDb(0.0f);
    comp.setAttackMs(50.0f);
    comp.setReleaseMs(500.0f);
    comp.setLimiterEnabled(false);

    // Drive envelope up
    auto loud = makeTone(1000.0, static_cast<int>(kSampleRate), 0.5f);
    processBlocks(comp, loud.data(), static_cast<int>(kSampleRate));
    const float grBefore = comp.gainReductionDb();

    comp.reset();

    // After reset, the first block of silence should leave GR at ~0.
    std::vector<float> silence(kBlockSize * 2, 0.0f);
    comp.process(silence.data(), kBlockSize, 2);
    const float grAfter = comp.gainReductionDb();

    report("reset() pulls envelope down",
           grBefore < -2.0f && grAfter > -1.0f,
           "before=" + std::to_string(grBefore) + ", after="
               + std::to_string(grAfter));
}

} // namespace

int main()
{
    std::printf("ClientComp DSP test harness (fs=%.0f Hz, block=%d)\n\n",
                kSampleRate, kBlockSize);

    testBypassDisabled();
    testBelowThreshold();
    testStaticRatio();
    testLimitRatio();
    testMakeupGain();
    testLimiterCeiling();
    testStereoLinked();
    testAttackTiming();
    testSoftKneeMonotonic();
    testTransientSanity();
    testResetClearsEnvelope();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : (std::to_string(g_failed) + " test(s) failed.").c_str());
    return g_failed == 0 ? 0 : 1;
}
