// Small dense linear algebra in plain loops (the plan rules out Eigen and
// LAPACK): what numpy's linalg.solve and an SVD's subspace become.
#pragma once

#include <cmath>
#include <complex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace data2g::linalg {

using cd = std::complex<double>;

// LAPACK's pivot measure: |x| for reals, |re| + |im| (cabs1) for complex.
inline double pivot_abs(double x) { return std::abs(x); }
inline double pivot_abs(const cd& x) { return std::abs(x.real()) + std::abs(x.imag()); }

// Solve A X = B in place, as np.linalg.solve (LU, partial pivoting): `a` is
// n x n row-major and destroyed; `b` is n x nrhs row-major and becomes X.
// Throws on an exactly singular pivot, where LAPACK's gesv does.
template <typename T>
void lu_solve(T* a, int n, T* b, int nrhs) {
    for (int k = 0; k < n; ++k) {
        int p = k;
        double best = pivot_abs(a[k * n + k]);
        for (int i = k + 1; i < n; ++i)
            if (const double v = pivot_abs(a[i * n + k]); v > best) {
                best = v;
                p = i;
            }
        if (best == 0.0) throw std::runtime_error("lu_solve: singular matrix");
        if (p != k) {
            for (int j = 0; j < n; ++j) std::swap(a[k * n + j], a[p * n + j]);
            for (int j = 0; j < nrhs; ++j) std::swap(b[k * nrhs + j], b[p * nrhs + j]);
        }
        const T inv = T(1) / a[k * n + k];
        for (int i = k + 1; i < n; ++i) {
            const T l = a[i * n + k] * inv;
            if (l == T(0)) continue;
            for (int j = k + 1; j < n; ++j) a[i * n + j] -= l * a[k * n + j];
            for (int j = 0; j < nrhs; ++j) b[i * nrhs + j] -= l * b[k * nrhs + j];
        }
    }
    for (int k = n - 1; k >= 0; --k) {
        const T inv = T(1) / a[k * n + k];
        for (int j = 0; j < nrhs; ++j) {
            T s = b[k * nrhs + j];
            for (int i = k + 1; i < n; ++i) s -= a[k * n + i] * b[i * nrhs + j];
            b[k * nrhs + j] = s * inv;
        }
    }
}

// In place lower Cholesky factor of a Hermitian n x n `a` (row-major; the
// upper triangle is left as it was). False if `a` is not numerically
// positive definite.
bool cholesky(cd* a, int n);

// Solve L Y = B in place for lower-triangular L (cholesky's), B n x nrhs.
void forward_solve(const cd* l, int n, cd* b, int nrhs);

// The same for a real symmetric `a`.
bool cholesky(double* a, int n);
void forward_solve(const double* l, int n, double* b, int nrhs);

// Eigen-decomposition of a Hermitian n x n matrix by cyclic Jacobi: `a`
// (row-major) is destroyed, column j of `v` (n x n) is the eigenvector of
// eigenvalue j. Unsorted. Accurate to a few ulps of the largest eigenvalue.
std::vector<double> hermitian_eigen(std::vector<cd>& a, std::vector<cd>& v, int n);

}  // namespace data2g::linalg
