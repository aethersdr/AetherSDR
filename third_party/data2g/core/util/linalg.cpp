#include "util/linalg.hpp"

#include <cmath>
#include <cstddef>

namespace data2g::linalg {

// Complex arithmetic below is spelled out on the (re, im) pairs that
// std::complex guarantees: without -ffast-math, operator* checks every
// product for NaN, which keeps GCC from vectorizing the inner loops.

bool cholesky(cd* a, int n) {
    for (int j = 0; j < n; ++j) {
        const double* rj = reinterpret_cast<const double*>(a + j * n);
        double d = a[j * n + j].real();
        for (int m = 0; m < j; ++m) d -= rj[2 * m] * rj[2 * m] + rj[2 * m + 1] * rj[2 * m + 1];
        if (!(d > 0.0)) return false;
        const double ljj = std::sqrt(d), inv = 1.0 / ljj;
        a[j * n + j] = ljj;
        for (int i = j + 1; i < n; ++i) {
            double* ri = reinterpret_cast<double*>(a + i * n);
            double re = ri[2 * j], im = ri[2 * j + 1];
            for (int m = 0; m < j; ++m) {  // - L[i,m] * conj(L[j,m])
                re -= ri[2 * m] * rj[2 * m] + ri[2 * m + 1] * rj[2 * m + 1];
                im -= ri[2 * m + 1] * rj[2 * m] - ri[2 * m] * rj[2 * m + 1];
            }
            ri[2 * j] = re * inv;
            ri[2 * j + 1] = im * inv;
        }
    }
    return true;
}

void forward_solve(const cd* l, int n, cd* b, int nrhs) {
    for (int i = 0; i < n; ++i) {
        double* bi = reinterpret_cast<double*>(b + i * nrhs);
        for (int j = 0; j < i; ++j) {
            const double lr = l[i * n + j].real(), li = l[i * n + j].imag();
            const double* bj = reinterpret_cast<const double*>(b + j * nrhs);
            for (int c = 0; c < nrhs; ++c) {
                bi[2 * c] -= lr * bj[2 * c] - li * bj[2 * c + 1];
                bi[2 * c + 1] -= lr * bj[2 * c + 1] + li * bj[2 * c];
            }
        }
        const double inv = 1.0 / l[i * n + i].real();
        for (int c = 0; c < 2 * nrhs; ++c) bi[c] *= inv;
    }
}

bool cholesky(double* a, int n) {
    for (int j = 0; j < n; ++j) {
        const double* rj = a + j * n;
        double d = rj[j];
        for (int m = 0; m < j; ++m) d -= rj[m] * rj[m];
        if (!(d > 0.0)) return false;
        const double ljj = std::sqrt(d), inv = 1.0 / ljj;
        a[j * n + j] = ljj;
        for (int i = j + 1; i < n; ++i) {
            double* ri = a + i * n;
            double s = ri[j];
            for (int m = 0; m < j; ++m) s -= ri[m] * rj[m];
            ri[j] = s * inv;
        }
    }
    return true;
}

void forward_solve(const double* l, int n, double* b, int nrhs) {
    for (int i = 0; i < n; ++i) {
        double* bi = b + i * nrhs;
        for (int j = 0; j < i; ++j) {
            const double lij = l[i * n + j];
            const double* bj = b + j * nrhs;
            for (int c = 0; c < nrhs; ++c) bi[c] -= lij * bj[c];
        }
        const double inv = 1.0 / l[i * n + i];
        for (int c = 0; c < nrhs; ++c) bi[c] *= inv;
    }
}

// Cyclic Jacobi, after SSTVAE's real jacobi_eigen (native/core/modem/
// modem.cpp), made Hermitian: each rotation first turns a_pq real with a
// phase e on q, then is the real one. Only rows p and q are read (columns
// follow by symmetry), and V is kept transposed, so every loop is contiguous.
std::vector<double> hermitian_eigen(std::vector<cd>& a, std::vector<cd>& v, int n) {
    const auto N = static_cast<std::size_t>(n);
    auto row = [N](std::vector<cd>& m, int r) { return reinterpret_cast<double*>(m.data() + static_cast<std::size_t>(r) * N); };
    auto at = [N](std::vector<cd>& m, int r, int c) -> cd& {
        return m[static_cast<std::size_t>(r) * N + static_cast<std::size_t>(c)];
    };
    std::vector<cd> vt(N * N, cd(0.0));
    for (int i = 0; i < n; ++i) at(vt, i, i) = 1.0;
    double norm = 0.0;
    for (const cd& x : a) norm += std::norm(x);
    for (int sweep = 0; sweep < 100; ++sweep) {
        double off = 0.0;
        for (int p = 0; p < n; ++p)
            for (int q = p + 1; q < n; ++q) off += std::norm(at(a, p, q));
        if (off <= 1e-30 * norm) break;
        for (int p = 0; p < n; ++p) {
            for (int q = p + 1; q < n; ++q) {
                double* ap = row(a, p);
                double* aq = row(a, q);
                const double mag = std::hypot(ap[2 * q], ap[2 * q + 1]);
                if (mag == 0.0) continue;
                const double er = ap[2 * q] / mag, ei = ap[2 * q + 1] / mag;  // e = a_pq / |a_pq|
                const double theta = (aq[2 * q] - ap[2 * p]) / (2.0 * mag);
                const double t = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;
                const double app = ap[2 * p] - t * mag, aqq = aq[2 * q] + t * mag;
                // rows: a_pk' = c a_pk - s e a_qk, a_qk' = s a_pk + c e a_qk
                for (int k = 0; k < n; ++k) {
                    const double pr = ap[2 * k], pi = ap[2 * k + 1];
                    const double qr = er * aq[2 * k] - ei * aq[2 * k + 1], qi = er * aq[2 * k + 1] + ei * aq[2 * k];
                    ap[2 * k] = c * pr - s * qr;
                    ap[2 * k + 1] = c * pi - s * qi;
                    aq[2 * k] = s * pr + c * qr;
                    aq[2 * k + 1] = s * pi + c * qi;
                }
                for (int k = 0; k < n; ++k) {  // columns by symmetry: a_kp = conj(a_pk)
                    at(a, k, p) = cd(ap[2 * k], -ap[2 * k + 1]);
                    at(a, k, q) = cd(aq[2 * k], -aq[2 * k + 1]);
                }
                at(a, p, p) = app;
                at(a, q, q) = aqq;
                at(a, p, q) = at(a, q, p) = 0.0;
                // V's columns: v_kp' = c v_kp - s conj(e) v_kq, v_kq' = s v_kp + c conj(e) v_kq
                double* vp = row(vt, p);
                double* vq = row(vt, q);
                for (int k = 0; k < n; ++k) {
                    const double pr = vp[2 * k], pi = vp[2 * k + 1];
                    const double qr = er * vq[2 * k] + ei * vq[2 * k + 1], qi = er * vq[2 * k + 1] - ei * vq[2 * k];
                    vp[2 * k] = c * pr - s * qr;
                    vp[2 * k + 1] = c * pi - s * qi;
                    vq[2 * k] = s * pr + c * qr;
                    vq[2 * k + 1] = s * pi + c * qi;
                }
            }
        }
    }
    v.resize(N * N);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) at(v, i, j) = at(vt, j, i);
    std::vector<double> lam(N);
    for (int i = 0; i < n; ++i) lam[static_cast<std::size_t>(i)] = at(a, i, i).real();
    return lam;
}

}  // namespace data2g::linalg
