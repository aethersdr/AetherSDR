#pragma once

#include <algorithm>
#include <array>
#include <ranges>

namespace AetherSDR {

// Standard 104-code DTCS/DCS set used by the IC-9700. Values are decimal
// representations of the three displayed octal digits: 023 is stored as 23,
// while formatting restores the significant leading zero for the operator.
inline constexpr std::array<int, 104> kDtcsCodes{{
    23, 25, 26, 31, 32, 36, 43, 47, 51, 53, 54, 65, 71, 72, 73, 74,
    114, 115, 116, 122, 125, 131, 132, 134, 143, 145, 152, 155, 156, 162,
    165, 172, 174, 205, 212, 223, 225, 226, 243, 244, 245, 246, 251, 252,
    255, 261, 263, 265, 266, 271, 274, 306, 311, 315, 325, 331, 332, 343,
    346, 351, 356, 364, 365, 371, 411, 412, 413, 423, 431, 432, 445, 446,
    452, 454, 455, 462, 464, 465, 466, 503, 506, 516, 523, 526, 532, 546,
    565, 606, 612, 624, 627, 631, 632, 654, 662, 664, 703, 712, 723, 731,
    732, 734, 743, 754,
}};

[[nodiscard]] inline bool isCanonicalDtcsCode(int code) noexcept
{
    return std::ranges::find(kDtcsCodes, code) != kDtcsCodes.end();
}

} // namespace AetherSDR
