#include "ldpc/ldpc.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <array>
#include <bit>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "util/cancel.hpp"
#include "util/pool.hpp"
#include "util/simd.hpp"

#ifdef __clang__
// Clang contracts a*b+c into FMA by default where the target has it
// (aarch64); the float32 sums below must round exactly as numpy's.
#pragma STDC FP_CONTRACT OFF
#endif

namespace data2g::ldpc {

int kb_of(int bg) {
    if (bg != 1 && bg != 2) throw std::invalid_argument("base graph must be 1 or 2");
    return bg == 1 ? 22 : 10;
}

namespace {

// ldpc.LIFTING_SIZES: a * 2^j up to 384, a in {2, 3, 5, ..., 15}, j < 8.
bool lifting_size(int z) {
    for (int a : {2, 3, 5, 7, 9, 11, 13, 15})
        for (int j = 0; j < 8; ++j)
            if ((a << j) == z) return true;
    return false;
}

}  // namespace

// ldpc.layout: base graph by size and rate, graph 2's k_b by k.
std::pair<int, int> layout(int k, int n, int bg) {
    const double rate = static_cast<double>(k) / n;
    if (bg == 0) bg = (k <= 292 || (k <= 3824 && rate <= 0.67) || rate <= 0.25) ? 2 : 1;
    const int kb_z = bg == 1 ? 22 : k > 640 ? 10 : k > 560 ? 9 : k > 192 ? 8 : 6;
    for (int z = 1; z <= 384; ++z)
        if (lifting_size(z) && kb_z * z >= k) return {bg, z};
    throw std::invalid_argument("k=" + std::to_string(k) + " exceeds every lifting size");
}

const tables::ShiftTable* shift_table(int bg, int z) {
    for (const auto& t : tables::LDPC_SHIFTS)
        if (t.bg == bg && t.z == z) return &t;
    return nullptr;
}

Code qc_code(int k, int n, int bg) {
    const auto [g, z] = layout(k, n, bg);
    const auto* t = shift_table(g, z);
    if (!t)
        throw std::out_of_range("no shift table for base graph " + std::to_string(g) + ", Z=" + std::to_string(z) +
                                " (k=" + std::to_string(k) + ")");
    return Code(*t, k, n);
}

// --- code -------------------------------------------------------------------

// The inverse of the 4Z x 4Z dual-diagonal core over GF(2), rows packed in
// 64-bit words. Depends only on the shift table, so mother() shares it.
struct Code::CoreInv {
    std::once_flag once;
    int words = 0;
    std::vector<std::uint64_t> inv;
};

Code::Code(const tables::ShiftTable& table, int k_, int n_)
    : z(table.z), kb(kb_of(table.bg)), k(k_), n(n_), mb(0), table_(&table), core_(std::make_shared<CoreInv>()) {
    if (!(0 < k && k <= kb * z))
        throw std::invalid_argument("k=" + std::to_string(k) + " does not fit kb*z=" + std::to_string(kb * z));
    const int need_parity = n - (k - 2 * z);
    if (need_parity <= 0) throw std::invalid_argument("n too small: not even the info bits fit");
    mb = std::max(CORE, (need_parity + z - 1) / z);
    if (mb > table.rows) throw std::invalid_argument("rate below this base graph's mother rate");
    for (int col = 2 * z; col < n_cols() && static_cast<int>(sent_.size()) < n; ++col)
        if (col < k || col >= kb * z) sent_.push_back(col);
}

Code Code::mother(int n_) const {
    const int n_all = (full_cols() - 2) * z - (kb * z - k);
    Code m(*table_, k, std::min(n_ ? n_ : n_all, n_all));
    m.core_ = core_;
    return m;
}

std::pair<std::vector<int>, std::vector<int>> Code::edges() const {
    std::pair<std::vector<int>, std::vector<int>> out;
    for (int r = 0; r < mb; ++r)
        for (int c = 0; c < kb + mb; ++c)
            if (const int s = shift(r, c); s >= 0)
                for (int i = 0; i < z; ++i) {
                    out.first.push_back(r * z + i);
                    out.second.push_back(c * z + (i + s) % z);
                }
    return out;
}

namespace {

// out[i] ^= in[(i + s) mod z] for i < z: one circulant block times x.
void xor_shifted(std::uint8_t* out, const std::uint8_t* in, int z, int s) {
    for (int i = 0; i < z - s; ++i) out[i] ^= in[i + s];
    for (int i = z - s; i < z; ++i) out[i] ^= in[i + s - z];
}

}  // namespace

Mat<std::uint8_t> Code::encode_full(const Mat<std::uint8_t>& bits) const {
    if (static_cast<int>(bits.cols) != k) throw std::invalid_argument("expected (B, k) info bits");
    const int nc = 4 * z, words = (nc + 63) / 64;
    std::call_once(core_->once, [&] {
        // Gauss-Jordan on [core | I]: each half `words` wide.
        const int w2 = 2 * words;
        std::vector<std::uint64_t> m(static_cast<std::size_t>(nc) * w2, 0);
        auto set = [&](int row, int col) { m[row * w2 + col / 64] ^= std::uint64_t{1} << (col % 64); };
        for (int r = 0; r < CORE; ++r)
            for (int c = 0; c < CORE; ++c)
                if (const int s = shift(r, kb + c); s >= 0)
                    for (int i = 0; i < z; ++i) set(r * z + i, c * z + (i + s) % z);
        for (int i = 0; i < nc; ++i) set(i, words * 64 + i);
        auto bit = [&](int row, int col) { return (m[row * w2 + col / 64] >> (col % 64)) & 1; };
        for (int c = 0; c < nc; ++c) {
            int piv = c;
            while (piv < nc && !bit(piv, c)) ++piv;
            if (piv == nc) throw std::runtime_error("core not invertible");
            if (piv != c) std::swap_ranges(m.begin() + piv * w2, m.begin() + (piv + 1) * w2, m.begin() + c * w2);
            for (int r = 0; r < nc; ++r)
                if (r != c && bit(r, c))
                    for (int w = 0; w < w2; ++w) m[r * w2 + w] ^= m[c * w2 + w];
        }
        core_->words = words;
        core_->inv.resize(static_cast<std::size_t>(nc) * words);
        for (int r = 0; r < nc; ++r)
            std::copy_n(m.begin() + r * w2 + words, words, core_->inv.begin() + r * words);
    });

    Mat<std::uint8_t> out(bits.rows, n_cols(), 0);
    std::vector<std::uint8_t> a_s(nc);
    std::vector<std::uint64_t> packed(words);
    for (std::size_t b = 0; b < bits.rows; ++b) {
        std::uint8_t* o = out[b];
        std::copy_n(bits[b], k, o);
        std::fill(a_s.begin(), a_s.end(), 0);
        for (int r = 0; r < CORE; ++r)
            for (int c = 0; c < kb; ++c)
                if (const int s = shift(r, c); s >= 0) xor_shifted(&a_s[r * z], o + c * z, z, s);
        std::fill(packed.begin(), packed.end(), 0);
        for (int i = 0; i < nc; ++i) packed[i / 64] |= std::uint64_t{a_s[i] & 1u} << (i % 64);
        for (int j = 0; j < nc; ++j) {
            int par = 0;
            for (int w = 0; w < words; ++w) par ^= std::popcount(core_->inv[j * words + w] & packed[w]);
            o[kb * z + j] = static_cast<std::uint8_t>(par & 1);
        }
        // Extension rows: one identity parity column each, fixed by the
        // info and core columns alone (QCLDPC.encode_full).
        for (int r = CORE; r < mb; ++r)
            for (int c = 0; c < kb + CORE; ++c)
                if (const int s = shift(r, c); s >= 0) xor_shifted(o + (kb + r) * z, o + c * z, z, s);
    }
    return out;
}

Mat<std::uint8_t> Code::encode(const Mat<std::uint8_t>& bits) const {
    const auto full = encode_full(bits);
    Mat<std::uint8_t> out(bits.rows, n);
    for (std::size_t b = 0; b < bits.rows; ++b)
        for (int j = 0; j < n; ++j) out[b][j] = full[b][sent_[j]];
    return out;
}

std::vector<std::uint8_t> Code::syndrome_ok(const Mat<std::uint8_t>& full) const {
    if (static_cast<int>(full.cols) != n_cols()) throw std::invalid_argument("expected (B, n_cols) codewords");
    std::vector<std::uint8_t> ok(full.rows, 1);
    std::vector<std::uint8_t> syn(z);
    for (std::size_t b = 0; b < full.rows; ++b)
        for (int r = 0; r < mb && ok[b]; ++r) {
            std::fill(syn.begin(), syn.end(), 0);
            for (int c = 0; c < kb + mb; ++c)
                if (const int s = shift(r, c); s >= 0) xor_shifted(syn.data(), full[b] + c * z, z, s);
            ok[b] = std::all_of(syn.begin(), syn.end(), [](auto v) { return (v & 1) == 0; });
        }
    return ok;
}

// --- decoder ----------------------------------------------------------------

namespace {

// ph.sum(axis=-1) in ldpc.Decoder, over a check's n = dmax slots: a[0..d)
// real, a[d..n) zero padding. numpy's order depends on the batch size:
// v2c[:, chk] comes out batch-innermost for B > 1, so numpy adds the slots
// in turn; for B = 1 the slots are contiguous and it sums pairwise (8
// accumulators from n = 8). So a codeword's soft values in Python depend
// on what it was batched with, and here likewise.
float numpy_sum(const float* a, int d, int n, bool pairwise) {
    if (!pairwise || n < 8) {
        float res = 0.0f;
        for (int i = 0; i < d; ++i) res += a[i];
        return res;
    }
    auto val = [&](int i) { return i < d ? a[i] : 0.0f; };
    float r[8];
    for (int j = 0; j < 8; ++j) r[j] = val(j);
    int i = 8;
    for (; i < n - n % 8; i += 8)
        for (int j = 0; j < 8; ++j) r[j] += val(i + j);
    float res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
    for (; i < n; ++i) res += val(i);
    return 0.0f + res;
}

constexpr double LN2 = 0x1.62e42fefa39efp-1;
constexpr double LN2_HI = 0x1.62e42fee00000p-1, LN2_LO = 0x1.a39ef35793c76p-33;
constexpr double SHIFTER = 0x1.8p52;  // adding it rounds to an integer, held in the low bits

// ldpc._phi, -log(tanh(clip(x, 1e-7, 30) / 2)), with numpy's float32
// roundings: the clipped x, x / 2, tanh's result and log's result are each
// a float. tanh and log are evaluated in double, so each float is
// correctly rounded (test_ldpc checks how often); numpy's own SIMD float32
// tanh and log are not (about 1 result in 5 differs by an ULP). In place over
// v[0..n), branch-free and written into the loop so it vectorizes at -O2.
// Most of a decode: the AVX-512 clone is 1.5x the AVX2 one on the whole
// decode, bit for bit (checked over every float32).
DATA2G_SIMD_CLONES void phi_all(float* __restrict v, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        // np.clip(x, 1e-7, 30), NaN passing through. Selects are bitwise
        // throughout: GCC turns ?: into branches here and stops vectorizing.
        auto pick = [](bool c, auto a, auto b) {
            using U = std::conditional_t<sizeof(a) == 8, std::uint64_t, std::uint32_t>;
            const U mask = -static_cast<U>(c);
            return std::bit_cast<decltype(a)>((std::bit_cast<U>(a) & mask) | (std::bit_cast<U>(b) & ~mask));
        };
        float x = v[i];
        x = pick(x < 1e-7f, 1e-7f, x);
        x = pick(x > 30.0f, 30.0f, x);
        const double h = x * 0.5f;
        // tanh(h) = (1 - e) / (1 + e), e = exp(-2h) = 2^-k exp(-r)
        const double xd = 2.0 * h;
        const double m = xd * (1.0 / LN2) + SHIFTER;
        const double kd = m - SHIFTER;
        const double u = (kd * LN2_HI - xd) + kd * LN2_LO;  // -r, |r| <= ln2 / 2
        // exp(u), Taylor to u^10 / 10!: relative error < 3e-13
        const double p = 1 + u * (1 + u * (1. / 2 + u * (1. / 6 + u * (1. / 24 + u * (1. / 120 + u * (1. / 720 +
                         u * (1. / 5040 + u * (1. / 40320 + u * (1. / 362880 + u * (1. / 3628800))))))))));
        const double scale = std::bit_cast<double>((std::uint64_t{1023} << 52) - (std::bit_cast<std::uint64_t>(m) << 52));
        const double e = scale * p;
        const double t_exp = (1.0 - e) / (1.0 + e);
        // Small h, where 1 - e cancels: the series to h^9 (error < h^11 / 100).
        const double h2 = h * h;
        const double t_ser = h * (1.0 + h2 * (-1.0 / 3 + h2 * (2.0 / 15 + h2 * (-17.0 / 315 + h2 * (62.0 / 2835)))));
        const float t = static_cast<float>(pick(h < 0.0625, t_ser, t_exp));
        // log(t) = k ln2 + log(mant), mant in [sqrt(1/2), sqrt(2)): 2 atanh(s),
        // series to s^13 (relative error < 2e-12).
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(t) - 0x3f3504f3u;
        const int k = static_cast<std::int32_t>(bits) >> 23;
        const double mant = std::bit_cast<float>((bits & 0x7fffffu) + 0x3f3504f3u);
        const double s = (mant - 1.0) / (mant + 1.0), s2 = s * s;
        const double q = 1 + s2 * (1. / 3 + s2 * (1. / 5 + s2 * (1. / 7 + s2 * (1. / 9 + s2 * (1. / 11 + s2 * (1. / 13))))));
        v[i] = static_cast<float>(-(k * LN2 + 2.0 * s * q));
    }
}

// phi_all in single precision, the decoders' default (set_exact_phi): the same -log(tanh(x / 2)) with the
// polynomials cut to what a float result needs (relative error about 1e-6 against phi_all), four lanes on
// NEON and SSE rather than two, and about half the arithmetic: a decode is 1.6x faster on x86-64, with the
// same frame error rate (1.6M codewords, 6 modes, AWGN/MPP/MPD waterfalls and floors: 11 vs 13 codewords
// lost by one side only, out of 61.5k errors). The decoder is receiver-only, so this changes nothing on
// the wire; it does give up phi_all's bit-for-bit match with the Python reference.
DATA2G_SIMD_CLONES void phi_all_fast(float* __restrict v, std::size_t n) {
    constexpr float LOG2E = 1.442695041f, LN2_HI_F = 0.693145752f, LN2_LO_F = 1.42860677e-6f, SHIFT_F = 12582912.0f;
    constexpr float LN2_F = 0.693147182f;
    for (std::size_t i = 0; i < n; ++i) {
        auto pick = [](bool c, auto a, auto b) {
            using U = std::conditional_t<sizeof(a) == 8, std::uint64_t, std::uint32_t>;
            const U mask = -static_cast<U>(c);
            return std::bit_cast<decltype(a)>((std::bit_cast<U>(a) & mask) | (std::bit_cast<U>(b) & ~mask));
        };
        float x = v[i];
        x = pick(x < 1e-7f, 1e-7f, x);
        x = pick(x > 30.0f, 30.0f, x);
        const float h = x * 0.5f;
        // e = exp(-x) = 2^-k exp(-r), r = x - k ln2, |r| <= ln2 / 2
        const float m = x * LOG2E + SHIFT_F;
        const float kf = m - SHIFT_F;
        const float r = (x - kf * LN2_HI_F) - kf * LN2_LO_F;
        const float p = 1.0f + r * (-1.0f + r * (1.0f / 2 + r * (-1.0f / 6 + r * (1.0f / 24 + r * (-1.0f / 120 + r * (1.0f / 720))))));
        const std::int32_t k = static_cast<std::int32_t>(kf);
        const float e = std::bit_cast<float>(static_cast<std::uint32_t>(127 - k) << 23) * p;
        const float t_exp = (1.0f - e) / (1.0f + e);
        const float h2 = h * h;
        const float t_ser = h * (1.0f + h2 * (-1.0f / 3 + h2 * (2.0f / 15 + h2 * (-17.0f / 315))));
        const float t = pick(h < 0.125f, t_ser, t_exp);
        // log(t) = k ln2 + 2 atanh(s), mant in [sqrt(1/2), sqrt(2))
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(t) - 0x3f3504f3u;
        const int kk = static_cast<std::int32_t>(bits) >> 23;
        const float mant = std::bit_cast<float>((bits & 0x7fffffu) + 0x3f3504f3u);
        const float sv = (mant - 1.0f) / (mant + 1.0f), s2 = sv * sv;
        const float q = 1.0f + s2 * (1.0f / 3 + s2 * (1.0f / 5 + s2 * (1.0f / 7 + s2 * (1.0f / 9))));
        v[i] = -(static_cast<float>(kk) * LN2_F + 2.0f * sv * q);
    }
}

std::atomic<bool> exact_phi{std::getenv("DATA2G_EXACT_PHI") != nullptr};

// Variable-to-check messages: m = tot[var[e]] - c2v[e], as sign and size.
DATA2G_SIMD_CLONES void v2c(const float* __restrict t, const float* __restrict c, const int* __restrict var,
                           std::size_t n, std::uint8_t* __restrict neg, float* __restrict mag) {
    for (std::size_t e = 0; e < n; ++e) {
        const float m = t[var[e]] - c[e];
        neg[e] = m < 0;
        mag[e] = std::fabs(m);
    }
}

}  // namespace

void phi(std::span<float> v) { phi_all(v.data(), v.size()); }

bool set_exact_phi(bool exact) { return exact_phi.exchange(exact); }

Decoder::Decoder(const Code& code)
    : k_(code.k), n_cols_(code.n_cols()), filler_end_(code.kb * code.z), n_checks_(code.mb * code.z), dmax_(0),
      sent_(code.sent()) {
    const int z = code.z;
    chk_ptr_.push_back(0);
    for (int r = 0; r < code.mb; ++r)
        for (int i = 0; i < z; ++i) {
            for (int c = 0; c < code.kb + code.mb; ++c)
                if (const int s = code.shift(r, c); s >= 0) var_.push_back(c * z + (i + s) % z);
            chk_ptr_.push_back(static_cast<int>(var_.size()));
            dmax_ = std::max(dmax_, chk_ptr_.back() - chk_ptr_[chk_ptr_.size() - 2]);
        }
    if (dmax_ > 128) throw std::invalid_argument("check degree above numpy's pairwise block");
    layers_ = code.mb;
    var_ptr_.assign(n_cols_ + 1, 0);
    for (int v : var_) ++var_ptr_[v + 1];
    for (int v = 0; v < n_cols_; ++v) var_ptr_[v + 1] += var_ptr_[v];
    var_edges_.resize(var_.size());
    std::vector<int> fill(var_ptr_.begin(), var_ptr_.end() - 1);
    for (int e = 0; e < static_cast<int>(var_.size()); ++e) var_edges_[fill[var_[e]]++] = e;
}

Decoded Decoder::decode(const Mat<float>& llr, int iters, std::span<const float> alpha, bool posterior) const {
    if (llr.cols != sent_.size()) throw std::invalid_argument("expected (B, n) LLRs");
    if (iters < 1) throw std::invalid_argument("iters must be >= 1");
    if (alpha.size() > 1 && alpha.size() < static_cast<std::size_t>(iters))
        throw std::invalid_argument("alpha: empty, one value, or one per iteration");
    const std::size_t B = llr.rows, E = var_.size();
    Mat<float> ch(B, n_cols_, 0.0f), tot(B, n_cols_), c2v(B, E, 0.0f);
    for (std::size_t b = 0; b < B; ++b) {
        for (std::size_t j = 0; j < sent_.size(); ++j) ch[b][sent_[j]] = std::min(std::max(llr[b][j], -CH_CLAMP), CH_CLAMP);
        std::fill(ch[b] + k_, ch[b] + filler_end_, BIG);
    }
    // ch + gather @ c2v as scipy's csr product sums it: each variable's
    // messages in edge order, from 0, then added to ch.
    auto total = [&](std::size_t b) {
        const float* c = c2v[b];
        for (int v = 0; v < n_cols_; ++v) {
            float s = 0.0f;
            for (int j = var_ptr_[v]; j < var_ptr_[v + 1]; ++j) s += c[var_edges_[j]];
            tot[b][v] = ch[b][v] + s;
        }
    };
    for (std::size_t b = 0; b < B; ++b) total(b);

    // Codewords are independent within an iteration: one pool task each.
    // Only the stop (every codeword at once) couples them, as in Python.
    struct Scratch {
        std::vector<float> m, mag, ph;
        std::vector<std::uint8_t> neg, hard;
    };
    const int z = n_checks_ / layers_;
    std::vector<std::uint8_t> ok(B, 0);
    const auto phi_fn = exact_phi.load(std::memory_order_relaxed) ? phi_all : phi_all_fast;
    for (int it = 0; it < iters; ++it) {
        cancel::check();
        const bool bp = alpha.empty();
        const float a = bp ? 0.0f : alpha.size() == 1 ? alpha[0] : alpha[static_cast<std::size_t>(it)];
        pool::parallel_for(B, [&](std::size_t b) {
            cancel::check();
            thread_local Scratch w;
            w.m.resize(E);
            w.mag.resize(E);
            w.ph.resize(E);
            w.neg.resize(E);
            w.hard.resize(n_cols_);
            auto& mag = w.mag;
            auto& ph = w.ph;
            auto& neg = w.neg;
            auto& hard = w.hard;
            float* c = c2v[b];
            float* t = tot[b];
            if (bp) {
                // ldpc.Decoder._layered: each base row's Z checks (a contiguous edge range, each variable at most
                // once) from the totals less their old messages, then the totals take the new ones at once
                for (int l = 0; l < layers_; ++l) {
                    const int lo = chk_ptr_[l * z], hi = chk_ptr_[(l + 1) * z], n = hi - lo;
                    for (int e = lo; e < hi; ++e) w.m[e] = t[var_[e]] - c[e];
                    for (int e = lo; e < hi; ++e) {
                        neg[e] = w.m[e] < 0;
                        mag[e] = std::fabs(w.m[e]);
                    }
                    std::copy(mag.begin() + lo, mag.begin() + hi, ph.begin() + lo);
                    phi_fn(ph.data() + lo, static_cast<std::size_t>(n));
                    for (int q = l * z; q < (l + 1) * z; ++q) {
                        const int e0 = chk_ptr_[q], d = chk_ptr_[q + 1] - e0;
                        const float S = numpy_sum(&ph[e0], d, dmax_, B == 1);
                        std::uint8_t sneg = 0;
                        for (int e = e0; e < e0 + d; ++e) sneg ^= neg[e];
                        for (int e = e0; e < e0 + d; ++e) {
                            mag[e] = S - ph[e];
                            neg[e] ^= sneg;
                        }
                    }
                    phi_fn(mag.data() + lo, static_cast<std::size_t>(n));
                    // -x is x with the sign bit flipped: spelled so, since the
                    // ?: became a branch mispredicted on half the edges (55% of
                    // a failing decode)
                    for (int e = lo; e < hi; ++e) {
                        c[e] = std::bit_cast<float>(std::bit_cast<std::uint32_t>(mag[e]) ^ (std::uint32_t{neg[e]} << 31));
                        t[var_[e]] = w.m[e] + c[e];
                    }
                }
            } else {
                v2c(t, c, var_.data(), E, neg.data(), mag.data());
                // Normalized min-sum over the check's dmax slots, padding
                // reading BIG; ties go to the first slot (argmin).
                for (int q = 0; q < n_checks_; ++q) {
                    const int e0 = chk_ptr_[q], d = chk_ptr_[q + 1] - e0;
                    float min1 = INFINITY, min2 = INFINITY;
                    int i1 = 0;
                    std::uint8_t sneg = 0;
                    for (int j = 0; j < dmax_; ++j) {
                        const float v = j < d ? mag[e0 + j] : BIG;
                        if (v < min1) {
                            min2 = min1;
                            min1 = v;
                            i1 = j;
                        } else if (v < min2) {
                            min2 = v;
                        }
                        if (j < d) sneg ^= neg[e0 + j];
                    }
                    for (int j = 0; j < d; ++j) {
                        const float out = a * (j == i1 ? min2 : min1);
                        c[e0 + j] = (neg[e0 + j] ^ sneg) ? -out : out;
                    }
                }
                total(b);
            }
            for (int v = 0; v < n_cols_; ++v) hard[v] = tot[b][v] < 0;
            bool good = true;
            for (int q = 0; q < n_checks_ && good; ++q) {
                std::uint8_t par = 0;
                for (int e = chk_ptr_[q]; e < chk_ptr_[q + 1]; ++e) par ^= hard[var_[e]];
                good = par == 0;
            }
            ok[b] = good;
        });
        if (std::all_of(ok.begin(), ok.end(), [](std::uint8_t v) { return v != 0; })) break;
    }

    Decoded out{Mat<std::uint8_t>(B, k_), std::move(ok), {}};
    for (std::size_t b = 0; b < B; ++b)
        for (int j = 0; j < k_; ++j) out.bits[b][j] = tot[b][j] < 0;
    if (posterior) {
        out.posterior = Mat<float>(B, sent_.size());
        for (std::size_t b = 0; b < B; ++b)
            for (std::size_t j = 0; j < sent_.size(); ++j) out.posterior[b][j] = tot[b][sent_[j]];
    }
    return out;
}

}  // namespace data2g::ldpc
