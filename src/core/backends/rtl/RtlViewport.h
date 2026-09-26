#pragma once

#include "core/SharedCapturePolicy.h"
#include <algorithm>
#include <cmath>
#include <optional>

namespace AetherSDR::rtl {

// A display window is a contiguous set of genuine FFT bins. No interpolation,
// invented edge bins, amplitude correction, or capture/tuning state lives here.
struct RtlViewport {
    static constexpr int kRtlSpectrumBins = 65536;
    int firstBin = 0;
    int binCount = 0;
    int sourceBinCount = 0;
    double centerHz = 0;
    double spanHz = 0;
    double minimumSpanHz = 0;
    double maximumSpanHz = 0;

    static std::optional<RtlViewport> fit(const SharedCapturePolicy::CaptureDescriptor& capture,
                                         int bins, double center, double span)
    {
        if (!std::isfinite(center) || !std::isfinite(span) || span <= 0
            || !std::isfinite(capture.centerHz) || capture.centerHz < 0
            || capture.centerHz > SharedCapturePolicy::kMaxMagnitudeHz
            || !std::isfinite(capture.achievedSampleRateHz) || capture.achievedSampleRateHz < 1
            || capture.achievedSampleRateHz > SharedCapturePolicy::kMaxMagnitudeHz
            || !std::isfinite(capture.usableLeftHz) || !std::isfinite(capture.usableRightHz)
            || capture.usableLeftHz < 0 || capture.usableRightHz < 0
            || capture.usableLeftHz > capture.achievedSampleRateHz / 2
            || capture.usableRightHz > capture.achievedSampleRateHz / 2
            || bins < 16 || bins > 65536 || !capture.captureId || !capture.generation) {
            return {};
        }
        const double step = capture.achievedSampleRateHz / bins;
        const double origin = capture.centerHz - capture.achievedSampleRateHz / 2;
        const double low = std::max(0.0, capture.centerHz - capture.usableLeftHz);
        const double high = capture.centerHz + capture.usableRightHz;
        const int first = std::clamp(int(std::ceil((low - origin) / step)), 0, bins);
        const int end = std::clamp(int(std::floor((high - origin) / step)), 0, bins);
        if (end - first < 16) { return {}; }
        const int count = std::clamp(int(std::round(std::clamp(span / step, 16.0,
                                                             double(end - first)))), 16, end - first);
        const double boundedCenter = std::clamp(center,
            origin + (first + count / 2.0) * step,
            origin + (end - count / 2.0) * step);
        const int start = std::clamp(int(std::round((boundedCenter - origin) / step - count / 2.0)),
                                     first, end - count);
        return RtlViewport{start, count, bins, origin + (start + count / 2.0) * step,
                           count * step, 16 * step, (end - first) * step};
    }

    // Return the nearest capture placement that can show the requested real
    // FFT-bin window. A full-width view has one legal center, so any drag
    // moves capture. The transaction owner still validates hardware limits
    // and publishes only confirmed readback.
    static std::optional<double> captureCenterFor(
        const SharedCapturePolicy::CaptureDescriptor& capture,
        int bins, double center, double span)
    {
        const auto view = fit(capture, bins, center, span);
        if (!view) { return {}; }
        const double step = capture.achievedSampleRateHz / bins;
        const double origin = capture.centerHz - capture.achievedSampleRateHz / 2;
        const double low = std::max(0.0, capture.centerHz - capture.usableLeftHz);
        const double high = capture.centerHz + capture.usableRightHz;
        const int first = std::clamp(int(std::ceil((low - origin) / step)), 0, bins);
        const int end = std::clamp(int(std::floor((high - origin) / step)), 0, bins);
        const double minimum = origin + (first + view->binCount / 2.0) * step;
        const double maximum = origin + (end - view->binCount / 2.0) * step;
        return capture.centerHz + center - std::clamp(center, minimum, maximum);
    }
};

} // namespace AetherSDR::rtl
