// data2g/gfldpc.py: nonbinary LDPC over GF(2^p) (the CPM modes' data code,
// one symbol per M-FSK tone) and its FFT-BP decoder on symbol
// likelihoods. The codes are generated tables (tables::CpmSpec's gf_*,
// from data2g/codes_data/gf_codes.npz); the echelon encoder is built here
// as gfldpc.GFCode builds it, so both pick the same information set.
// Parity: tests/test_native_codes.py (through codes::), native/tests/test_gfldpc.cpp.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "util/mat.hpp"

namespace data2g::gfldpc {

// GF(2^p), p = 3, 4 or 5: mul (q * q, row-major) and inv (q).
struct GF {
    int p, q;
    std::vector<std::uint8_t> mul, inv;
    explicit GF(int p);
};

// H as padded rows (gfldpc.GFCode): var (m * dmax) a slot's symbol, -1
// padding at the end; h its coefficient. The core code is the first n0
// symbols and the rows within them; every other row an extension row,
// one symbol of its own past n0, in order.
class Code {
public:
    Code(int p, int n, int n0, int dmax, std::span<const std::int16_t> var, std::span<const std::uint8_t> h);

    int p, q, n, n0, m, dmax;
    std::vector<int> var;
    std::vector<std::uint8_t> h;

    const GF& gf() const { return gf_; }
    int k() const { return static_cast<int>(info_.size()); }
    const std::vector<int>& info() const { return info_; }  // information symbols' positions, ascending
    // The code on its first n symbols: the rows that reach no further (gfldpc.cut).
    Code cut(int n) const;
    Mat<std::uint8_t> encode(const Mat<std::uint8_t>& u) const;  // (B, k) -> (B, n) symbols
    bool syndrome_ok(const std::uint8_t* x) const;  // n symbols

private:
    Code(const Code& c, int n);
    void build();
    GF gf_;
    std::vector<int> info_, parity_;
    std::vector<std::uint8_t> a_;  // (parity, k): parity[i] = sum_j a[i][j] info[j]
    std::vector<int> ext_rows_;    // extension rows, by their own symbol n0, n0 + 1, ...
};

struct Decoded {
    Mat<std::uint8_t> hard;  // (B, n) symbol decisions
    std::vector<std::uint8_t> ok;  // (B,) every check holds
};

// Immutable after construction: decode is reentrant.
class Decoder {
public:
    explicit Decoder(const Code& code);
    const Code& code() const { return code_; }
    // (B, n * q) symbol log-likelihoods (any per-symbol offset), flooding
    // FFT-BP as gfldpc.Decoder, each codeword stopping once its checks hold.
    Decoded decode(const Mat<float>& ll, int iters) const;

private:
    Code code_;
    std::vector<int> col_ptr_, col_edges_;  // symbol -> its edges (row-major slot ids)
};

// (B, nbits) bits -> (B, k) symbols, p bits each MSB first, zero filled; and back.
Mat<std::uint8_t> pack(const Mat<std::uint8_t>& bits, int p, int k);
Mat<std::uint8_t> unpack(const Mat<std::uint8_t>& x, int p);

}  // namespace data2g::gfldpc
