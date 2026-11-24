// data2g/equalizer.py: pilot-based channel estimation for one burst.
//
// Every function takes the band's baseband carrier frequencies `bb` (Hz,
// ofdm.band(name).bb; bb(band) below builds them). Pilot arrays are
// Mat<cd> (pilots or rows, carriers), as in Python.
#pragma once

#include <array>
#include <complex>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "generated/config.hpp"
#include "util/mat.hpp"

namespace data2g::equalizer {

using cd = std::complex<double>;
using Support = std::pair<int, int>;  // (first, last) delay, samples

inline constexpr double FRAME_S = static_cast<double>(config::FRAME_SAMPLES) / config::FS;
inline constexpr int DELAY_MIN = -2 * config::NCP, DELAY_MAX = 2 * config::NCP;  // _DELAYS
inline constexpr int TIME_TAPS = 4;
inline constexpr int DD_TAPS = 2;
inline constexpr double DEFAULT_SPREAD_HZ = 2.0;
inline constexpr bool LOO_SPREAD = true;  // equalizer.filter_spread's, see equalizer.py
inline constexpr int LOO_MIN_PILOTS = 8;
inline constexpr double LOO_TOL = 0.1;
inline constexpr std::array<double, 12> SPREAD_GRID = {0.02, 0.05, 0.1, 0.2, 0.35, 0.5, 0.7, 1.0, 1.4, 2.0, 2.8, 4.0};
inline constexpr bool PER_CARRIER_NOISE = true;
inline constexpr double NOISE_SHAPE_PRIOR = 8.0;
inline constexpr int PREAMBLE_DIFFS = 7;  // preamble_noise_k's samples: the default 8 repeats' differences
inline constexpr double SUPPORT_NOISE_Z = 3.09;  // delay_support's noise gate, sigmas; 0: off
inline constexpr int SUPPORT_SLACK = 4;  // _support_basis's
inline constexpr int PADDED_SLACK = 20;  // support_slack's padded order
inline constexpr double PADDED_GAIN = 2.0;
inline constexpr double INF = std::numeric_limits<double>::infinity();

// CARRIER0 + RS * (k0 + k) - FCENTER.
std::vector<double> bb(const config::Band& band);

double residual_cfo(const Mat<cd>& h_pilot);
std::vector<double> delay_profile(const Mat<cd>& h_pilot, std::span<const double> bb);
// n0: the thermal noise per carrier, for the noise gate (INF: none).
Support delay_support(const Mat<cd>& h_pilot, std::span<const double> bb, double floor_db = -15.0,
                      double n0 = INF);
int window_shift(Support support);

// _support_basis: an orthonormal basis U (nc x r) of the support's delays
// across the carriers (the span of the SVD's leading r vectors; which basis
// is not specified), r, the uncapped rank, and 1 - each carrier's leverage.
// Cached per (bb, support, slack), thread-safe.
struct Basis {
    Mat<cd> u;
    int r = 0, r_full = 0;
    std::vector<double> keep;
};
std::shared_ptr<const Basis> support_basis(std::span<const double> bb, int d0, int d1, int slack = SUPPORT_SLACK);

struct Smoothed {
    Mat<cd> hs;
    double n0 = INF;  // inf where the residual has < 2 dimensions
    int r = 0;
    std::vector<double> keep;
};
Smoothed freq_smooth(const Mat<cd>& h_pilot, Support support, std::span<const double> bb, int slack = SUPPORT_SLACK);
// The burst's projection order: SUPPORT_SLACK or PADDED_SLACK (equalizer.py).
int support_slack(const Mat<cd>& h_pilot, Support support, std::span<const double> bb, double n0_pre = INF);

double preamble_noise(const Mat<cd>& h_repeats);
std::vector<double> preamble_noise_k(const Mat<cd>& h_repeats);
// samples in 1..tables::GAMMA_Q99_MAX; throws std::out_of_range outside.
std::vector<double> per_carrier_noise(std::span<const double> power_k, int samples);
double doppler_corr(double dt, double spread_hz);
double measure_spread(const Mat<cd>& hs, double n0_s);
double filter_spread(const Mat<cd>& hs, double n0_s, double measured);

struct Estimate {
    int n_f = 0, nc = 0;
    Mat<cd> h;        // (n_f * 5, nc): frame f, data symbol s at row f * 5 + s
    Mat<double> mse;  // the same shape
    double n0 = 0, p_sig = 0, spread_hz = 0;
    double filter_spread_hz = 0;  // the time filters' (refine's too); spread_hz is the reported one
    int slack = SUPPORT_SLACK;  // the support's padding (support_slack; refine's too)
    std::vector<double> n0_k;
};
// n0_pre_k empty: None. Throws std::invalid_argument without a noise estimate.
Estimate estimate(const Mat<cd>& h_pilot, Support support, std::span<const double> bb, double n0_pre = INF,
                  std::span<const double> n0_pre_k = {});

Mat<cd> time_shift_phase(std::span<const double> shift, std::span<const double> bb);

// Returns h and mse, (F * S, nc). p_sig, spread_hz, n0, slack: estimate()'s.
std::pair<Mat<cd>, Mat<double>> refine(const Mat<cd>& h_pilot, std::span<const double> t_pilot, const Mat<cd>& z,
                                       const Mat<double>& w, const Mat<double>& t_rows, Support support,
                                       double p_sig, double spread_hz, double n0, std::span<const double> bb,
                                       int slack = SUPPORT_SLACK);

}  // namespace data2g::equalizer
