#include "core/backends/rtl/RtlViewport.h"
#include <cstdio>
#include <limits>

using AetherSDR::rtl::RtlViewport;
using AetherSDR::SharedCapturePolicy::CaptureDescriptor;
int main()
{
    int failures = 0;
    const auto check = [&](bool value, const char* message) {
        if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
    };
    const CaptureDescriptor capture{1, 1, 100'000'000, 2'400'000, 1'080'000, 1'080'000};
    const auto full = RtlViewport::fit(capture, 2048, 100'000'000, 3'000'000);
    check(full && full->firstBin == 103 && full->binCount == 1842
        && full->centerHz == 100'000'000 && full->spanHz == 2'158'593.75,
          "full view contains only real bins inside the usable capture");
    const auto narrow = RtlViewport::fit(capture, 2048, 100'000'000, 1);
    check(narrow && narrow->firstBin == 1016 && narrow->binCount == 16
        && narrow->centerHz == 100'000'000 && narrow->spanHz == 18'750,
          "zoom floor honestly exposes sixteen original FFT bins");
    const auto edge = RtlViewport::fit(capture, 2048, 900'000'000, 18'750);
    check(edge && edge->firstBin == 1929 && edge->binCount == 16
        && edge->centerHz + edge->spanHz / 2 == 101'079'296.875,
          "display pan clamps to the usable edge without inventing RF coverage");
    const auto offset = RtlViewport::fit(capture, 2048, 99'800'000, 200'000);
    check(offset && offset->firstBin == 768 && offset->binCount == 171
        && offset->centerHz == 99'800'195.3125 && offset->spanHz == 200'390.625,
          "off-center crop retains the original RF frequency of every FFT bin");
    check(RtlViewport::captureCenterFor(capture, 2048, 100'100'000, 200'000) == 100'000'000,
          "in-capture pan leaves hardware center fixed");
    check(RtlViewport::captureCenterFor(capture, 2048, 100'100'000, 3'000'000) == 100'100'000,
          "full-width drag follows the requested RF center");
    const auto beyond = RtlViewport::captureCenterFor(capture, 2048, 101'100'000, 200'000);
    check(beyond && *beyond > 100'000'000 && *beyond < 101'100'000,
          "narrow pan beyond the capture edge moves only enough to expose the requested window");
    for (double invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
        check(!RtlViewport::fit(capture, 2048, 100'000'000, invalid), "invalid span is refused");
    }
    check(!RtlViewport::fit(capture, 15, 100'000'000, 18'750), "undersized FFT cannot promise the zoom floor");
    auto invalid = capture; invalid.usableLeftHz = 2'400'000;
    check(!RtlViewport::fit(invalid, 2048, 100'000'000, 18'750), "invalid usable capture is refused");
    invalid = capture; invalid.achievedSampleRateHz = 1e-300;
    check(!RtlViewport::fit(invalid, 2048, 100'000'000, 18'750), "subnormal rate is refused before index arithmetic");
    std::printf("rtl_viewport_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
