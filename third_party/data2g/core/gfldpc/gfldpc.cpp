#include "gfldpc/gfldpc.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace data2g::gfldpc {

namespace {

constexpr unsigned POLY[] = {0, 0, 0, 0b1011, 0b10011, 0b100101};  // gfldpc.POLY by p

// Unnormalized Walsh-Hadamard transform in place (Sylvester order: scipy.linalg.hadamard's).
void wht(float* a, int q) {
    for (int h = 1; h < q; h *= 2)
        for (int i = 0; i < q; i += 2 * h)
            for (int j = i; j < i + h; ++j) {
                const float x = a[j], y = a[j + h];
                a[j] = x + y;
                a[j + h] = x - y;
            }
}

}  // namespace

GF::GF(int p_) : p(p_), q(1 << p_), mul(static_cast<std::size_t>(q * q), 0), inv(static_cast<std::size_t>(q), 0) {
    if (p < 3 || p > 5) throw std::invalid_argument("GF(2^p): p must be 3, 4 or 5");
    std::vector<int> exp(static_cast<std::size_t>(q - 1)), log(static_cast<std::size_t>(q), 0);
    unsigned x = 1;
    for (int i = 0; i < q - 1; ++i) {
        exp[i] = static_cast<int>(x);
        log[x] = i;
        x <<= 1;
        if (x & static_cast<unsigned>(q)) x ^= POLY[p];
    }
    for (int a = 1; a < q; ++a) {
        for (int b = 1; b < q; ++b) mul[a * q + b] = static_cast<std::uint8_t>(exp[(log[a] + log[b]) % (q - 1)]);
        inv[a] = static_cast<std::uint8_t>(exp[(q - 1 - log[a]) % (q - 1)]);
    }
}

Code::Code(int p_, int n_, int n0_, int dmax_, std::span<const std::int16_t> var_, std::span<const std::uint8_t> h_)
    : p(p_), q(1 << p_), n(n_), n0(n0_ ? n0_ : n_), m(static_cast<int>(var_.size()) / dmax_), dmax(dmax_),
      var(var_.begin(), var_.end()), h(h_.begin(), h_.end()), gf_(p_) {
    if (var_.size() != h_.size() || var_.size() % static_cast<std::size_t>(dmax_))
        throw std::invalid_argument("gfldpc::Code: var and h must be m * dmax");
    build();
}

Code::Code(const Code& c, int n_) : p(c.p), q(c.q), n(n_), n0(std::min(c.n0, n_)), m(0), dmax(0), gf_(c.p) {
    // the rows within n_, as gfldpc.cut: their slots up to the widest kept row's
    std::vector<int> keep;
    for (int r = 0; r < c.m; ++r) {
        int top = -1, w = 0;
        for (int d = 0; d < c.dmax; ++d) {
            const int v = c.var[r * c.dmax + d];
            if (v >= 0) top = std::max(top, v), ++w;
        }
        if (top < n_) {
            keep.push_back(r);
            dmax = std::max(dmax, w);
        }
    }
    m = static_cast<int>(keep.size());
    for (int r : keep)
        for (int d = 0; d < dmax; ++d) {
            var.push_back(c.var[r * c.dmax + d]);
            h.push_back(c.h[r * c.dmax + d]);
        }
    build();
}

Code Code::cut(int n_) const { return Code(*this, n_); }

void Code::build() {
    // core rows (all symbols < n0) and extension rows by their own symbol
    std::vector<int> core;
    ext_rows_.assign(static_cast<std::size_t>(n - n0), -1);
    for (int r = 0; r < m; ++r) {
        int top = -1;
        for (int d = 0; d < dmax; ++d) top = std::max(top, var[r * dmax + d]);
        if (top < n0) core.push_back(r);
        else if (ext_rows_[top - n0] < 0) ext_rows_[top - n0] = r;
        else throw std::invalid_argument("gfldpc::Code: two extension rows own one symbol");
    }
    for (int r : ext_rows_)
        if (r < 0) throw std::invalid_argument("gfldpc::Code: an extension symbol without its row");
    // the core's reduced row-echelon form, pivots from the last column (gfldpc.GFCode._echelon)
    const int mc = static_cast<int>(core.size());
    std::vector<std::uint8_t> H(static_cast<std::size_t>(mc * n0), 0);
    for (int i = 0; i < mc; ++i)
        for (int d = 0; d < dmax; ++d) {
            const int v = var[core[i] * dmax + d];
            if (v >= 0) H[i * n0 + v] ^= h[core[i] * dmax + d];
        }
    const auto* mul = gf_.mul.data();
    int r = 0;
    std::vector<std::uint8_t> is_piv(static_cast<std::size_t>(n0), 0);
    for (int col = n0 - 1; col >= 0 && r < mc; --col) {
        int pr = -1;
        for (int i = r; i < mc; ++i)
            if (H[i * n0 + col]) {
                pr = i;
                break;
            }
        if (pr < 0) continue;
        if (pr != r) std::swap_ranges(H.begin() + pr * n0, H.begin() + (pr + 1) * n0, H.begin() + r * n0);
        const int iv = gf_.inv[H[r * n0 + col]];
        for (int j = 0; j < n0; ++j) H[r * n0 + j] = mul[iv * q + H[r * n0 + j]];
        for (int i = 0; i < mc; ++i) {
            const int f = H[i * n0 + col];
            if (i == r || !f) continue;
            for (int j = 0; j < n0; ++j) H[i * n0 + j] ^= mul[f * q + H[r * n0 + j]];
        }
        parity_.push_back(col);
        is_piv[col] = 1;
        ++r;
    }
    for (int c = 0; c < n0; ++c)
        if (!is_piv[c]) info_.push_back(c);
    const std::size_t k = info_.size();
    a_.resize(static_cast<std::size_t>(r) * k);
    for (int i = 0; i < r; ++i)
        for (std::size_t j = 0; j < k; ++j) a_[i * k + j] = H[i * n0 + info_[j]];
}

Mat<std::uint8_t> Code::encode(const Mat<std::uint8_t>& u) const {
    const std::size_t k = info_.size();
    if (u.cols != k) throw std::invalid_argument("gfldpc::Code::encode: expected (B, k)");
    const auto* mul = gf_.mul.data();
    Mat<std::uint8_t> x(u.rows, static_cast<std::size_t>(n), 0);
    for (std::size_t b = 0; b < u.rows; ++b) {
        std::uint8_t* xb = x[b];
        for (std::size_t j = 0; j < k; ++j) xb[info_[j]] = u[b][j];
        for (std::size_t i = 0; i < parity_.size(); ++i) {
            unsigned acc = 0;
            for (std::size_t j = 0; j < k; ++j) acc ^= mul[a_[i * k + j] * q + u[b][j]];
            xb[parity_[i]] = static_cast<std::uint8_t>(acc);
        }
        for (int e = 0; e < n - n0; ++e) {  // each extension symbol from its row, in order
            const int row = ext_rows_[e], own = n0 + e;
            unsigned acc = 0, hs = 0;
            for (int d = 0; d < dmax; ++d) {
                const int v = var[row * dmax + d];
                if (v < 0) continue;
                if (v == own) hs = h[row * dmax + d];
                else acc ^= mul[h[row * dmax + d] * q + xb[v]];
            }
            xb[own] = mul[gf_.inv[hs] * q + acc];
        }
    }
    return x;
}

bool Code::syndrome_ok(const std::uint8_t* x) const {
    const auto* mul = gf_.mul.data();
    for (int r = 0; r < m; ++r) {
        unsigned acc = 0;
        for (int d = 0; d < dmax; ++d) {
            const int v = var[r * dmax + d];
            if (v >= 0) acc ^= mul[h[r * dmax + d] * q + x[v]];
        }
        if (acc) return false;
    }
    return true;
}

Decoder::Decoder(const Code& code) : code_(code) {
    std::vector<std::vector<int>> cols(static_cast<std::size_t>(code.n));
    for (int e = 0; e < code.m * code.dmax; ++e)
        if (code.var[e] >= 0) cols[code.var[e]].push_back(e);
    col_ptr_.push_back(0);
    for (const auto& c : cols) {
        col_edges_.insert(col_edges_.end(), c.begin(), c.end());
        col_ptr_.push_back(static_cast<int>(col_edges_.size()));
    }
}

namespace {

// v *= w, then scaled to a maximum of 1 (no underflow over a product), floored at FLOOR.
constexpr float FLOOR = 1e-20f;
void mul_norm(float* v, const float* w, int q) {
    float mx = 0.0f;
    for (int a = 0; a < q; ++a) mx = std::max(mx, v[a] *= w[a]);
    const float k = mx > 0.0f ? 1.0f / mx : 1.0f;
    for (int a = 0; a < q; ++a) v[a] = std::max(v[a] * k, FLOOR);
}

}  // namespace

// The same belief propagation as gfldpc.Decoder (flooding, the check in the
// Walsh-Hadamard domain), on probabilities in place of log-probabilities: no
// exp or log in the loop, decisions alike.
Decoded Decoder::decode(const Mat<float>& ll, int iters) const {
    const Code& c = code_;
    const int q = c.q, n = c.n, D = c.dmax;
    if (ll.cols != static_cast<std::size_t>(n * q)) throw std::invalid_argument("gfldpc::Decoder: expected (B, n * q)");
    const auto* mul = c.gf().mul.data();
    const auto* inv = c.gf().inv.data();
    Decoded out{Mat<std::uint8_t>(ll.rows, static_cast<std::size_t>(n)), std::vector<std::uint8_t>(ll.rows, 0)};
    const std::size_t E = static_cast<std::size_t>(c.m * D);
    int dv = 0;
    for (int v = 0; v < n; ++v) dv = std::max(dv, col_ptr_[v + 1] - col_ptr_[v]);
    std::vector<float> ch(static_cast<std::size_t>(n * q)), c2v(E * q), v2c(E * q), W(static_cast<std::size_t>(D * q)),
        fwd(static_cast<std::size_t>((std::max(D, dv) + 1) * q)), tmp(static_cast<std::size_t>(q));
    for (std::size_t b = 0; b < ll.rows; ++b) {
        std::uint8_t* hard = out.hard[b];
        for (int v = 0; v < n; ++v) {  // exp(ll - its max)
            const float* l = ll[b] + v * q;
            const float mx = *std::max_element(l, l + q);
            for (int a = 0; a < q; ++a) ch[v * q + a] = std::max(std::exp(l[a] - mx), FLOOR);
        }
        std::fill(c2v.begin(), c2v.end(), 1.0f);
        bool ok = false;
        for (int it = 0;; ++it) {
            // symbols: the posterior's decision, and each edge's message (the others' product, sum 1)
            for (int v = 0; v < n; ++v) {
                const int* es = &col_edges_[col_ptr_[v]];
                const int d = col_ptr_[v + 1] - col_ptr_[v];
                std::copy_n(&ch[v * q], q, &fwd[0]);  // fwd[j]: ch times the first j edges'
                for (int j = 0; j < d; ++j) {
                    std::copy_n(&fwd[j * q], q, &fwd[(j + 1) * q]);
                    mul_norm(&fwd[(j + 1) * q], &c2v[es[j] * q], q);
                }
                const float* post = &fwd[d * q];
                hard[v] = static_cast<std::uint8_t>(std::max_element(post, post + q) - post);
                std::fill(tmp.begin(), tmp.end(), 1.0f);  // the product of the edges past j
                for (int j = d - 1; j >= 0; --j) {
                    float* m = &v2c[es[j] * q];
                    float s = 0.0f;
                    for (int a = 0; a < q; ++a) s += m[a] = fwd[j * q + a] * tmp[a];
                    const float k = 1.0f / s;
                    for (int a = 0; a < q; ++a) m[a] *= k;
                    if (j) mul_norm(tmp.data(), &c2v[es[j] * q], q);
                }
            }
            ok = c.syndrome_ok(hard);
            if (ok || it == iters) break;
            // checks
            for (int r = 0; r < c.m; ++r) {
                for (int d = 0; d < D; ++d) {
                    const int e = r * D + d;
                    float* w = &W[d * q];
                    if (c.var[e] < 0) {  // padding: a known zero, whose transform is all ones
                        std::fill(w, w + q, 1.0f);
                        continue;
                    }
                    const int hi = inv[c.h[e]];
                    const float* m = &v2c[e * q];
                    for (int y = 0; y < q; ++y) w[y] = m[mul[hi * q + y]];  // P_y[y] = P_x[h^-1 y]
                    wht(w, q);
                }
                // products of the others' transforms: forward, then backward
                std::copy(W.begin(), W.begin() + q, fwd.begin());
                for (int d = 1; d < D; ++d)
                    for (int a = 0; a < q; ++a) fwd[d * q + a] = fwd[(d - 1) * q + a] * W[d * q + a];
                std::fill(tmp.begin(), tmp.end(), 1.0f);  // the backward product past d
                for (int d = D - 1; d >= 0; --d) {
                    const int e = r * D + d;
                    if (c.var[e] >= 0) {
                        float R[32];
                        for (int a = 0; a < q; ++a) R[a] = (d ? fwd[(d - 1) * q + a] : 1.0f) * tmp[a];
                        wht(R, q);
                        const int hv = c.h[e];
                        float* m = &c2v[e * q];
                        float mx = 0.0f;
                        for (int a = 0; a < q; ++a) mx = std::max(mx, m[a] = std::max(R[mul[hv * q + a]], 0.0f));
                        const float k = mx > 0.0f ? 1.0f / mx : 1.0f;
                        for (int a = 0; a < q; ++a) m[a] = std::max(m[a] * k, FLOOR);
                    }
                    for (int a = 0; a < q; ++a) tmp[a] *= W[d * q + a];
                }
            }
        }
        out.ok[b] = ok;
    }
    return out;
}

Mat<std::uint8_t> pack(const Mat<std::uint8_t>& bits, int p, int k) {
    Mat<std::uint8_t> out(bits.rows, static_cast<std::size_t>(k), 0);
    for (std::size_t b = 0; b < bits.rows; ++b)
        for (std::size_t i = 0; i < bits.cols && i < static_cast<std::size_t>(k * p); ++i)
            out[b][i / p] |= static_cast<std::uint8_t>((bits[b][i] & 1) << (p - 1 - static_cast<int>(i % p)));
    return out;
}

Mat<std::uint8_t> unpack(const Mat<std::uint8_t>& x, int p) {
    Mat<std::uint8_t> out(x.rows, x.cols * p);
    for (std::size_t b = 0; b < x.rows; ++b)
        for (std::size_t i = 0; i < x.cols * p; ++i) out[b][i] = (x[b][i / p] >> (p - 1 - static_cast<int>(i % p))) & 1;
    return out;
}

}  // namespace data2g::gfldpc
