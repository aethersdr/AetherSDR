// data2g/polar.py: CRC-aided polar code with quasi-uniform puncturing, and
// its successive-cancellation list decoder.
//
// The Gaussian-approximation design (polar.ga_reliability) is not ported:
// submodes carry frozen info sets (codes::Spec::info_pos) and the CPM
// control codewords' GA design is frozen by (k, e) in tables::POLAR_GA.
// Nothing here knows about submodes: a spec's code is codes::polar_code.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "util/mat.hpp"

namespace data2g::polar {

// x <- x G mod 2 in place, G = F^(kron n), natural order; x.size() a power of 2.
void transform(std::span<std::uint8_t> x);

struct PolarCode {
    int k = 0, e = 0, n = 0;  // info bits incl. CRC, transmitted bits, mother length
    std::vector<std::uint16_t> info_pos;   // sorted
    std::vector<std::uint16_t> punctured;  // first n - e of bit-reversed order, sorted
    std::vector<std::uint16_t> sent;       // the rest, increasing
    std::vector<std::uint8_t> frozen;      // (n,), 1 = frozen

    PolarCode(int k, int e, std::span<const std::uint16_t> info_pos);

    // (B, k) bits 0/1 -> (B, e) coded bits.
    Mat<std::uint8_t> encode(const Mat<std::uint8_t>& bits) const;
};

// The frozen GA design for (k, e); throws std::out_of_range if there is none.
std::span<const std::uint16_t> ga_info_pos(int k, int e);

struct SclResult {
    Mat<std::uint8_t> paths;  // (B, L * k): path l of row b at [b][l * k], best metric first
    Mat<float> metric;        // (B, L), ascending (stable: ties keep path order)
};

// CA-SCL, float32 throughout as in numpy. decode() is const and allocates
// its own workspace, so one decoder may serve several threads.
class SCLDecoder {
public:
    explicit SCLDecoder(PolarCode code, int list_size = 8);
    const PolarCode& code() const { return code_; }
    int list_size() const { return L_; }

    // (B, e) LLRs (positive: bit 0) in code order.
    SclResult decode(const Mat<float>& llr) const;

private:
    PolarCode code_;
    int L_;
};

}  // namespace data2g::polar
