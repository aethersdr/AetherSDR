// data2g/cpm.py: the constant-envelope M-FSK modes (grids, specs, burst
// layout, modulation, Costas sync, header ML, noncoherent soft bits).
//
// Every on-air tone pattern (sync blocks, header tones) is a frozen table
// (tables::CPM_GRIDS); nothing here draws random numbers. Unlike Python's,
// modulate() returns the audio before the TX bandpass: bandpass() is that
// step (cpm.modulate = bandpass(modulate)).
#pragma once

#include <complex>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "tables/cpm.hpp"
#include "util/mat.hpp"

namespace data2g::cpm {

using Grid = tables::CpmGrid;
using Spec = tables::CpmSpec;

// nullptr if there is no such grid / spec (data modes and control codewords).
const Grid* grid(std::string_view name);
const Spec* spec(std::string_view name);
const Grid& grid_of(const Spec& s);
const Spec& ctl(const Grid& g);

// The header's 10-bit value before its CRC-6: (mode index + 2 dup) << 6 | n_data.
int header_value(int index, int n_data, bool dup);
std::span<const std::uint8_t> header_symbols(const Grid& g, int value);

struct Layout {
    int n = 0;
    std::vector<int> sync_rows, sync_tones;
    std::vector<std::vector<int>> hdr_rows;
    std::vector<int> data_rows;
    int front = 0;
};

int stream_symbols(const Grid& g, int n_data, bool dup);
Layout layout(const Grid& g, int n_sym);
double burst_seconds(const Spec& s, int n_cw, bool dup = false);

std::vector<int> to_tones(const Grid& g, std::span<const std::uint8_t> bits);
// glide: the frequency trajectory smoothed over this fraction of a symbol (CpmTxFilter).
std::vector<double> tones(const Grid& g, std::span<const int> sym, double glide = 0.0);
// Every slot's coded bits (control first, twice if dup) -> audio before the
// TX bandpass, with the bandwidth cap's glide (tx_filter).
std::vector<double> modulate(const Spec& s, const std::vector<std::vector<std::uint8_t>>& coded, bool dup, int cap = 0);
// cpm.tx_filter: the grid's TX filter under bandwidth cap code `cap` (0..2).
const tables::CpmTxFilter& tx_filter(const Grid& g, int cap);
// cpm.TxFilter.overshoot: CLIP_OVERSHOOT, its last factor repeated to `passes`.
std::vector<double> overshoot(const tables::CpmTxFilter& f);
// cpm.bandpass: modulate()'s audio through the TX filter of cap code `cap`,
// clip and filter (waveform::tx_condition) over the tones +- its margin.
std::vector<double> bandpass(const Grid& g, std::span<const double> x, int cap = 0);

// (n_sym, m + 2 extra) tone energies from `start`, CFO removed; shares per row.
Mat<double> energies(const Grid& g, std::span<const double> x, std::int64_t start, int n_sym, double cfo, int extra = 0);
Mat<double> shares(const Mat<double>& E);

struct Detection {
    double score;
    std::int64_t start;
    double cfo;
};
// mid: the mid block alone, its first sample for the start (find_mid).
Detection detect(const Grid& g, std::span<const double> x, double reach_hz = 150.0, bool fine = true,
                 bool front_only = false, int n_sym = 0, double floor = -1.0, bool mid = false);

// (n_sym, m) energies of one codeword -> n_sym * bits LLRs, mapping order (the control codeword's).
std::vector<double> llrs(const Grid& g, const Mat<double>& E);
// -> n_sym * m symbol log-likelihoods, mapping order, value v's on tone gray(v) (a GF(M) data codeword's).
std::vector<double> symbol_ll(const Grid& g, const Mat<double>& E);

// cpm.tone_llr: per row and tone, that tone's log-likelihood ratio against a random one (floored by llr_eps).
Mat<double> tone_llr(const Grid& g, const Mat<double>& E);

struct Header {
    const Spec* spec;
    int n_data;
    bool dup;
    double score, runner_up;
    double front;  // the front block's part of score
};
Header read_header(const Grid& g, std::span<const double> x, std::int64_t s0, double cfo, int copies = 2);

struct Soft {
    std::vector<std::vector<double>> slots;  // per slot, mapping order
    Mat<double> E;                           // the stream's tone energies
};
Soft soft(const Grid& g, std::span<const double> x, std::int64_t s0, double cfo, int n_data, bool dup);

double peak_ratio(const Grid& g, std::span<const double> x, std::int64_t s0, double cfo, bool mid = false);

struct Lock {
    const Spec* spec;
    int n_data;
    bool dup;
    std::int64_t start;
    double cfo, score, header_score, header_margin;
    std::int64_t end, header_end;
    // find's candidate (wait): a front whose first header copy fell short. Only
    // start (the detected one), cfo, score and header_end (where x must reach
    // for late_lock) are set; spec is nullptr.
    bool wait = false;
    int copies = 0;  // header copies the lock was read from (an early lock: 1)
};
// threshold: nullopt = SYNC_THRESHOLD plus the lock floors (lock_llr, hdr_margin). hi: nullopt = len(x).
// wait: a candidate instead of nullopt while the second header copy is still arriving.
std::optional<Lock> find(const Grid& g, std::span<const double> x, std::optional<double> threshold = std::nullopt,
                         double reach_hz = 150.0, bool front_only = true, std::int64_t lo = 0,
                         std::optional<std::int64_t> hi = std::nullopt, bool wait = false);
// find's candidate (x holding its header_end) -> its lock from both header copies
// (lock_llr2, hdr_margin2), or nullopt.
std::optional<Lock> late_lock(const Grid& g, std::span<const double> x, const Lock& cand);
// A burst whose front was missed, from its mid block and both header copies
// (cpm.find_mid): the mid block's start in [lo, hi). hi: nullopt = len(x).
std::optional<Lock> find_mid(const Grid& g, std::span<const double> x, std::int64_t lo = 0,
                             std::optional<std::int64_t> hi = std::nullopt);
// cpm.confirm: a lock's second look, tone_llr over its mid block and second
// header copy at the locked word, unfloored; under CONFIRM_LLR the burst is
// not there (receiver::Receiver lets an early lock go). confirm_end: one past
// its second header copy's last sample.
inline constexpr double CONFIRM_LLR = -2.0;
double confirm(const Grid& g, std::span<const double> x, const Lock& lock);
std::int64_t confirm_end(const Grid& g, const Lock& lock);
// Samples from a burst's start to the end of its first `copies` header copies,
// the latest any word puts them.
std::int64_t hdr_span(const Grid& g, int copies);
// cpm.prime_lock (docs/arq.md §3a): a burst of grid g starting in [lo, hi], its CFO near `cfo`, any valid
// word, scored over every row the word fixes; nullopt under PRIME_LLR. x must hold the second header copy
// of a start at hi.
inline constexpr double PRIME_LLR = 16.0;
inline constexpr int PRIME_STEP = 4;
inline constexpr double PRIME_CFO[] = {-1.0 / 16, 0.0, 1.0 / 16};
std::optional<Lock> prime_lock(const Grid& g, std::span<const double> x, std::int64_t lo, std::int64_t hi, double cfo);

// cpm.measure: arq/phy's measure makes the effective-MI features (arq/predictor's)
// of `snr`, per symbol.
struct Measure {
    double snr_est, spread_est, frames;
    std::vector<double> snr;
};
Measure measure(const Grid& g, const Mat<double>& E, int n_sym);

// cpm.receive: an early lock's burst, whole in x (same sample origin) ->
// the per-slot soft bits and tone energies data2g.arq.phy reads.
struct Received {
    const Spec* spec;
    int n_cw, n_ctl_slots;  // slots, control included; control slots
    bool dup;
    std::vector<std::vector<double>> soft;
    Mat<double> E;
    double cfo;
    std::int64_t preamble_start, header_end;
};
// cpm.refine: a lock's start and CFO by the likelihood of the burst's first refine_symbols symbols.
std::pair<std::int64_t, double> refine(const Grid& g, std::span<const double> x, std::int64_t s0, double cfo,
                                       const Spec& spec, int n_data, bool dup);
Received receive(std::span<const double> x, const Lock& lock);

}  // namespace data2g::cpm
