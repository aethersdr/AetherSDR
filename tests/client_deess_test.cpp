// Standalone test harness for ClientDeEss DSP.
// Build: produced by CMake as `client_deess_test` target.
// Run:   ./build/client_deess_test
// Exit code 0 on all pass, 1 on any failure.

#include "core/ClientDeEss.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using AetherSDR::ClientDeEss;

namespace {

constexpr double kSampleRate = 24000.0;
constexpr int    kBlockSize  = 128;

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-54s %s\n",
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

float peakAbsStereo(const float* data, int frames)
{
    float peak = 0.0f;
    const int samples = frames * 2;
    for (int i = 0; i < samples; ++i) {
        peak = std::max(peak, std::fabs(data[i]));
    }
    return peak;
}

void processBlocks(ClientDeEss& d, float* buf, int frames)
{
    int remaining = frames;
    float* p = buf;
    while (remaining > 0) {
        const int n = std::min(kBlockSize, remaining);
        d.process(p, n, 2);
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

float dbToLin(float db)
{
    return std::pow(10.0f, db * 0.05f);
}

// ── Tests ──────────────────────────────────────────────────────────────

void testBypass()
{
    ClientDeEss d;
    d.prepare(kSampleRate);
    d.setEnabled(false);

    const int frames = 1024;
    auto ref = makeTone(5000.0, frames, dbToLin(-6.0f));
    auto out = ref;
    processBlocks(d, out.data(), frames);

    float maxDiff = 0.0f;
    for (size_t i = 0; i < out.size(); ++i)
        maxDiff = std::max(maxDiff, std::fabs(out[i] - ref[i]));

    report("bypass: disabled passes through unchanged",
           maxDiff < 1e-6f,
           "maxDiff=" + std::to_string(maxDiff));
}

// Low-frequency signal (below the sidechain band) should NOT trigger
// attenuation even at loud levels.
void testLowFreqPassthrough()
{
    ClientDeEss d;
    d.prepare(kSampleRate);
    d.setEnabled(true);
    d.setFrequencyHz(6000.0f);
    d.setQ(2.0f);
    d.setThresholdDb(-40.0f);
    d.setAmountDb(-12.0f);
    d.setAttackMs(1.0f);
    d.setReleaseMs(50.0f);

    // 300 Hz at -6 dBFS — well below the sibilant band.
    const float amp = dbToLin(-6.0f);
    auto buf = makeTone(300.0, 12000, amp);   // 500 ms, let envelope settle
    processBlocks(d, buf.data(), 12000);

    const int tailFrames = 512;
    auto tail = makeTone(300.0, tailFrames, amp);
    processBlocks(d, tail.data(), tailFrames);

    const float outPeak = peakAbsStereo(tail.data(), tailFrames);
    const float grDb = linToDb(outPeak / amp);

    // Should be within a fraction of a dB of unity — the bandpass
    // rejects 300 Hz enough that the detector stays below threshold.
    report("low-freq: below-band tone passes untouched",
           std::fabs(grDb) < 0.5f,
           "GR=" + std::to_string(grDb) + " dB");
}

// High-frequency signal in the sidechain band, above threshold, should
// produce attenuation clamped at `amountDb`.
void testSibilantAttenuated()
{
    ClientDeEss d;
    d.prepare(kSampleRate);
    d.setEnabled(true);
    d.setFrequencyHz(6000.0f);
    d.setQ(2.0f);
    d.setThresholdDb(-40.0f);
    d.setAmountDb(-8.0f);
    d.setAttackMs(0.5f);
    d.setReleaseMs(50.0f);

    // 6 kHz at -6 dBFS — smack in the sidechain band, well above threshold.
    const float amp = dbToLin(-6.0f);
    auto buf = makeTone(6000.0, 12000, amp);
    processBlocks(d, buf.data(), 12000);

    const int tailFrames = 512;
    auto tail = makeTone(6000.0, tailFrames, amp);
    processBlocks(d, tail.data(), tailFrames);

    const float outPeak = peakAbsStereo(tail.data(), tailFrames);
    const float grDb = linToDb(outPeak / amp);

    // Expected: attenuation clamped at amountDb (-8 dB).  Allow ±1 dB.
    report("sibilant: HF band above threshold clamps to amount",
           grDb < 0.0f && std::fabs(grDb - (-8.0f)) < 1.0f,
           "GR=" + std::to_string(grDb) + " dB (amount=-8)");
}

// Amount clamp: with amountDb = -3 dB, even a very loud HF tone must
// not attenuate by more than ~3 dB.
void testAmountClamp()
{
    ClientDeEss d;
    d.prepare(kSampleRate);
    d.setEnabled(true);
    d.setFrequencyHz(6000.0f);
    d.setQ(2.0f);
    d.setThresholdDb(-50.0f);
    d.setAmountDb(-3.0f);
    d.setAttackMs(0.5f);
    d.setReleaseMs(50.0f);

    const float amp = dbToLin(-2.0f);
    auto buf = makeTone(6000.0, 12000, amp);
    processBlocks(d, buf.data(), 12000);

    const int tailFrames = 512;
    auto tail = makeTone(6000.0, tailFrames, amp);
    processBlocks(d, tail.data(), tailFrames);

    const float outPeak = peakAbsStereo(tail.data(), tailFrames);
    const float grDb = linToDb(outPeak / amp);

    report("amount: reduction capped at -3 dB",
           grDb >= -3.5f,
           "GR=" + std::to_string(grDb) + " dB (should be ≥ -3.5)");
}

// Moving the frequency knob should steer the sidechain band: a
// 2 kHz tone that triggered attenuation at freq=2000 should be
// untouched at freq=10000.
void testFrequencySteering()
{
    ClientDeEss d;
    d.prepare(kSampleRate);
    d.setEnabled(true);
    d.setQ(2.0f);
    d.setThresholdDb(-40.0f);
    d.setAmountDb(-12.0f);
    d.setAttackMs(0.5f);
    d.setReleaseMs(50.0f);

    const float amp = dbToLin(-6.0f);

    // Tuned to 2 kHz — a 2 kHz tone should trigger attenuation.
    d.setFrequencyHz(2000.0f);
    d.reset();
    auto a = makeTone(2000.0, 12000, amp);
    processBlocks(d, a.data(), 12000);
    auto aTail = makeTone(2000.0, 512, amp);
    processBlocks(d, aTail.data(), 512);
    const float grLow = linToDb(peakAbsStereo(aTail.data(), 512) / amp);

    // Move the sidechain up to 10 kHz; same 2 kHz tone should now
    // slip past it.
    d.setFrequencyHz(10000.0f);
    d.reset();
    auto b = makeTone(2000.0, 12000, amp);
    processBlocks(d, b.data(), 12000);
    auto bTail = makeTone(2000.0, 512, amp);
    processBlocks(d, bTail.data(), 512);
    const float grHigh = linToDb(peakAbsStereo(bTail.data(), 512) / amp);

    // With a broad Q=2 bandpass at 10 kHz the 2 kHz tone still leaks
    // some reduction (skirt rolloff, not brick-wall).  The point is
    // that steering produces a significantly different gate response
    // — require at least 5 dB more reduction on the in-tune side.
    report("frequency: steering changes what counts as sibilant",
           grLow < grHigh - 5.0f,
           "grLow=" + std::to_string(grLow)
           + " grHigh=" + std::to_string(grHigh));
}

// Sanity: complex tone with AM modulation produces finite output, no
// NaN/Inf, peak doesn't exceed input.
void testTransientSanity()
{
    ClientDeEss d;
    d.prepare(kSampleRate);
    d.setEnabled(true);
    d.setFrequencyHz(5000.0f);
    d.setQ(2.0f);
    d.setThresholdDb(-30.0f);
    d.setAmountDb(-8.0f);
    d.setAttackMs(1.0f);
    d.setReleaseMs(100.0f);

    const int frames = 24000;
    std::vector<float> buf(frames * 2);
    const double twoPi = 6.283185307179586476;
    for (int i = 0; i < frames; ++i) {
        // Mix of a low-frequency (vowel) and a high-frequency (sibilant)
        // component with varying amplitude.
        const float env = 0.3f + 0.3f
            * static_cast<float>(std::sin(twoPi * 3.0 * i / kSampleRate));
        const float s = env * (
            0.6f * static_cast<float>(std::sin(twoPi * 500.0  * i / kSampleRate))
          + 0.4f * static_cast<float>(std::sin(twoPi * 5500.0 * i / kSampleRate)));
        buf[i * 2]     = s;
        buf[i * 2 + 1] = s;
    }
    const float inPeak = peakAbsStereo(buf.data(), frames);
    processBlocks(d, buf.data(), frames);
    const float outPeak = peakAbsStereo(buf.data(), frames);

    report("transient: mixed voice + sibilant stays finite",
           !anyNaNorInf(buf.data(), frames) && outPeak <= inPeak + 1e-5f,
           "inPeak=" + std::to_string(inPeak)
           + " outPeak=" + std::to_string(outPeak));
}

void testReset()
{
    ClientDeEss d;
    d.prepare(kSampleRate);
    d.setEnabled(true);
    d.setFrequencyHz(6000.0f);
    d.setThresholdDb(-40.0f);
    d.setAmountDb(-8.0f);

    const float amp = dbToLin(-6.0f);
    auto buf = makeTone(6000.0, 4800, amp);
    processBlocks(d, buf.data(), 4800);

    d.reset();

    std::vector<float> sil(1024, 0.0f);
    d.process(sil.data(), 512, 2);
    report("reset: no NaN/Inf after flush + silence",
           !anyNaNorInf(sil.data(), 512));
}

} // namespace

int main()
{
    std::printf("ClientDeEss test harness @ %.0f Hz, %d-frame blocks\n\n",
                kSampleRate, kBlockSize);

    testBypass();
    testLowFreqPassthrough();
    testSibilantAttenuated();
    testAmountClamp();
    testFrequencySteering();
    testTransientSanity();
    testReset();

    std::printf("\n%s (%d failure%s)\n",
                g_failed == 0 ? "ALL PASS" : "FAILED",
                g_failed, g_failed == 1 ? "" : "s");
    return g_failed == 0 ? 0 : 1;
}
