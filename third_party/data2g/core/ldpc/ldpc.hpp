// data2g/ldpc.py: quasi-cyclic LDPC on two base graphs, any (K, N), and its
// batched flooding decoder (sum-product, or normalized min-sum).
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "tables/ldpc.hpp"
#include "util/mat.hpp"

namespace data2g::ldpc {

inline constexpr int CORE = 4;
inline constexpr float CH_CLAMP = 16.0f;  // channel LLR cap at the decoder input
inline constexpr float BIG = 1e4f;        // LLR of filler bits (known zeros)

int kb_of(int bg);  // info columns: 22 (graph 1) or 10 (graph 2)
// (base graph, Z) for k bits in n; bg 0 chooses as ldpc.layout does.
std::pair<int, int> layout(int k, int n, int bg = 0);
const tables::ShiftTable* shift_table(int bg, int z);  // nullptr if none

class Code {
public:
    // Throws std::invalid_argument where QCLDPC raises ValueError.
    Code(const tables::ShiftTable& table, int k, int n);

    int z, kb, k, n, mb;  // mb: base rows kept for n

    int full_rows() const { return table_->rows; }
    int full_cols() const { return table_->cols; }
    int shift(int r, int c) const { return table_->shift[r * table_->cols + c]; }  // full base, -1 = zero
    int n_cols() const { return (kb + mb) * z; }
    const std::vector<int>& sent() const { return sent_; }  // transmitted columns, in order
    // The same code with more base rows: n transmitted bits (0: all).
    Code mother(int n = 0) const;
    // (check, variable) for every 1 in H, in QCLDPC.edges order.
    std::pair<std::vector<int>, std::vector<int>> edges() const;

    Mat<std::uint8_t> encode(const Mat<std::uint8_t>& bits) const;       // (B, k) -> (B, n)
    Mat<std::uint8_t> encode_full(const Mat<std::uint8_t>& bits) const;  // (B, k) -> (B, n_cols)
    std::vector<std::uint8_t> syndrome_ok(const Mat<std::uint8_t>& full) const;  // (B, n_cols) -> (B,)

private:
    struct CoreInv;
    const tables::ShiftTable* table_;
    std::vector<int> sent_;
    std::shared_ptr<CoreInv> core_;  // computed on first encode, shared with mother()
};

// ldpc._phi, -log(tanh(clip(x, 1e-7, 30) / 2)) in float32, in place.
void phi(std::span<float> v);

// Decoders use a single-precision phi by default: 1.6x faster, the same frame
// error rate. Exact uses phi() and matches the Python reference bit for bit
// (tests, paired native/Python runs); DATA2G_EXACT_PHI in the environment
// starts it on. Takes effect from the next decode; returns the old setting.
bool set_exact_phi(bool exact);

// Throws std::out_of_range if there is no shift table for the layout.
Code qc_code(int k, int n, int bg = 0);

struct Decoded {
    Mat<std::uint8_t> bits;  // (B, k) info bit decisions
    std::vector<std::uint8_t> ok;  // (B,) every check satisfied
    Mat<float> posterior;  // (B, n) a-posteriori LLRs of the sent bits, if asked
};

// Immutable after construction: decode is reentrant and thread-safe.
class Decoder {
public:
    explicit Decoder(const Code& code);

    // (B, n) channel LLRs of the sent bits. alpha empty: sum-product (BP),
    // row-layered as ldpc.Decoder (each base row's Z checks a layer); one
    // value: min-sum normalized by it, flooding; more: alpha[it] per iteration.
    // Stops once every codeword in the batch satisfies H, as ldpc.Decoder.
    Decoded decode(const Mat<float>& llr, int iters = 30, std::span<const float> alpha = {},
                   bool posterior = false) const;

private:
    int k_, n_cols_, filler_end_, n_checks_, dmax_, layers_ = 0;  // layers_: base rows, Z checks each
    std::vector<int> sent_;
    std::vector<int> var_;      // edge -> variable; edges in check order
    std::vector<int> chk_ptr_;  // check -> its first edge
    std::vector<int> var_ptr_, var_edges_;  // variable -> its edges, ascending
};

}  // namespace data2g::ldpc
