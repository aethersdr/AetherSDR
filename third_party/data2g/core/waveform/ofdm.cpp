#include "waveform/ofdm.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>

#include "util/simd.hpp"

namespace data2g::waveform {
namespace {

using config::FCENTER;
using config::FS;
using config::M;
using config::NCP;
using config::NSYM;
using config::PREAMBLE_CP;

constexpr double TWO_PI = 2.0 * std::numbers::pi;

// re/im[k] += sum over n of demod[k][n] * win[n], n ascending, every
// carrier at once. A third of a live receive: SIMD clones.
DATA2G_SIMD_CLONES void demod_sums(const cdouble* __restrict win, const double* __restrict dre,
                                   const double* __restrict dim, std::size_t nc, double* __restrict re,
                                   double* __restrict im) {
    for (std::size_t n = 0; n < static_cast<std::size_t>(M); ++n) {
        const double br = win[n].real(), bi = win[n].imag();
        const double* ar = dre + n * nc;
        const double* ai = dim + n * nc;
        for (std::size_t k = 0; k < nc; ++k) {
            re[k] += ar[k] * br - ai[k] * bi;
            im[k] += ar[k] * bi + ai[k] * br;
        }
    }
}

// Python's slice bounds for z[a:b] on a sequence of length n.
std::int64_t slice_bound(std::int64_t i, std::int64_t n) {
    if (i < 0) i += n;
    return std::clamp<std::int64_t>(i, 0, n);
}

Band make_band(const config::Band& s) {
    Band b;
    b.spec = &s;
    for (int k = 0; k < s.nc; ++k) {
        b.freqs.push_back(config::CARRIER0 + config::RS * (s.k0 + k));
        b.bb.push_back(b.freqs.back() - FCENTER);
    }
    const auto nc = static_cast<std::size_t>(s.nc);
    b.mod = Mat<cdouble>(NSYM, nc);
    for (int n = 0; n < NSYM; ++n)
        for (std::size_t k = 0; k < nc; ++k) b.mod[static_cast<std::size_t>(n)][k] = phasor((n - NCP) * b.freqs[k]);
    b.demod = Mat<cdouble>(nc, M);
    for (std::size_t k = 0; k < nc; ++k)
        for (int n = 0; n < M; ++n) b.demod[k][static_cast<std::size_t>(n)] = phasor(n * b.bb[k], -1);
    b.demod_re = b.demod_im = Mat<double>(M, nc);
    for (std::size_t k = 0; k < nc; ++k)
        for (std::size_t n = 0; n < static_cast<std::size_t>(M); ++n) {
            b.demod_re[n][k] = b.demod[k][n].real();
            b.demod_im[n][k] = b.demod[k][n].imag();
        }
    // the pilot as an exact rational turn, as ofdm.band builds it
    for (int num : s.pilot_num) {
        const int q = ((num % config::PILOT_PHASE_DEN) + config::PILOT_PHASE_DEN) % config::PILOT_PHASE_DEN;
        b.pilot.push_back(std::polar(1.0, TWO_PI * q / config::PILOT_PHASE_DEN));
    }
    b.preamble_template.resize(static_cast<std::size_t>(b.preamble_samples()));
    for (std::size_t i = 0; i < b.preamble_template.size(); ++i) {
        const std::int64_t n = static_cast<std::int64_t>(i) - PREAMBLE_CP;
        cdouble acc{};
        for (std::size_t k = 0; k < nc; ++k) acc += phasor(n * b.bb[k]) * b.pilot[k];
        b.preamble_template[i] = 0.5 * acc;
    }
    return b;
}

}  // namespace

cdouble phasor(std::int64_t cycles_num, int sign) {
    std::int64_t q = cycles_num % FS;
    if (q < 0) q += FS;
    // numpy's order: (sign * 2pi * q) times (1 / FS), its complex division
    // by a real; the same double as Python's, so the same sin/cos.
    return std::polar(1.0, (sign * TWO_PI * static_cast<double>(q)) * (1.0 / FS));
}

double Band::preamble_threshold() const {
    if (spec->preamble_repeats != config::PREAMBLE_REPEATS)
        throw std::logic_error("no threshold generated for this preamble length");
    return config::PREAMBLE_THRESHOLD;
}

std::pair<double, double> Band::tx_bandpass() const {
    if (spec->name == "w") return {config::TX_BANDPASS[0], config::TX_BANDPASS[1]};
    return {static_cast<double>(freqs.front() - 75), static_cast<double>(freqs.back() + 75)};
}

std::vector<double> Band::modulate_symbols(const Mat<cdouble>& symbols) const {
    const auto nc = static_cast<std::size_t>(spec->nc);
    if (symbols.cols != nc) throw std::invalid_argument("modulate_symbols: need (n_sym, nc) symbols");
    std::vector<double> out(symbols.rows * NSYM);
    for (std::size_t s = 0; s < symbols.rows; ++s)
        for (std::size_t n = 0; n < static_cast<std::size_t>(NSYM); ++n) {
            cdouble acc{};
            for (std::size_t k = 0; k < nc; ++k) acc += mod[n][k] * symbols[s][k];
            out[s * NSYM + n] = acc.real();
        }
    return out;
}

std::vector<cdouble> Band::demod_window(std::span<const cdouble> z, std::int64_t start, std::int64_t backoff) const {
    const auto len = static_cast<std::int64_t>(z.size());
    const std::int64_t s = start - backoff, a = slice_bound(s, len), e = slice_bound(s + M, len);
    std::array<cdouble, M> win{};
    for (std::int64_t i = a; i < e; ++i) win[static_cast<std::size_t>(i - a)] = z[static_cast<std::size_t>(i)];
    // Each carrier's sum in order, as the complex loop `acc += demod[k][n] *
    // win[n]` would (the same doubles: no FMA in an ISO build), but over all
    // carriers at once in real arithmetic, so it vectorizes and no NaN
    // check intervenes (modem's receive spent a third of its time here).
    const std::size_t nc = static_cast<std::size_t>(spec->nc);
    std::vector<double> re(nc, 0.0), im(nc, 0.0);
    demod_sums(win.data(), demod_re.data.data(), demod_im.data.data(), nc, re.data(), im.data());
    std::vector<cdouble> out(nc);
    for (std::size_t k = 0; k < nc; ++k) out[k] = (2.0 / M) * cdouble(re[k], im[k]);
    return out;
}

std::vector<double> Band::preamble_waveform() const {
    std::vector<double> out(static_cast<std::size_t>(preamble_samples()));
    for (std::size_t i = 0; i < out.size(); ++i) {
        const std::int64_t n = static_cast<std::int64_t>(i) - PREAMBLE_CP;
        cdouble acc{};
        for (std::size_t k = 0; k < freqs.size(); ++k) acc += phasor(n * freqs[k]) * pilot[k];
        out[i] = acc.real();
    }
    return out;
}

const Band& band(std::string_view name) {
    static const auto bands = [] {
        std::array<Band, config::BANDS.size()> out;
        for (std::size_t i = 0; i < out.size(); ++i) out[i] = make_band(config::BANDS[i]);
        return out;
    }();
    for (const auto& b : bands)
        if (b.spec->name == name) return b;
    throw std::out_of_range("no band " + std::string(name));
}

}  // namespace data2g::waveform
