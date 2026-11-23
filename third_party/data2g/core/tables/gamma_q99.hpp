// The equalizer's per-carrier noise quantiles, compiled in: generated/gamma_q99.cpp.
#pragma once

#include <array>

namespace data2g::tables {

// scipy.stats.gamma.ppf(0.99, n) / n at index n - 1 (equalizer.per_carrier_noise).
inline constexpr int GAMMA_Q99_MAX = 2048;
extern const std::array<double, GAMMA_Q99_MAX> GAMMA_Q99;

}  // namespace data2g::tables
