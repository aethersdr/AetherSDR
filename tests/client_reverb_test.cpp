// Standalone test harness for ClientReverb DSP (Freeverb).
// Build: CMake target `client_reverb_test`.  Exit 0 = pass.

#include "core/ClientReverb.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using AetherSDR::ClientReverb;

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

std::vector<float> makeImpulse(int frames, float amplitude = 0.9f)
{
    std::vector<float> buf(frames * 2, 0.0f);
    buf[0] = amplitude;
    buf[1] = amplitude;
    return buf;
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

float l2NormStereo(const float* data, int frames)
{
    double sumSq = 0.0;
    const int samples = frames * 2;
    for (int i = 0; i < samples; ++i) sumSq += data[i] * data[i];
    return static_cast<float>(std::sqrt(sumSq));
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

void processBlocks(ClientReverb& r, float* buf, int frames)
{
    int remaining = frames;
    float* p = buf;
    while (remaining > 0) {
        const int n = std::min(kBlockSize, remaining);
        r.process(p, n, 2);
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

} // namespace

int main()
{
    // ── Test 1: default construction ──────────────────────────────────
    {
        ClientReverb r;
        r.prepare(kSampleRate);
        report("constructor starts disabled", !r.isEnabled());
    }

    // ── Test 2: enable toggle ─────────────────────────────────────────
    {
        ClientReverb r;
        r.prepare(kSampleRate);
        r.setEnabled(true);
        const bool gotOn = r.isEnabled();
        r.setEnabled(false);
        const bool gotOff = !r.isEnabled();
        report("enable toggle", gotOn && gotOff);
    }

    // ── Test 3: impulse response produces non-trivial tail ────────────
    {
        ClientReverb r;
        r.prepare(kSampleRate);
        r.setEnabled(true);
        r.setMix(1.0f);                // fully wet
        r.setDecayS(2.0f);
        r.setPreDelayMs(0.0f);
        // Longer buffer so the tail has room to manifest.
        const int totalFrames = static_cast<int>(kSampleRate * 1.0);
        std::vector<float> buf = makeImpulse(totalFrames, 0.9f);
        processBlocks(r, buf.data(), totalFrames);
        // Tail region: skip the first 10 ms (direct + initial echoes)
        // and measure L2 norm of the next ~100 ms.
        const int tailStart = static_cast<int>(kSampleRate * 0.01);
        const int tailLen   = static_cast<int>(kSampleRate * 0.10);
        const float tailEnergy = l2NormStereo(
            buf.data() + tailStart * 2, tailLen);
        report("impulse → non-zero tail", tailEnergy > 1e-4f,
               "energy=" + std::to_string(tailEnergy));
        report("impulse → finite output",
               !anyNaNorInf(buf.data(), totalFrames));
    }

    // ── Test 4: tail energy decays over time ──────────────────────────
    {
        ClientReverb r;
        r.prepare(kSampleRate);
        r.setEnabled(true);
        r.setMix(1.0f);
        r.setDecayS(1.5f);
        r.setPreDelayMs(0.0f);
        const int totalFrames = static_cast<int>(kSampleRate * 2.0);
        std::vector<float> buf = makeImpulse(totalFrames, 0.9f);
        processBlocks(r, buf.data(), totalFrames);
        const int halfLen = totalFrames / 2;
        const float firstHalf = l2NormStereo(buf.data(), halfLen);
        const float lastHalf  = l2NormStereo(buf.data() + halfLen * 2,
                                             totalFrames - halfLen);
        report("tail energy decays", firstHalf > lastHalf * 1.5f,
               "first=" + std::to_string(firstHalf) +
               " last=" + std::to_string(lastHalf));
    }

    // ── Test 5: mix=0 → essentially dry passthrough ───────────────────
    // With mix=0 the wet path is silenced and dryGain becomes 1.0
    // (see recacheIfDirty: dryGain = 1 - 0.5 * mix → 1.0 at mix=0).
    {
        ClientReverb r;
        r.prepare(kSampleRate);
        r.setEnabled(true);
        r.setMix(0.0f);
        r.setPreDelayMs(0.0f);
        const int frames = 2048;
        std::vector<float> ref  = makeTone(440.0, frames, 0.3f);
        std::vector<float> work = ref;
        processBlocks(r, work.data(), frames);
        double diffSumSq = 0.0;
        for (int i = 0; i < frames * 2; ++i) {
            const float d = work[i] - ref[i];
            diffSumSq += d * d;
        }
        const float rms = static_cast<float>(
            std::sqrt(diffSumSq / (frames * 2)));
        report("mix=0 → dry passthrough", rms < 1e-4f,
               "rms diff=" + std::to_string(rms));
    }

    // ── Test 6: mix=1 reduces dry level meaningfully ──────────────────
    // Freeverb's fixed gain (0.015) attenuates the wet path, so a pure
    // tone at mix=1 should be noticeably quieter than the dry input.
    {
        ClientReverb r;
        r.prepare(kSampleRate);
        r.setEnabled(true);
        r.setMix(1.0f);
        r.setPreDelayMs(0.0f);
        const int frames = 4096;
        std::vector<float> buf = makeTone(440.0, frames, 0.5f);
        const float dryPeak = peakAbsStereo(buf.data(), frames);
        processBlocks(r, buf.data(), frames);
        const float wetPeak = peakAbsStereo(buf.data(), frames);
        // dryGain = 1 - 0.5*1 = 0.5 → a pure-tone mix-1 output should
        // sit well below the original dry peak.
        report("mix=1 attenuates direct signal",
               wetPeak < dryPeak * 0.9f,
               "dry=" + std::to_string(dryPeak) +
               " wet=" + std::to_string(wetPeak));
    }

    // ── Test 7: pre-delay shifts the wet tail start ───────────────────
    // With an impulse at t=0 and pre-delay=50 ms, the first 40 ms
    // of output (dry=0 after impulse frame) should be silent apart
    // from the impulse itself.
    {
        ClientReverb r;
        r.prepare(kSampleRate);
        r.setEnabled(true);
        r.setMix(1.0f);
        r.setPreDelayMs(50.0f);
        r.setDecayS(2.0f);
        const int totalFrames = static_cast<int>(kSampleRate * 0.3);
        std::vector<float> buf = makeImpulse(totalFrames, 0.9f);
        processBlocks(r, buf.data(), totalFrames);
        // Measure energy BEFORE the pre-delay period kicks in.  Skip
        // the first 2 frames (the impulse itself × dryGain may leak
        // through).
        const int preGapEnd  = static_cast<int>(kSampleRate * 0.04);
        const float preEnergy = l2NormStereo(buf.data() + 4,
                                             preGapEnd - 2);
        const int postStart   = static_cast<int>(kSampleRate * 0.06);
        const int postLen     = static_cast<int>(kSampleRate * 0.10);
        const float postEnergy = l2NormStereo(
            buf.data() + postStart * 2, postLen);
        report("pre-delay holds back the wet tail",
               postEnergy > preEnergy * 3.0f,
               "pre=" + std::to_string(preEnergy) +
               " post=" + std::to_string(postEnergy));
    }

    // ── Test 8: mono input works (channels=1) ─────────────────────────
    {
        ClientReverb r;
        r.prepare(kSampleRate);
        r.setEnabled(true);
        r.setMix(1.0f);
        const int frames = 2048;
        std::vector<float> buf(frames, 0.0f);
        buf[0] = 0.9f;   // impulse
        r.process(buf.data(), frames, 1);
        report("mono process does not crash / corrupt",
               !anyNaNorInf(buf.data(), frames / 2));
    }

    if (g_failed == 0) {
        std::printf("\nAll tests passed.\n");
        return 0;
    }
    std::printf("\n%d test(s) failed.\n", g_failed);
    return 1;
}
