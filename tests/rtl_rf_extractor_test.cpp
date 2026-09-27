#include "core/backends/rtl/RtlRfExtractor.h"
#include "CallbackAllocationProbe.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <type_traits>
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
static void firstFailureObservation()
{
    static_assert(std::is_trivially_copyable_v<Extractor::Failure>);
    static_assert(std::is_trivially_copyable_v<std::optional<Extractor::Failure>>);
    struct RejectingSink final : Extractor::Sink {
        int calls = 0;
        bool iqBlock(std::span<const float>, std::span<const float>, std::uint64_t) noexcept override
        { ++calls; return false; }
    } sink;
    const auto process = [&sink](Extractor& extractor, std::uint64_t first,
                                std::span<const std::complex<float>> input, bool discontinuity = false) {
        inCallback = true;
        const bool result = extractor.process(config().capture, first, input, sink, discontinuity);
        inCallback = false;
        return result;
    };
    std::array<std::complex<float>, 64> shortInput{};
    Extractor gap(config());
    check(!gap.failure(), "new extractor has no failure observation");
    check(process(gap, 25, shortInput), "nonzero origin starts without a complete DSP block");
    check(!process(gap, 90, shortInput) && gap.withdrawn(), "one missing capture sample still withdraws");
    const auto gapFailure = gap.failure();
    check(gapFailure && gapFailure->reason == Extractor::FailureReason::CapturePositionMismatch
        && gapFailure->hasExpectedCaptureFirst && gapFailure->expectedCaptureFirst == 89
        && gapFailure->captureFirst == 90 && gapFailure->captureFrames == 64
        && gapFailure->hasIqFirst && gapFailure->iqFirst == 1 && gapFailure->iqFrames == 0,
        "gap observation names exact expected/actual capture and next IQ positions");
    check(!process(gap, 89, shortInput) && gap.failure() == gapFailure,
        "withdrawn retry cannot erase or replace the first gap");

    Extractor discontinuous(config());
    check(process(discontinuous, 25, shortInput), "explicit-discontinuity fixture starts");
    check(!process(discontinuous, 89, shortInput, true), "explicit discontinuity remains a refusal");
    check(discontinuous.failure()
        && discontinuous.failure()->reason == Extractor::FailureReason::Discontinuity
        && discontinuous.failure()->captureFirst == discontinuous.failure()->expectedCaptureFirst,
        "explicit discontinuity differs from a positional gap");

    Extractor invalid(config());
    shortInput[3] = {std::numeric_limits<float>::quiet_NaN(), 0};
    check(!process(invalid, 50, shortInput), "nonfinite input remains refused");
    const auto invalidFailure = invalid.failure();
    check(invalidFailure && invalidFailure->reason == Extractor::FailureReason::NonFiniteInput
        && invalidFailure->captureFirst == 50 && invalidFailure->captureFrames == 64
        && !invalidFailure->hasExpectedCaptureFirst && !invalidFailure->hasIqFirst,
        "pre-conversion failure does not invent an IQ or previous capture position");
    shortInput[3] = {};
    check(!process(invalid, 50, shortInput) && invalid.failure() == invalidFailure,
        "later finite input retains the nonfinite first cause");

    Extractor overflow(config());
    check(!process(overflow, std::numeric_limits<std::uint64_t>::max() - 31, shortInput),
        "capture end overflow remains refused");
    check(overflow.failure() && overflow.failure()->reason == Extractor::FailureReason::CapturePositionOverflow,
        "capture overflow has its own precise reason");

    Extractor empty(config());
    check(!process(empty, 10, {}) && empty.failure()
        && empty.failure()->reason == Extractor::FailureReason::EmptyInput,
        "empty input has a distinct first-failure reason");

    Extractor rejected(config());
    std::vector<std::complex<float>> input(65536);
    check(!process(rejected, 0, input) && rejected.withdrawn(), "sink rejection still withdraws immediately");
    const auto rejectedFailure = rejected.failure();
    check(rejectedFailure && rejectedFailure->reason == Extractor::FailureReason::SinkRejected
        && rejectedFailure->captureFirst == 0 && rejectedFailure->captureFrames == input.size()
        && rejectedFailure->hasIqFirst && rejectedFailure->iqFirst == 0 && rejectedFailure->iqFrames == 1024
        && rejected.outputFrames() == 0 && sink.calls == 1,
        "sink failure names the attempted full IQ block without counting it as emitted");
    check(!process(rejected, input.size(), shortInput) && rejected.failure() == rejectedFailure && sink.calls == 1,
        "withdrawal preserves sink cause and never calls it again");
}
int main()
{
    firstFailureObservation();
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
        const auto mismatch = changed.failure();
        check(mismatch && mismatch->reason == Extractor::FailureReason::CaptureMismatch,
              "non-withdrawing metadata rejection is observable");
        check(changed.process(config().capture, 0, input, output), "matching generation still works");
        check(changed.failure() == mismatch, "success does not reset the first observation");
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
