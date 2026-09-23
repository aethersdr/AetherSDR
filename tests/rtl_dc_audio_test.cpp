// Synthetic ADC IQ through the production NCO/resampler and blocking WDSP FM
// channel. No USB, peer, radio, display manipulation, or calibrated dBm claim.
#include "core/backends/rtl/RtlRfExtractor.h"
#include "core/backends/rtl/RtlCaptureTransaction.h"
#include "core/dsp/WdspChannel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>
#include <vector>

using Extractor = AetherSDR::rtl::RtlRfExtractor;
using Transaction = AetherSDR::rtl::RtlCaptureTransaction;
namespace {
int failures = 0;
void check(bool accepted, const char* message)
{
    if (!accepted) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
constexpr double kRate = 2'400'000;
constexpr double kCarrier = 100'000'000;
constexpr double kTone = 1000;
constexpr double kAmplitude = 0.1;
constexpr std::uint64_t kDiscard = 96'000;
constexpr std::uint64_t kCount = 48'000;

struct Measurement : Extractor::Sink {
    std::unique_ptr<WdspChannel> channel;
    std::vector<float> pcm;
    double peak = 0;
    double fundamental = 0;
    double residualRatio = 0;
    bool iqBlock(std::span<const float> i, std::span<const float> q,
                 std::uint64_t first) noexcept override
    {
        std::array<float, 1024> left{}, right{};
        if (channel->processIq(i, q, left, right) != WdspChannel::ProcessResult::Ok) { return false; }
        for (std::size_t n = 0; n < left.size(); ++n) {
            if (!std::isfinite(left[n]) || !std::isfinite(right[n])) { return false; }
            peak = std::max(peak, std::abs(double(left[n])));
            if (first + n >= kDiscard && first + n < kDiscard + kCount) { pcm.push_back(left[n]); }
        }
        return true;
    }
    void finish()
    {
        check(pcm.size() == kCount, "exact settled sample interval survives extraction and DSP");
        if (pcm.empty()) { return; }
        double sum = 0, square = 0, re = 0, im = 0;
        for (std::size_t n = 0; n < pcm.size(); ++n) {
            const double phase = 2 * std::numbers::pi * kTone * n / 48'000;
            sum += pcm[n]; square += double(pcm[n]) * pcm[n];
            re += pcm[n] * std::cos(phase); im += pcm[n] * std::sin(phase);
        }
        fundamental = 2 * std::hypot(re, im) / pcm.size();
        const double mean = sum / pcm.size();
        const double energy = square / pcm.size();
        residualRatio = std::sqrt(std::max(0.0, energy - fundamental * fundamental / 2 - mean * mean)
                                  / std::max(energy, 1e-20));
        check(peak < 1.0, "startup, ADC bias change and settled PCM retain headroom");
        channel.reset();
    }
};

enum class Bias { Clean, Step, BlindMean };

Measurement measure(double fmRecipe, double modulation, double offset, Bias bias, bool fragmented = false)
{
    Measurement result;
    result.pcm.reserve(kCount);
    WdspChannel::Config dsp;
    dsp.mode = WdspChannel::Mode::Fm;
    dsp.fmReceive = WdspChannel::FmReceive{fmRecipe};
    dsp.filterLowHz = -8000; dsp.filterHighHz = 8000;
    dsp.blockForOutput = true;
    result.channel = WdspChannel::create(dsp);
    check(bool(result.channel), "production FM recipe constructs");
    if (!result.channel) { return result; }
    const Extractor::Config cfg{{1, 1, kCarrier - offset, kRate, .45 * kRate, .45 * kRate},
        {0, kCarrier, -8000, 8000, 0, 3000, 3000}, 48'000, 1024};
    Extractor extractor(cfg);
    check(extractor.valid(), "complete passband fits displaced capture");
    std::array<std::complex<float>, 8192> iq{};
    constexpr std::array<std::size_t, 4> chunks{257, 4093, 8192, 997};
    std::uint32_t random = 12345;
    for (std::uint64_t first = 0, block = 0; first < 7'440'000; ++block) {
        const std::size_t count = std::min<std::uint64_t>(7'440'000 - first,
            fragmented ? chunks[block % chunks.size()] : iq.size());
        for (std::size_t i = 0; i < count; ++i) {
            const double time = (first + i) / kRate;
            const double phase = 2 * std::numbers::pi * offset * time
                + modulation / kTone * std::sin(2 * std::numbers::pi * kTone * time);
            const std::complex<double> adcBias = bias == Bias::Clean ? std::complex<double>{}
                : time < 1.25 ? std::complex<double>{.02, -.01} : std::complex<double>{.04, .03};
            std::complex<double> value = std::polar(kAmplitude, phase) + adcBias;
            random = 1664525 * random + 1013904223;
            value += std::complex<double>{(int(random >> 16) - 32768) * .002 / 32768,
                                          (int(random & 65535) - 32768) * .002 / 32768};
            value = {std::round((value.real() + 1) * 127.5) / 127.5 - 1,
                     std::round((value.imag() + 1) * 127.5) / 127.5 - 1};
            // Negative control only: blind removal destroys the wanted carrier
            // mean at low modulation index. Production never does this.
            if (bias == Bias::BlindMean) {
                value -= adcBias + std::complex<double>{kAmplitude * std::cyl_bessel_j(0, modulation / kTone), 0};
            }
            iq[i] = {float(value.real()), float(value.imag())};
        }
        check(extractor.process(cfg.capture, first, {iq.data(), count}, result), "continuous production extraction");
        first += count;
    }
    result.finish();
    std::printf("recipe=%.0f modulation=%.0f offset=%.0f bias=%d fragmented=%d samples=%zu fundamental=%.8f residual=%.8f peak=%.8f\n",
        fmRecipe, modulation, offset, int(bias), fragmented, result.pcm.size(), result.fundamental,
        result.residualRatio, result.peak);
    return result;
}
} // namespace

int main()
{
    for (double recipe : {2500.0, 5000.0}) {
        for (double modulation : {100.0, 2500.0}) {
            for (double offset : {-600'000.0, -Transaction::kDcSeparationHz, Transaction::kDcSeparationHz}) {
                const Measurement clean = measure(recipe, modulation, offset, Bias::Clean);
                const Measurement biased = measure(recipe, modulation, offset, Bias::Step);
                check(clean.fundamental > .005, "wanted low-index FM survives digital translation");
                check(std::abs(biased.fundamental / clean.fundamental - 1) < .15,
                    "displaced converter bias preserves matched wanted tone gain");
                check(biased.residualRatio < clean.residualRatio + .035,
                    "displaced converter bias does not add material settled distortion");
                if (recipe == 2500 && modulation == 100 && offset == -600'000) {
                    const Measurement fragmented = measure(recipe, modulation, offset, Bias::Step, true);
                    check(fragmented.pcm == biased.pcm, "callback chunking cannot change displaced FM PCM");
                }
            }
        }
    }
    const Measurement centered = measure(2500, 2500, 0, Bias::Step);
    const Measurement displaced = measure(2500, 2500, -600'000, Bias::Step);
    check(centered.residualRatio > displaced.residualRatio * 5,
        "negative control detects biased zero-IF FM distortion");
    const Measurement lowIndex = measure(2500, 100, 0, Bias::Clean);
    const Measurement removedMean = measure(2500, 100, 0, Bias::BlindMean);
    check(removedMean.fundamental > lowIndex.fundamental * 2,
        "negative control detects blind mean removal destroying low-index FM fidelity");
    std::printf("rtl_dc_audio_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
