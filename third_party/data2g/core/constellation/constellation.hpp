// data2g/constellation.py: point sets, mapping, bit LLRs, ACE projection.
// Point i carries i's bits, MSB first. Constellations are frozen tables
// (tables::CONSTELLATIONS), looked up by name.
#pragma once

#include <complex>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "tables/constellation.hpp"

namespace data2g::constellation {

using cd = std::complex<double>;
using Constellation = tables::Constellation;

// nullptr if there is no such constellation.
const Constellation* find(std::string_view name);

// bits (0/1, m per symbol, MSB first) -> points. Throws std::invalid_argument
// if the length is not a multiple of m or a bit is not 0/1.
std::vector<cd> modulate(std::span<const std::uint8_t> bits, const Constellation& c);

// Exact bit LLRs (log P0/P1) for y = h x + n, n ~ CN(0, var), points equally
// likely: m per symbol, symbol order. Same three paths as the reference (Gray
// QPSK closed form, Gray square QAM per axis, general), so the numerics agree.
std::vector<double> llr(std::span<const cd> y, std::span<const cd> h, std::span<const double> var,
                        const Constellation& c);

// ACE's projection of got onto the region around want that dirs (2 per cell,
// row-major (n, 2), unit or zero) allow.
std::vector<cd> ace_project(std::span<const cd> got, std::span<const cd> want, std::span<const cd> dirs);

}  // namespace data2g::constellation
