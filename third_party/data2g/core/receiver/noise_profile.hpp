// data2g/receiver.py's NoiseProfile: the passband's noise between bursts,
// per sub-band, from 0.1 s blocks of idle audio (median power, and the 90th
// percentile over it), impulses per minute, and a burst's in-band SNR from
// power alone. The engine feeds it what it hears and marks bursts and its
// own transmissions. Times are sample indices, round(t * FS).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "config/config.hpp"

namespace data2g::receiver {

struct NoiseSnapshot {
    std::array<double, 5> db{}, tail_db{};
    double impulses_per_min = 0.0;
    int blocks = 0;
};

class NoiseProfile {
public:
    static constexpr std::array<std::pair<int, int>, 5> BANDS_HZ = {
        {{350, 950}, {950, 1300}, {1300, 1750}, {1750, 2100}, {2100, 2700}}};
    static constexpr int BLOCK = config::FS / 10;
    static constexpr std::size_t WINDOW = 600;
    static constexpr double COMMIT_S = 3.0;
    static constexpr std::size_t MIN_BLOCKS = 20;
    static constexpr double RECOVER_S = 0.6;
    static constexpr int PIECE = config::FS / 100;  // impulse detection: 10 ms pieces
    static constexpr double IMPULSE_X = 10.0;       // over the block's median piece RMS
    static constexpr std::size_t RECENT = 400;      // blocks (40 s) of spectra kept for span_snr_db
    using Spectrum = std::vector<double>;           // a block's power spectrum, rfft bins 0..BLOCK/2

    NoiseProfile();
    void feed(std::span<const double> x, double t_start);  // heard from t_start (s); a gap starts a new block
    void mark(double start, double end);                    // not noise from start to end (s)
    std::optional<NoiseSnapshot> snapshot() const;
    // A BLOCK's power spectrum (Hann window), as every block is measured.
    Spectrum spectrum(std::span<const double> blk) const;
    // The fed blocks wholly inside start..end (s).
    std::vector<Spectrum> span_spectra(double start, double end) const;
    // A burst's in-band SNR from power alone (receiver.py NoiseProfile.span_snr_db):
    // its blocks' mean power in lo..hi Hz over the floor (the kept blocks'
    // median, or floor_spectra's), excess over 1, in SNR_REF_BW_HZ; dB.
    std::optional<double> span_snr_db(const std::vector<Spectrum>& spectra, double lo, double hi,
                                      const std::vector<Spectrum>* floor_spectra = nullptr) const;
    // the span's in-band power over the floor in the kept blocks' own spread (median, MAD), scaled to
    // the span (receiver.py NoiseProfile.span_z): a carrier sense, the same test in any band and span
    std::optional<double> span_z(const std::vector<Spectrum>& spectra, double lo, double hi) const;

private:
    struct Block {
        std::int64_t start, end;
        std::array<double, 5> p;
        int impulses;
        Spectrum spec;
    };
    struct Recent {
        std::int64_t start, end;
        Spectrum spec;
    };
    std::vector<double> win_, buf_;
    std::int64_t s0_ = 0;
    std::deque<Block> pending_;
    std::vector<std::pair<std::int64_t, std::int64_t>> busy_;
    std::deque<std::array<double, 5>> kept_;
    std::deque<int> kept_impulses_;
    std::deque<Spectrum> kept_spec_;
    static std::vector<std::size_t> band_bins(double lo, double hi);  // rfft bins in lo..hi Hz
    static double band_mean(const Spectrum& s, const std::vector<std::size_t>& band);
    std::deque<Recent> recent_;
};

}  // namespace data2g::receiver
