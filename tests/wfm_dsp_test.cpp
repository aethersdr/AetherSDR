// Standalone tests for the SkyRoof-parity WFM DSP chain (WfmDsp).
// Run: ./build/wfm_dsp_test
//
// Pins the three properties that make HS-SoundModem decode reliably:
//   1. The NCO removes the discriminator DC term for off-centre signals
//      (fixed pan + Doppler-stepped slice), without disturbing the audio.
//   2. Any native DAX IQ rate (96 k here) is resampled to exactly 48 kHz
//      with I/Q phase integrity preserved — the FSK tone survives intact.
//   3. Streaming state (NCO, resamplers, discriminator, FIR) is continuous
//      across process() block boundaries — no clicks at chunk edges.

#include "core/WfmDsp.h"

#include <cmath>
#include <cstdio>
#include <numbers>   // std::numbers::pi — M_PI needs _USE_MATH_DEFINES on MSVC
#include <string>
#include <vector>

using AetherSDR::WfmDsp;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-68s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail.c_str());
    if (!ok) ++g_failed;
}

// FM signal: carrier at `offsetHz` from IQ centre, frequency-modulated by a
// `toneHz` sine with peak deviation `devHz`. Unit amplitude.
std::vector<float> makeFm(int rate, int frames, double offsetHz,
                          double devHz, double toneHz)
{
    std::vector<float> iq(static_cast<size_t>(frames) * 2);
    double phase = 0.0;
    const double twoPi = 2.0 * std::numbers::pi;
    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        const double instFreq = offsetHz + devHz * std::cos(twoPi * toneHz * t);
        phase += twoPi * instFreq / rate;
        if (phase > std::numbers::pi) phase -= twoPi;
        iq[2 * static_cast<size_t>(i)]     = static_cast<float>(std::cos(phase));
        iq[2 * static_cast<size_t>(i) + 1] = static_cast<float>(std::sin(phase));
    }
    return iq;
}

double meanOf(const std::vector<float>& x, size_t from, size_t count)
{
    double sum = 0.0;
    for (size_t i = from; i < from + count; ++i) sum += x[i];
    return sum / static_cast<double>(count);
}

// Single-bin DFT magnitude → amplitude of a sine at toneHz (48 kHz signal).
// `count` must span an integer number of tone cycles.
double toneAmplitude(const std::vector<float>& x, size_t from, size_t count,
                     double toneHz)
{
    double re = 0.0, im = 0.0;
    const double w = 2.0 * std::numbers::pi * toneHz / WfmDsp::kAudioRate;
    for (size_t i = 0; i < count; ++i) {
        const double v = x[from + i];
        re += v * std::cos(w * static_cast<double>(i));
        im += v * std::sin(w * static_cast<double>(i));
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(count);
}

constexpr double kDevHz  = 3000.0;   // G3RUH-like deviation
constexpr double kToneHz = 1200.0;
// disc = Δφ·kGain/π; Δφ = 2π·dev/fs  →  amplitude = 2·dev·kGain/fs
constexpr double kExpectedAmp = 2.0 * kDevHz * 3.0 / 48000.0;  // 0.375

// Steady-state analysis window: skip FIR (47) + resampler transients,
// then take an integer number of 1200 Hz cycles (24000 = 600 cycles).
constexpr size_t kSkip = 4800;
constexpr size_t kSpan = 24000;

} // namespace

int main()
{
    // ── 1. Centred signal at 48 k: passthrough, correct tone, no DC ──────
    {
        WfmDsp dsp(48000);
        std::vector<float> out;
        dsp.process(makeFm(48000, 48000, 0.0, kDevHz, kToneHz).data(), 48000, out);

        report("48k passthrough keeps sample count",
               out.size() == 48000,
               "got " + std::to_string(out.size()));

        const double dc  = meanOf(out, kSkip, kSpan);
        const double amp = toneAmplitude(out, kSkip, kSpan, kToneHz);
        report("48k centred: no DC term", std::abs(dc) < 0.01,
               "dc=" + std::to_string(dc));
        report("48k centred: tone amplitude ~0.375",
               std::abs(amp - kExpectedAmp) < 0.1 * kExpectedAmp,
               "amp=" + std::to_string(amp));
    }

    // ── 2. +10 kHz off-centre: DC without NCO, clean with NCO ────────────
    {
        const auto iq = makeFm(48000, 48000, 10000.0, kDevHz, kToneHz);

        WfmDsp uncorrected(48000);
        std::vector<float> out;
        uncorrected.process(iq.data(), 48000, out);
        const double dcRaw = meanOf(out, kSkip, kSpan);
        // expected DC = 2·offset·kGain/fs = 2·10000·3/48000 = 1.25
        report("10 kHz offset, no NCO: DC term ≈1.25 (would clip downstream)",
               dcRaw > 1.0, "dc=" + std::to_string(dcRaw));

        WfmDsp corrected(48000);
        corrected.setFreqOffsetHz(10000.0f);
        corrected.process(iq.data(), 48000, out);
        const double dc  = meanOf(out, kSkip, kSpan);
        const double amp = toneAmplitude(out, kSkip, kSpan, kToneHz);
        report("10 kHz offset, NCO: DC removed", std::abs(dc) < 0.01,
               "dc=" + std::to_string(dc));
        report("10 kHz offset, NCO: tone amplitude intact",
               std::abs(amp - kExpectedAmp) < 0.1 * kExpectedAmp,
               "amp=" + std::to_string(amp));
    }

    // ── 3. 96 k native rate: resampled to exactly 48 k, tone intact ──────
    {
        WfmDsp dsp(96000);
        dsp.setFreqOffsetHz(10000.0f);
        std::vector<float> out;
        dsp.process(makeFm(96000, 96000, 10000.0, kDevHz, kToneHz).data(), 96000, out);

        report("96k→48k: output ≈ half the input frames",
               out.size() > 47000 && out.size() <= 48000,
               "got " + std::to_string(out.size()));

        const double dc  = meanOf(out, kSkip, kSpan);
        const double amp = toneAmplitude(out, kSkip, kSpan, kToneHz);
        report("96k→48k: no DC term", std::abs(dc) < 0.01,
               "dc=" + std::to_string(dc));
        report("96k→48k: tone amplitude intact",
               std::abs(amp - kExpectedAmp) < 0.1 * kExpectedAmp,
               "amp=" + std::to_string(amp));
    }

    // ── 4. Chunked processing matches single-shot (state continuity) ─────
    {
        const auto iq = makeFm(96000, 96000, 5000.0, kDevHz, kToneHz);

        WfmDsp whole(96000);
        whole.setFreqOffsetHz(5000.0f);
        std::vector<float> outWhole;
        whole.process(iq.data(), 96000, outWhole);

        WfmDsp chunked(96000);
        chunked.setFreqOffsetHz(5000.0f);
        std::vector<float> outChunked, block;
        constexpr int kChunk = 1024;   // not a divisor of 96000 on purpose
        for (int off = 0; off < 96000; off += kChunk) {
            const int n = std::min(kChunk, 96000 - off);
            chunked.process(iq.data() + 2 * static_cast<size_t>(off), n, block);
            outChunked.insert(outChunked.end(), block.begin(), block.end());
        }

        const size_t n = std::min(outWhole.size(), outChunked.size());
        double maxDiff = 0.0;
        for (size_t i = 0; i < n; ++i)
            maxDiff = std::max(maxDiff,
                               static_cast<double>(std::abs(outWhole[i] - outChunked[i])));
        report("chunked == single-shot (no clicks at block edges)",
               n > 90000 / 2 && maxDiff < 1e-3,
               "n=" + std::to_string(n) + " maxDiff=" + std::to_string(maxDiff));
    }

    // ── 5. maxFreqOffsetHz policy values ─────────────────────────────────
    {
        report("maxFreqOffsetHz @48k = 14800 (0.95·24k − 8k guard)",
               std::abs(WfmDsp(48000).maxFreqOffsetHz() - 14800.0f) < 1.0f);
        report("maxFreqOffsetHz @96k limited by 48k output window",
               std::abs(WfmDsp(96000).maxFreqOffsetHz() - 14800.0f) < 1.0f);
        report("maxFreqOffsetHz @24k = 3400 (degraded window)",
               std::abs(WfmDsp(24000).maxFreqOffsetHz() - 3400.0f) < 1.0f);
    }

    if (g_failed) {
        std::printf("\n%d test(s) FAILED\n", g_failed);
        return 1;
    }
    std::printf("\nAll tests passed\n");
    return 0;
}
