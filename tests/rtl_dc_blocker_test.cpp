#include "core/backends/rtl/RtlDcBlocker.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

using AetherSDR::rtl::RtlDcBlocker;
using Complex = std::complex<float>;
namespace {
int failures = 0;
void check(bool value, const char* message)
{
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
std::vector<Complex> tone(double rate, double hz, double seconds)
{
    std::vector<Complex> samples(static_cast<std::size_t>(rate * seconds));
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = std::polar(0.25f, float(2 * std::numbers::pi * hz * i / rate));
    }
    return samples;
}
double power(std::span<const Complex> samples)
{
    double sum = 0;
    for (Complex value : samples) { sum += std::norm(std::complex<double>(value)); }
    return sum / samples.size();
}
void blocked(RtlDcBlocker& filter, std::vector<Complex>& samples, std::size_t chunk)
{
    for (std::size_t offset = 0; offset < samples.size(); offset += chunk) {
        filter.process(std::span(samples).subspan(offset, std::min(chunk, samples.size() - offset)));
    }
}
}
int main()
{
    for (double rate : {225001.0, 300000.0, 900001.0, 2400000.0, 3000000.0}) {
        RtlDcBlocker filter;
        std::vector<Complex> dc(std::size_t(rate * 0.4), {0.2f, -0.3f});
        filter.configure(false, rate);
        const auto original = dc;
        filter.process(dc);
        check(dc == original, "disabled path preserves every IQ bit");
        filter.configure(true, rate);
        blocked(filter, dc, 8192);
        const double residual = 10 * std::log10(power(std::span(dc).last(std::size_t(rate * 0.1))) / 0.13);
        std::printf("rate=%.0f DC residual last 100ms=%.3f dB\n", rate, residual);
        check(residual < -85, "DC step settled below -85 dB after 300ms");
        check(std::abs(dc[std::size_t(rate * 0.22)]) < std::sqrt(0.13) * 0.00101,
            "startup step below -60dB after 220ms; samples are not discarded");
        filter.reset();
        auto replay = original;
        blocked(filter, replay, 127);
        check(dc == replay, "arbitrary callback partitions do not change output and reset is deterministic");
        for (double hz : {0.0, 1.0, 5.0, 50.0, 100.0, 500.0, -500.0, 48000.0}) {
            auto samples = tone(rate, hz, 0.5);
            filter.configure(true, rate);
            blocked(filter, samples, 613);
            const double gain = std::sqrt(power(std::span(samples).last(std::size_t(rate * 0.1))) / 0.0625);
            const double pole = std::exp(-2 * std::numbers::pi * 5 / rate);
            const std::complex<double> delay = std::polar(1.0, -2 * std::numbers::pi * hz / rate);
            const double expected = std::abs((1 + pole) / 2 * (1.0 - delay) / (1.0 - pole * delay));
            check(std::abs(gain - expected) < 0.00008, "measured complex carrier gain matches continuous high-pass response");
            if (rate == 2400000) { std::printf("carrier=%g Hz gain=%g dB\n", hz, 20 * std::log10(gain)); }
        }
    }
    constexpr double rate = 2400000;
    RtlDcBlocker filter;
    std::mt19937 random(7311);
    std::normal_distribution<float> normal(0, 0.02f);
    std::vector<Complex> noise(std::size_t(rate * 0.5));
    for (Complex& sample : noise) { sample = {normal(random), normal(random)}; }
    const double inputNoise = power(std::span(noise).last(240000));
    filter.configure(true, rate); filter.process(noise);
    const double noiseDb = 10 * std::log10(power(std::span(noise).last(240000)) / inputNoise);
    std::printf("wideband noise change=%g dB\n", noiseDb);
    check(std::abs(noiseDb) < 0.001, "wideband noise power is preserved outside the narrow DC notch");

    // Wanted modulation is measured after 300ms settling. Both +/- capture
    // offsets and FM deviation extremes exercise real IQ, not drawing masks.
    for (double carrier : {-48000.0, 48000.0, 0.0, 50.0}) {
        for (int mode = 0; mode < 4; ++mode) {
            std::vector<Complex> samples(std::size_t(rate * 0.5));
            const double deviation = mode == 0 ? 5000 : 2500;
            for (std::size_t i = 0; i < samples.size(); ++i) {
                const double phase = 2 * std::numbers::pi * 1000 * i / rate;
                const double modulation = mode < 2 ? -deviation / 1000 * std::cos(phase) : 0;
                const double amplitude = mode == 2 ? 0.2 * (1 + 0.7 * std::cos(phase)) : 0.2;
                samples[i] = std::polar(float(amplitude), float(2 * std::numbers::pi * carrier * i / rate + modulation));
            }
            const auto input = samples;
            filter.configure(true, rate); blocked(filter, samples, 8192);
            double error = 0;
            for (std::size_t i = 720000; i < samples.size(); ++i) {
                error += std::norm(samples[i] - input[i]);
            }
            const double relativeError = std::sqrt(error / (samples.size() - 720000)
                / power(std::span(input).subspan(720000)));
            std::printf("mode=%d carrier=%g relative IQ error=%g\n", mode, carrier, relativeError);
            if (std::abs(carrier) >= 48000) {
                check(relativeError < 0.0002, "FM/FMN/AM/CW at DC-clear placement preserve wanted IQ within 0.02 percent");
            } else if (carrier == 0 && mode >= 2) {
                check(relativeError > 0.8, "AM/CW at capture center loses its carrier: limitation must remain explicit");
            }
        }
    }
    for (std::size_t chunk : {std::size_t(257), std::size_t(8192)}) {
        auto perBlockMean = tone(rate, 50, 0.5);
        for (std::size_t start = 0; start < perBlockMean.size(); start += chunk) {
            auto block = std::span(perBlockMean).subspan(start, std::min(chunk, perBlockMean.size() - start));
            std::complex<double> mean;
            for (Complex sample : block) { mean += sample; }
            mean /= block.size();
            for (Complex& sample : block) { sample -= Complex(mean); }
        }
        const double gainDb = 10 * std::log10(power(std::span(perBlockMean).last(240000)) / 0.0625);
        std::printf("rejected block-mean method: chunk=%zu 50Hz gain=%g dB\n", chunk, gainDb);
        check(gainDb < -10, "block-mean subtraction would destroy nearby RF depending on USB chunk size");
    }
    auto benchmark = tone(rate, 48000, 1);
    filter.configure(true, rate);
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 10; ++i) { filter.process(benchmark); }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    std::printf("DC filter: 10 seconds IQ in %.6f seconds; state=%zu bytes\n", elapsed, sizeof(filter));
    return failures ? 1 : 0;
}
