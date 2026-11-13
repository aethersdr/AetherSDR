#include "polar/polar.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

#include "tables/polar_ga.hpp"
#include "util/pool.hpp"

// Built with -O3 under GCC (native/CMakeLists.txt): at -O2 its very-cheap
// vectorizer cost model leaves the f and g loops scalar; -O3 vectorizes
// them, 1.7x on the whole decode. Clang and MSVC vectorize at -O2 already.

namespace data2g::polar {

namespace {

// np.logaddexp(float32(0), x): numpy's npy_logaddexpf with x = 0, bitwise.
// The two tails skip work without changing a bit: for x >= 16, exp(-x) is
// under half an ulp of x; for x <= -17, y = exp(x) < 2^-24, where log1p(y)
// rounds to y. Checked exhaustively over every float against glibc.
inline float softplus(float x) {
    constexpr float LOGE2 = 0.693147180559945309417232121458176568f;
    if (x >= 16.0f) return x;
    if (x <= -17.0f) return std::exp(x);
    if (x == 0.0f) return LOGE2;
    if (x > 0.0f) return x + std::log1p(std::exp(-x));
    if (x < 0.0f) return std::log1p(std::exp(x));
    return -x;  // NaN
}

// SCLDecoder._f, sign(a) sign(b) min(|a|, |b|), branch-free so it vectorizes.
// Same value and NaN propagation as numpy's; a zero may carry the other
// sign, which nothing downstream can see (softplus(+-0) and b +- 0 agree).
inline float f_op(float a, float b) {
    const float fa = std::fabs(a), fb = std::fabs(b);
    float m = fb < fa ? fb : fa;
    m = fb != fb ? fb : m;
    return std::copysign(m, a * b);  // a b keeps the product's sign through overflow and underflow
}

// numpy's sort order: NaN last.
inline bool less(float a, float b) { return a < b || (b != b && a == a); }

// Stable argsort of v[0..n) into idx (insertion sort: n is at most 2L).
inline void argsort(const float* v, int n, int* idx) {
    for (int i = 0; i < n; ++i) {
        int j = i;
        while (j > 0 && less(v[i], v[idx[j - 1]])) {
            idx[j] = idx[j - 1];
            --j;
        }
        idx[j] = i;
    }
}

// One codeword's decoder state. Depth d holds nodes of size n >> d.
struct Work {
    int L, m;
    const std::uint8_t* frozen;
    std::vector<std::vector<float>> alpha;           // [d]: (L, n >> d), node input
    std::vector<std::vector<std::uint8_t>> bl, br;   // [d]: (L, n >> (d + 1)), children's beta
    std::vector<std::vector<int>> p1, p2;            // [d]: (L,), children's path permutations
    std::vector<float> pm, cand;
    std::vector<int> idx;

    Work(int n, int L_, const std::uint8_t* fr) : L(L_), m(std::countr_zero(static_cast<unsigned>(n))), frozen(fr) {
        alpha.resize(m + 1);
        bl.resize(m);
        br.resize(m);
        p1.assign(m, std::vector<int>(L));
        p2.assign(m, std::vector<int>(L));
        for (int d = 0; d <= m; ++d) alpha[d].resize(static_cast<std::size_t>(L) * (n >> d));
        for (int d = 0; d < m; ++d) {
            bl[d].resize(static_cast<std::size_t>(L) * (n >> (d + 1)));
            br[d].resize(bl[d].size());
        }
        pm.resize(L);
        cand.resize(2 * L);
        idx.resize(2 * L);
    }

    // SCLDecoder._node: beta (L, n >> d) into out, path permutation into perm.
    void node(int d, int lo, std::uint8_t* out, int* perm) {
        if (d == m) {
            const float* a = alpha[d].data();
            if (frozen[lo]) {
                for (int l = 0; l < L; ++l) {
                    out[l] = 0;
                    perm[l] = l;
                    if (pm[l] != INFINITY || a[l] != a[l]) pm[l] += softplus(-a[l]);  // inf + finite stays inf
                }
                return;
            }
            for (int l = 0; l < L; ++l) {
                cand[l] = pm[l] + softplus(-a[l]);
                cand[L + l] = pm[l] + softplus(a[l]);
            }
            argsort(cand.data(), 2 * L, idx.data());
            for (int l = 0; l < L; ++l) {
                out[l] = static_cast<std::uint8_t>(idx[l] / L);
                perm[l] = idx[l] % L;
                pm[l] = cand[idx[l]];
            }
            return;
        }
        const int nd = static_cast<int>(alpha[d].size()) / L, h = nd / 2;
        const float* A = alpha[d].data();
        float* F = alpha[d + 1].data();
        for (int l = 0; l < L; ++l) {
            const float* __restrict a = A + l * nd;
            float* __restrict f = F + l * h;
            for (int i = 0; i < h; ++i) f[i] = f_op(a[i], a[h + i]);
        }
        int* q1 = p1[d].data();
        int* q2 = p2[d].data();
        std::uint8_t* left = bl[d].data();
        std::uint8_t* right = br[d].data();
        node(d + 1, lo, left, q1);
        // g on the survivors: their a, b are the rows q1 picked.
        for (int l = 0; l < L; ++l) {
            const float* __restrict a = A + q1[l] * nd;
            const std::uint8_t* __restrict u = left + l * h;
            float* __restrict f = F + l * h;
            for (int i = 0; i < h; ++i) f[i] = a[h + i] + (1.0f - 2.0f * u[i]) * a[i];  // exact: (+-1) a
        }
        node(d + 1, lo + h, right, q2);
        for (int l = 0; l < L; ++l) {
            const std::uint8_t* __restrict u = left + q2[l] * h;
            const std::uint8_t* __restrict v = right + l * h;
            std::uint8_t* __restrict o = out + l * nd;
            for (int i = 0; i < h; ++i) o[i] = u[i] ^ v[i];
            std::copy(v, v + h, o + h);
            perm[l] = q1[q2[l]];
        }
    }
};

}  // namespace

void transform(std::span<std::uint8_t> x) {
    const std::size_t n = x.size();
    for (std::size_t h = 1; h < n; h *= 2)
        for (std::size_t j = 0; j < n; j += 2 * h)
            for (std::size_t i = j; i < j + h; ++i) x[i] ^= x[i + h];
}

PolarCode::PolarCode(int k_, int e_, std::span<const std::uint16_t> info) : k(k_), e(e_), info_pos(info.begin(), info.end()) {
    if (e < 1 || k < 0 || k > e) throw std::invalid_argument("polar: need 0 <= k <= e, e >= 1");
    n = static_cast<int>(std::bit_ceil(static_cast<unsigned>(e)));
    std::ranges::sort(info_pos);
    if (static_cast<int>(info_pos.size()) != k || (k && info_pos.back() >= n) ||
        std::ranges::adjacent_find(info_pos) != info_pos.end())
        throw std::invalid_argument("polar: info_pos must be k distinct positions below n");
    const int bits = std::countr_zero(static_cast<unsigned>(n));
    for (int i = 0; i < n - e; ++i) {
        unsigned r = 0;
        for (int b = 0; b < bits; ++b) r |= ((static_cast<unsigned>(i) >> b) & 1u) << (bits - 1 - b);
        punctured.push_back(static_cast<std::uint16_t>(r));
    }
    std::ranges::sort(punctured);
    std::vector<std::uint8_t> is_sent(n, 1);
    for (auto p : punctured) is_sent[p] = 0;
    for (int i = 0; i < n; ++i)
        if (is_sent[i]) sent.push_back(static_cast<std::uint16_t>(i));
    frozen.assign(n, 1);
    for (auto p : info_pos) frozen[p] = 0;
}

Mat<std::uint8_t> PolarCode::encode(const Mat<std::uint8_t>& bits) const {
    if (static_cast<int>(bits.cols) != k) throw std::invalid_argument("polar encode: expected (B, k) bits");
    Mat<std::uint8_t> out(bits.rows, e);
    std::vector<std::uint8_t> u(n);
    for (std::size_t b = 0; b < bits.rows; ++b) {
        std::ranges::fill(u, 0);
        for (int j = 0; j < k; ++j) u[info_pos[j]] = bits[b][j];
        transform(u);
        for (int j = 0; j < e; ++j) out[b][j] = u[sent[j]];
    }
    return out;
}

std::span<const std::uint16_t> ga_info_pos(int k, int e) {
    for (const auto& d : tables::POLAR_GA)
        if (d.k == k && d.e == e) return d.info_pos;
    throw std::out_of_range("polar: no frozen GA design for k=" + std::to_string(k) + ", e=" + std::to_string(e));
}

SCLDecoder::SCLDecoder(PolarCode code, int list_size) : code_(std::move(code)), L_(list_size) {
    if (L_ < 1) throw std::invalid_argument("polar: list size must be >= 1");
}

SclResult SCLDecoder::decode(const Mat<float>& llr) const {
    const auto& c = code_;
    if (static_cast<int>(llr.cols) != c.e) throw std::invalid_argument("polar decode: expected (B, e) LLRs");
    const std::size_t B = llr.rows;
    const int L = L_, n = c.n, k = c.k;
    SclResult res{Mat<std::uint8_t>(B, static_cast<std::size_t>(L) * k), Mat<float>(B, L)};
    // rows are independent: one pool task each, with its own workspace
    pool::parallel_for(B, [&](std::size_t b) {
        Work w(n, L, c.frozen.data());
        std::vector<std::uint8_t> x(static_cast<std::size_t>(L) * n), u(n);
        std::vector<int> perm(L), order(L);
        float* a0 = w.alpha[0].data();
        std::fill(a0, a0 + n, 0.0f);
        for (int j = 0; j < c.e; ++j) a0[c.sent[j]] = llr[b][j];
        for (int l = 1; l < L; ++l) std::copy(a0, a0 + n, a0 + static_cast<std::size_t>(l) * n);
        std::ranges::fill(w.pm, INFINITY);
        w.pm[0] = 0.0f;
        w.node(0, 0, x.data(), perm.data());
        argsort(w.pm.data(), L, order.data());
        for (int l = 0; l < L; ++l) {
            const std::uint8_t* src = x.data() + static_cast<std::size_t>(order[l]) * n;
            std::copy(src, src + n, u.begin());
            transform(u);
            std::uint8_t* dst = res.paths[b] + static_cast<std::size_t>(l) * k;
            for (int j = 0; j < k; ++j) dst[j] = u[c.info_pos[j]];
            res.metric[b][l] = w.pm[order[l]];
        }
    });
    return res;
}

}  // namespace data2g::polar
