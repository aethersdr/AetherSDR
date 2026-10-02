#include "core/backends/rtl/RtlRfExtractor.h"
#include "CallbackAllocationProbe.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <vector>

using Extractor = AetherSDR::rtl::RtlRfExtractor;
static int failures = 0;
static void check(bool value, const char* message)
{
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
struct Sink : Extractor::Sink {
    std::vector<std::complex<float>> samples;
    std::uint64_t next = 0;
    bool iqBlock(std::span<const float> i, std::span<const float> q, std::uint64_t first) noexcept override
    {
        if (i.size() != 1024 || q.size() != i.size() || first != next) { return false; }
        for (std::size_t n = 0; n < i.size(); ++n) { samples.emplace_back(i[n], q[n]); }
        next += i.size();
        return true;
    }
};
static Extractor::Config config(double offset = 0)
{
    return {{1, 1, 100e6, 2.4e6, 1.08e6, 1.08e6},
        {0, 100e6 + offset, -15000, 15000, 0, 3000, 3000}, 48000, 1024};
}
static Sink convert(const Extractor::Config& cfg, const std::vector<std::complex<float>>& input,
                    bool fragmented)
{
    Extractor extractor(cfg); Sink sink; sink.samples.reserve(48000);
    const std::array<std::size_t, 4> chunks{1, 17, 263, 997};
    for (std::size_t offset = 0, block = 0; offset < input.size(); ++block) {
        const std::size_t count = std::min(input.size() - offset,
            fragmented ? chunks[block % chunks.size()] : Extractor::kMaxInput);
        inCallback = true;
        const bool accepted = extractor.process(cfg.capture, offset, std::span(input).subspan(offset, count), sink);
        inCallback = false;
        check(accepted, "arbitrary capture chunks remain continuous");
        offset += count;
    }
    const double expected = input.size() * double(cfg.outputRateHz) / cfg.capture.achievedSampleRateHz;
    check(sink.samples.size() <= expected + 1 && expected - sink.samples.size() < 1024 + 256,
          "count differs only by fixed-block and converter staging");
    return sink;
}
static double toneMagnitude(std::span<const float> samples, double hz)
{
    std::complex<double> sum{};
    for (std::size_t n = 0; n < samples.size(); ++n) {
        const double phase = -2 * std::numbers::pi * hz * n / 48000;
        sum += double(samples[n]) * std::complex<double>(std::cos(phase), std::sin(phase));
    }
    return std::abs(sum) / samples.size();
}
int main()
{
    Extractor extractor(config());
    check(extractor.valid(), "valid readback and full passband prepare extraction");
    std::vector<std::complex<float>> input(65536, {0.5f, 0});
    Sink sink; sink.samples.reserve(48000);
    check(extractor.process(config().capture, 0, input, sink), "production extractor accepts bounded capture");
    check(!sink.samples.empty(), "fixed planar blocks reach DSP");
    // Four independently modulated carriers, including both full-passband
    // capture edges. No duplicated waveform masquerading as a fourth signal.
    constexpr std::array<double, 4> offsets{-1062000, -1000000, 1000000, 1062000};
    constexpr std::array<double, 4> tones{701, 1093, 1601, 2203};
    std::vector<std::complex<float>> four(600000);
    for (std::size_t n = 0; n < four.size(); ++n) {
        for (std::size_t carrier = 0; carrier < offsets.size(); ++carrier) {
            const double t = n / 2400000.0;
            const double phase = 2 * std::numbers::pi * offsets[carrier] * t
                + 3500 / tones[carrier] * std::sin(2 * std::numbers::pi * tones[carrier] * t);
            four[n] += std::complex<float>(0.15 * std::cos(phase), 0.15 * std::sin(phase));
        }
    }
    for (std::size_t carrier = 0; carrier < offsets.size(); ++carrier) {
        const auto cfg = config(offsets[carrier]);
        const Sink whole = convert(cfg, four, false);
        const Sink fragmented = convert(cfg, four, true);
        check(whole.samples == fragmented.samples, "fragmented and whole IQ are bit-identical");
        std::vector<float> audio;
        for (std::size_t n = 2049; n < whole.samples.size(); ++n) {
            audio.push_back(std::arg(whole.samples[n] * std::conj(whole.samples[n - 1])));
        }
        const double wanted = toneMagnitude(audio, tones[carrier]);
        check(wanted > 0.1, "NCO sign retains the selected FM modulation");
        for (std::size_t other = 0; other < tones.size(); ++other) {
            if (other == carrier) { continue; }
            const double rejection = 20 * std::log10(wanted / std::max(1e-12, toneMagnitude(audio, tones[other])));
            check(rejection > 30, "adjacent distinct FM carrier rejected by at least 30 dB");
            std::printf("carrier %zu versus %zu rejection %.1f dB\n", carrier, other, rejection);
        }
    }
    {
        const auto cfg = config(offsets[1]);
        const Sink whole = convert(cfg, four, false);
        Extractor joined(cfg); Sink late; late.next = 247; late.samples.reserve(48000);
        for (std::size_t first = 12345; first < four.size();) {
            const std::size_t count = std::min(Extractor::kMaxInput, four.size() - first);
            check(joined.process(cfg.capture, first, std::span(four).subspan(first, count), late),
                "receiver joining between sample coincidences uses common output lattice");
            first += count;
        }
        double worst = 0;
        for (std::size_t n = 2048; n < late.samples.size() && n + 247 < whole.samples.size(); ++n) {
            worst = std::max(worst, double(std::abs(late.samples[n] - whole.samples[n + 247])));
        }
        check(worst < 0.0002, "late receiver shares absolute NCO phase and acoustic sample timing after startup delay");
    }
    {
        Extractor changed(config()); Sink output; output.samples.reserve(48000);
        auto stale = config().capture; ++stale.generation;
        check(!changed.process(stale, 0, input, output) && !changed.withdrawn(),
              "stale capture generation cannot consume current history");
        check(changed.process(config().capture, 0, input, output), "matching generation still works");
        check(!changed.process(config().capture, input.size() + 1, input, output) && changed.withdrawn(),
              "gap withdraws history without realtime reset");
        check(!changed.process(config().capture, input.size(), input, output), "withdrawn extractor cannot resume stale history");
    }
    {
        auto bad = config(); bad.slice.carrierHz = 101063000;
        check(!Extractor(bad).valid(), "full passband plus guard beyond edge refused");
        auto fractional = config(); fractional.capture.achievedSampleRateHz = 225001;
        fractional.capture.usableLeftHz = fractional.capture.usableRightHz = 100000;
        convert(fractional, input, true);
        Extractor invalid(config()); Sink output;
        input[3] = {std::numeric_limits<float>::quiet_NaN(), 0};
        check(!invalid.process(config().capture, 0, input, output) && output.samples.empty(),
              "nonfinite block refused before any partial DSP publication");
    }
    check(callbackAllocations == 0, "RF callback performs no ordinary C++ allocation");
    std::fprintf(stderr, "rtl_rf_extractor_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
