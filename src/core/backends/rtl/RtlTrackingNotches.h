#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace AetherSDR::rtl {
struct RtlTrackingNotch {
    int id = -1;
    double centerHz = 0;
    double widthHz = 200;
    bool active = true;
    bool operator==(const RtlTrackingNotch&) const = default;
};
struct RtlTrackingNotches {
    static constexpr int kCapacity = 32;
    bool enabled = true;
    std::vector<RtlTrackingNotch> entries;
    bool operator==(const RtlTrackingNotches&) const = default;
    bool valid() const noexcept
    {
        if (entries.size() > kCapacity) { return false; }
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const auto& notch = entries[index];
            if (notch.id < 0 || !std::isfinite(notch.centerHz)
                || notch.centerHz < 24000 || notch.centerHz > 1766000000
                || !std::isfinite(notch.widthHz) || notch.widthHz < 200 || notch.widthHz > 12000) { return false; }
            for (std::size_t earlier = 0; earlier < index; ++earlier) {
                if (entries[earlier].id == notch.id) { return false; }
            }
        }
        return true;
    }
};
} // namespace AetherSDR::rtl
