// The constellations (data2g/constellation.py), compiled in:
// generated/constellation.cpp.
#pragma once

#include <array>  // generated/constellation.cpp's tables
#include <complex>
#include <span>
#include <string_view>

namespace data2g::tables {

// A constellation (data2g/constellation.py): 2^m points, point i carrying
// i's bits MSB first, and its ACE directions, row-major (2^m, 2): unit
// outward directions point i may move along (zero: none). Frozen because
// learned sets get theirs from scipy's ConvexHull.
struct Constellation {
    std::string_view name;
    int m;
    std::span<const std::complex<double>> points, ace;
};

// gray-qam4..256, then every data2g/constellations/*.npy by name.
extern const std::span<const Constellation> CONSTELLATIONS;

}  // namespace data2g::tables
