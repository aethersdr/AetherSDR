#include "core/backends/rtl/HdFmIqAdapter.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <vector>

using Adapter = AetherSDR::rtl::HdFmIqAdapter;
namespace Policy = AetherSDR::SharedCapturePolicy;
static int failures = 0;
static void check(bool value, const char* text)
{ if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", text); } }
struct Sink final : Adapter::Sink {
    std::vector<std::complex<float>> data;
    bool hdIq(std::span<const float> input) noexcept override
    {
        if (input.empty() || input.size() % 2 || input.size() > 32768) { return false; }
        for (std::size_t n = 0; n < input.size(); n += 2) {
            if (!std::isfinite(input[n]) || !std::isfinite(input[n + 1])) { return false; }
            data.emplace_back(input[n], input[n + 1]);
        }
        return true;
    }
};
static double magnitude(std::span<const std::complex<float>> values, double hz)
{
    std::complex<double> sum{};
    for (std::size_t n = 0; n < values.size(); ++n) {
        const double phase = -2 * std::numbers::pi * hz * n / Adapter::kOutputRate;
        sum += std::complex<double>(values[n]) * std::complex<double>(std::cos(phase), std::sin(phase));
    }
    return std::abs(sum) / values.size();
}
int main()
{
    static_assert(Adapter::kOutputRate == 744187.5);
    constexpr std::array<int, 7> rates{900001, 1000000, 1536000, 1843200, 2000000, 2400000, 3000000};
    for (const int rate : rates) {
        const Policy::CaptureDescriptor capture{1, 1, 100e6, double(rate), 0.45 * rate, 0.45 * rate};
        const Policy::SliceDescriptor footprint{0, 100e6 + 65000, -225000, 225000, 0, 3000, 3000};
        std::vector<std::complex<float>> input(static_cast<std::size_t>(rate / 10));
        for (std::size_t n = 0; n < input.size(); ++n) {
            const double first = 2 * std::numbers::pi * (65000 + 195000) * n / rate;
            const double second = 2 * std::numbers::pi * (65000 - 195000) * n / rate;
            input[n] = std::complex<float>(0.2f * std::cos(first) + 0.3f * std::cos(second),
                0.2f * std::sin(first) + 0.3f * std::sin(second));
        }
        std::array<Sink, 2> sinks;
        for (std::size_t run = 0; run < 2; ++run) {
            Adapter adapter(capture, footprint);
            check(adapter.valid(), "full HD footprint admitted at every supported achieved rate");
            sinks[run].data.reserve(100000);
            constexpr std::array<std::size_t, 5> chunks{1, 17, 263, 997, 8192};
            bool accepted = true;
            for (std::size_t at = 0, block = 0; at < input.size(); ++block) {
                const std::size_t size = std::min(input.size() - at, run ? chunks[block % chunks.size()] : 8192);
                accepted = adapter.process(at, std::span(input).subspan(at, size), sinks[run]);
                if (!accepted) { break; }
                at += size;
            }
            check(accepted && adapter.valid(), "continuous arbitrary input partitions succeed");
            const double expected = input.size() * Adapter::kOutputRate / rate;
            check(adapter.outputFrames() == sinks[run].data.size()
                && adapter.outputFrames() <= expected + 1 && expected - adapter.outputFrames() < 4096,
                "exact fractional conversion count differs only by bounded converter staging");
        }
        check(sinks[0].data == sinks[1].data, "USB partitioning cannot alter the exact IQ sequence");
        if (sinks[0].data.size() > 10000) {
            const auto settled = std::span(sinks[0].data).subspan(4096);
            check(magnitude(settled, 195000) > 0.19 && magnitude(settled, -195000) > 0.29,
                "both outer HD sidebands survive translation before analog filtering");
            check(magnitude(settled, 100000) < 0.003, "adapter does not substitute in-band analog energy");
        } else { check(false, "converter produces nonzero observable output"); }
        std::printf("HD_IQ rate=%d output=%zu exactRate=744187.5 partitionEqual=%d\n", rate,
            sinks[0].data.size(), sinks[0].data == sinks[1].data);
    }
    const Policy::CaptureDescriptor capture{1, 1, 100e6, 1000000, 450000, 450000};
    const Policy::SliceDescriptor footprint{0, 100e6, -225000, 225000, 0, 3000, 3000};
    std::array<std::complex<float>, 256> input{}; Sink sink;
    Adapter gap(capture, footprint);
    check(gap.process(100, input, sink) && !gap.process(357, input, sink) && !gap.valid(),
        "a single missing capture sample terminally withdraws the adapter");
    Adapter nonfinite(capture, footprint); input[3] = {std::numeric_limits<float>::quiet_NaN(), 0};
    check(!nonfinite.process(0, input, sink), "nonfinite RF is refused before decoder submission");
    auto outside = footprint; outside.carrierHz += 230000;
    Adapter invalid(capture, outside);
    check(!invalid.valid(), "full sideband footprint plus guards must fit, regardless of analog width");
    std::printf("ALL PASS — %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
