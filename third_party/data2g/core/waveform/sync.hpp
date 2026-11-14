// data2g/waveform/sync.py: preamble detection, timing and CFO.
//
// Each preamble repeat is matched-filtered against the band's template on
// a grid of CFO hypotheses, and neighbouring repeats' outputs correlated:
//   D[n] = max_f |sum_{r>=1} c_f[n + CP + rM] conj(c_f[n + CP + (r-1)M])| / noise
// See the Python module for why. Every grid point is independent of the
// others (repeat_corrs, raw_stat), so they can later run in parallel.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "util/mat.hpp"
#include "waveform/ofdm.hpp"

namespace data2g::waveform {

inline constexpr double STEP_HZ = 12.5;
inline constexpr double NOISE_QUANTILE = 0.2;
inline constexpr std::array<double, 2> NOISE_REF_HZ = {-625.0, 625.0};
inline constexpr std::array<double, 2> NOISE_REF_HZ_W48 = {-250.0, 250.0};  // sync.NOISE_REF_BY_BAND
inline constexpr double LEVEL_FLOOR_DB = 1.0;  // sync.LEVEL_FLOOR_DB
inline constexpr int ALTERNATIVES = 3;
inline constexpr int TIME_ALTERNATIVES = 4;

struct SyncError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Acquisition {
    std::int64_t preamble_start;  // first preamble sample (CP start)
    double freq_offset;           // Hz
    double metric;                // detection statistic at the peak
    std::vector<std::pair<std::int64_t, double>> alternatives;  // (start, Hz), best first
};

// The searched CFO hypotheses, -reach..reach in STEP_HZ.
std::vector<double> cfo_grid(double reach = config::ACQUIRE_REACH_HZ);

// The band's template's repeat window [PREAMBLE_CP, PREAMBLE_CP + M), unit norm.
std::vector<cdouble> unit_template(const Band& band);

// c_f[n] = sum_k z[n+k] conj(t[k] e^{j 2 pi f k / FS}), "valid" outputs.
std::vector<cdouble> repeat_corr(std::span<const cdouble> z, std::span<const cdouble> t, double f);

// repeat_corr for each f in `freqs` (multiples of STEP_HZ), one row each:
// one FFT of z and of t, then per f an inverse FFT of the product with
// t's spectrum rolled by whole bins (sync.repeat_corrs).
Mat<cdouble> repeat_corrs(std::span<const cdouble> z, std::span<const cdouble> t, std::span<const double> freqs);

// The noise level of matched filter outputs C (bins, starts): per bin
// NOISE_QUANTILE of |c_f|^2 scaled to an exponential's mean, the lowest
// bin's (modem::find_copy without a StreamDetector's level).
double noise_level(const Mat<cdouble>& C);

struct RawStat {
    Mat<double> S;             // (grid, starts) before noise normalization
    std::vector<double> q;     // each bin's noise level, NOISE_REF_HZ bins last
    std::vector<double> freqs; // the searched hypotheses
    Mat<cdouble> outs;         // each searched bin's c_f, if asked for
};

// sync._raw_stat. repeats 0: the band's. levels_from: the levels from the
// outputs from there on, by one partition (StreamDetector).
RawStat raw_stat(std::span<const cdouble> z, const Band& band, double reach = config::ACQUIRE_REACH_HZ,
                 int repeats = 0, std::optional<std::size_t> levels_from = {}, bool keep_outs = false);

// (S / lowest noise level, the hypotheses).
std::pair<Mat<double>, std::vector<double>> detection_stat(std::span<const cdouble> z, const Band& band,
                                                           double reach = config::ACQUIRE_REACH_HZ, int repeats = 0);

// The earliest local maximum within `search` samples ahead of `peak`
// holding `frac` of its power; `peak` if none.
std::size_t first_path(std::span<const double> power, std::size_t peak, int search = config::FIRST_PATH_SEARCH,
                       double frac = config::FIRST_PATH_FRAC, bool cyclic = false);

// (start, CFO) from a detection at (n, f): first-path timing, and the
// phase advance between repeats' outputs for the CFO.
std::pair<std::int64_t, double> refine(std::span<const cdouble> z, const Band& band, std::int64_t n, double f);

// Detections in time order: per threshold crossing, the peak within `span`.
std::vector<std::size_t> crossings(std::span<const double> D, double threshold, std::size_t span, std::size_t limit);

// Find the preamble in baseband z. threshold: the band's if unset. search:
// [start, end) starts to hunt in. S: a precomputed statistic for z's
// starts (StreamDetector; -1 = do not search). Throws SyncError.
Acquisition acquire(std::span<const cdouble> z, const Band& band, std::optional<double> threshold = {},
                    double reach = config::ACQUIRE_REACH_HZ,
                    std::optional<std::pair<std::int64_t, std::int64_t>> search = {}, const Mat<double>* S = nullptr);

// detection_stat for a stream, each start's statistic computed once
// (sync.StreamDetector). The noise level is the median of the last CHUNKS
// fed chunks' per-bin levels, then the lowest bin's. Matched filter
// outputs are kept too (C from stream index c0) for modem.find_copy.
class StreamDetector {
public:
    static constexpr int CHUNKS = 8;

    explicit StreamDetector(const Band& band, double reach = config::ACQUIRE_REACH_HZ);

    void reset();
    void restart_at(std::int64_t pos);  // reset(), the stream resuming at index `pos`
    // Native only (receiver.cpp's catch-up after a confirmed burst; Python's
    // receiver feeds it all): jump over the samples before stream index
    // `pos` (>= fed), keeping the noise-level history.
    void skip_to(std::int64_t pos);
    void feed(std::span<const cdouble> z);  // the next contiguous baseband samples
    void trim(std::int64_t start);          // drop starts before stream index `start`
    std::optional<double> level() const;    // the noise level stat() divides by
    Mat<double> stat(std::int64_t lo, std::int64_t hi) const;  // starts [lo, hi), -1 where not computed

    std::size_t bins() const { return S_.size(); }
    std::span<const double> S(std::size_t bin) const { return std::span(S_[bin]).subspan(s_off_); }  // from s0
    std::span<const cdouble> C(std::size_t bin) const { return std::span(C_[bin]).subspan(c_off_); }  // from c0

    // The kept outputs for stream starts from max(start, c0): that index,
    // and per bin a pointer to its row there (cols columns each).
    struct Outputs {
        std::int64_t start;
        std::vector<const cdouble*> rows;
        std::size_t cols;
    };
    Outputs outputs_from(std::int64_t start) const;

    const Band& band;
    double reach;
    std::int64_t span;
    // State, public as in Python (read-only outside: restart_at / skip_to move `fed`).
    std::vector<cdouble> tail;  // the last span - 1 samples fed
    std::int64_t s0 = 0, fed = 0, c0 = 0;
    std::vector<std::vector<double>> levels;  // per chunk, each bin's level

private:
    // per bin; trim() advances an offset and compacts only past half, so a
    // hop's trim moves no memory (C is ~8 MB on the live receiver)
    std::vector<std::vector<double>> S_;
    std::vector<std::vector<cdouble>> C_;
    std::size_t s_off_ = 0, c_off_ = 0;
};

}  // namespace data2g::waveform
