// The QC-LDPC base matrices (data2g/codes_data/ldpc_shifts.npz), compiled
// in: generated/ldpc.cpp.
#pragma once

#include <cstdint>
#include <span>

namespace data2g::tables {

// A QC-LDPC base matrix for base graph bg lifted by z: rows x cols
// circulant shifts, row-major, -1 = zero block (data2g/ldpc.py).
struct ShiftTable {
    int bg, z, rows, cols;
    std::span<const std::int16_t> shift;
};

extern const std::span<const ShiftTable> LDPC_SHIFTS;

}  // namespace data2g::tables
