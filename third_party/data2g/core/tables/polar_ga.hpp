// Polar info sets Python designs at run time, compiled in:
// generated/polar_ga.cpp.
#pragma once

#include <cstdint>
#include <span>

namespace data2g::tables {

// codes.polar_code with no frozen file (the CPM control codewords), frozen
// here by (k, e).
struct PolarDesign {
    int k, e;
    std::span<const std::uint16_t> info_pos;
};
extern const std::span<const PolarDesign> POLAR_GA;

}  // namespace data2g::tables
