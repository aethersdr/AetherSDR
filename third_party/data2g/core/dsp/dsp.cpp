#include "dsp/dsp.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

#include "dsp/fft.hpp"

namespace data2g::dsp {
namespace {

constexpr double PI = std::numbers::pi;

// numpy's pairwise summation (loops_utils.h.src) over n contiguous
// doubles. width 2 is its complex form: interleaved (re, im), n counting
// doubles, which pairs its 8 accumulators differently.
void pairwise(const double* a, std::size_t n, int width, double* out) {
    if (n < 8) {
        for (int w = 0; w < width; ++w) out[w] = -0.0;
        for (std::size_t i = 0; i < n; i += static_cast<std::size_t>(width))
            for (int w = 0; w < width; ++w) out[w] += a[i + static_cast<std::size_t>(w)];
        return;
    }
    if (n <= 128) {
        double r[8];
        for (std::size_t j = 0; j < 8; ++j) r[j] = a[j];
        std::size_t i = 8;
        for (; i < n - n % 8; i += 8)
            for (std::size_t j = 0; j < 8; ++j) r[j] += a[i + j];
        if (width == 1) {
            out[0] = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
        } else {
            out[0] = (r[0] + r[2]) + (r[4] + r[6]);
            out[1] = (r[1] + r[3]) + (r[5] + r[7]);
        }
        for (; i < n; i += static_cast<std::size_t>(width))
            for (int w = 0; w < width; ++w) out[w] += a[i + static_cast<std::size_t>(w)];
        return;
    }
    std::size_t n2 = n / 2;
    n2 -= n2 % 8;
    double lo[2], hi[2];
    pairwise(a, n2, width, lo);
    pairwise(a + n2, n - n2, width, hi);
    for (int w = 0; w < width; ++w) out[w] = lo[w] + hi[w];
}

// numpy.sinc: sin(pi x)/(pi x), with x == 0 replaced by 1e-20.
double sinc(double x) {
    const double y = PI * (x == 0.0 ? 1.0e-20 : x);
    return std::sin(y) / y;
}

}  // namespace

double pairwise_sum(std::span<const double> a) {
    double s;
    pairwise(a.data(), a.size(), 1, &s);
    return 0.0 + s;  // the reduction's initial value
}

cdouble pairwise_sum(std::span<const cdouble> a) {
    double s[2];
    pairwise(reinterpret_cast<const double*>(a.data()), 2 * a.size(), 2, s);
    return {0.0 + s[0], 0.0 + s[1]};
}

double quantile(std::vector<double> v, double q) {
    if (v.empty()) throw std::invalid_argument("quantile of an empty array");
    const std::size_t n = v.size();
    const double vi = static_cast<double>(n - 1) * q;
    std::size_t prev = static_cast<std::size_t>(std::floor(vi)), next = prev + 1;
    if (vi >= static_cast<double>(n - 1)) prev = next = n - 1;
    const double gamma = vi - std::floor(vi);
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(prev), v.end());
    const double a = v[prev];
    const double b = next == prev ? a : *std::min_element(v.begin() + static_cast<std::ptrdiff_t>(prev) + 1, v.end());
    const double diff = b - a;
    return gamma >= 0.5 ? b - diff * (1 - gamma) : a + diff * gamma;
}

namespace {

// scipy.signal.firwin for one band [left, right] (fractions of Nyquist),
// Hamming window, scale=True at `scale_at` (0: DC, else the band centre).
std::vector<double> firwin_band(int numtaps, double left, double right, double scale_at) {
    const double alpha = 0.5 * (numtaps - 1);
    // Hamming, sym=True: 0.54 + (1 - 0.54) cos(linspace(-pi, pi, numtaps))
    const double step = (PI - -PI) / static_cast<double>(numtaps - 1);
    std::vector<double> h(static_cast<std::size_t>(numtaps)), m(h.size());
    for (int i = 0; i < numtaps; ++i) {
        const auto u = static_cast<std::size_t>(i);
        m[u] = static_cast<double>(i) - alpha;
        const double fac = i == numtaps - 1 ? PI : static_cast<double>(i) * step + -PI;
        const double win = 0.54 + (1.0 - 0.54) * std::cos(fac);  // general_hamming: 1 - alpha, not 0.46
        h[u] = (right * sinc(right * m[u]) - left * sinc(left * m[u])) * win;
    }
    std::vector<double> resp(h.size());
    for (std::size_t i = 0; i < h.size(); ++i) resp[i] = h[i] * std::cos(PI * m[i] * scale_at);
    const double s = pairwise_sum(resp);
    for (double& v : h) v /= s;
    return h;
}

}  // namespace

std::vector<double> firwin_bandpass(int numtaps, double lo_hz, double hi_hz, double fs) {
    const double nyq = 0.5 * fs, left = lo_hz / nyq, right = hi_hz / nyq;
    return firwin_band(numtaps, left, right, 0.5 * (left + right));  // unit gain at the passband centre
}

std::vector<double> firwin_lowpass(int numtaps, double cutoff_hz, double fs) {
    return firwin_band(numtaps, 0.0, cutoff_hz / (0.5 * fs), 0.0);  // unit gain at DC
}

std::vector<cdouble> hilbert(std::span<const double> x) {
    const std::size_t n = x.size();
    if (n == 0) return {};
    std::vector<cdouble> spectrum = fft(std::vector<cdouble>(x.begin(), x.end()), true);
    const std::size_t pos_end = n % 2 == 0 ? n / 2 : (n + 1) / 2, neg = n % 2 == 0 ? n / 2 + 1 : (n + 1) / 2;
    for (std::size_t i = 1; i < pos_end; ++i) spectrum[i] *= 2.0;
    for (std::size_t i = neg; i < n; ++i) spectrum[i] = 0.0;
    return fft(spectrum, false);
}

std::vector<double> convolve_same(std::span<const double> a, std::span<const double> v) {
    const std::ptrdiff_t n = static_cast<std::ptrdiff_t>(a.size()), m = static_cast<std::ptrdiff_t>(v.size());
    if (n < m) throw std::invalid_argument("convolve_same: len(a) < len(v)");
    // out[i] = sum_t v[t] a[i + offset - t]. Tap-major, so the loop over
    // outputs vectorizes without reassociating any one sum (numpy's order
    // is BLAS ddot's, unknowable: tolerance-class either way)
    const std::ptrdiff_t offset = (m - 1) / 2;
    std::vector<double> out(a.size(), 0.0);
    for (std::ptrdiff_t t = 0; t < m; ++t) {
        const std::ptrdiff_t shift = offset - t;  // out[i] reads a[i + shift]
        const std::ptrdiff_t lo = std::max<std::ptrdiff_t>(0, -shift), hi = std::min(n, n - shift);
        const double vt = v[static_cast<std::size_t>(t)];
        const double* src = a.data() + shift;
        for (std::ptrdiff_t i = lo; i < hi; ++i) out[static_cast<std::size_t>(i)] += vt * src[i];
    }
    return out;
}

std::size_t next_fast_len(std::size_t n) { return pocketfft::detail::util::good_size_cmplx(n); }

std::vector<cdouble> fftconvolve_valid(std::span<const cdouble> a, std::span<const cdouble> v) {
    if (a.size() < v.size() || v.empty()) throw std::invalid_argument("fftconvolve_valid: need len(a) >= len(v) > 0");
    const std::size_t n = next_fast_len(a.size() + v.size() - 1);
    std::vector<cdouble> pa(n), pv(n);
    std::copy(a.begin(), a.end(), pa.begin());
    std::copy(v.begin(), v.end(), pv.begin());
    auto [fa, fv] = fft_pair(pa, pv, true);
    for (std::size_t i = 0; i < n; ++i) fa[i] *= fv[i];
    const std::vector<cdouble> conv = fft(fa, false);
    const auto first = conv.begin() + static_cast<std::ptrdiff_t>(v.size() - 1);
    return {first, first + static_cast<std::ptrdiff_t>(a.size() - v.size() + 1)};
}

}  // namespace data2g::dsp
