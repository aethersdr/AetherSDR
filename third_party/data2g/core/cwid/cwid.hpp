// data2g/cwid.py: the CW (Morse) station ID's audio, and CwSense, which
// hears one on air so a waiting station holds instead of keying over it.
// Adapted from ion-hf (crates/ion-dsp/src/cw.rs, crates/ion/src/link/node.rs),
// relicensed for Data2G by its author.
#pragma once

#include <array>
#include <cstdint>
#include <complex>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "generated/config.hpp"

namespace data2g::cwid {

inline constexpr double WPM = 20.0;
inline constexpr double MIN_WPM = 15.0, MAX_WPM = 40.0;
inline constexpr double FREQ_HZ = config::FCENTER;
inline constexpr int RAMP = config::FS / 200;  // raised-cosine key edge, samples (5 ms)

std::string id_text(std::string_view call);  // "DE " and the call less any SSID
std::vector<std::pair<bool, int>> keying(std::string_view text);  // (key down, dots) runs
int unit_samples(double wpm);
// `text` keyed at `wpm` on `freq`, peak 1.0. Throws std::invalid_argument outside MIN_WPM..MAX_WPM.
std::vector<double> audio(std::string_view text, double wpm = WPM, double freq = FREQ_HZ);
double seconds(std::string_view text, double wpm = WPM);

// cwid.CwSense: per 20 ms window hit (key-down), quiet (key-up) or elsewhere (a burst); active when the last
// WIN_N windows hold MIN_HITS hits, MIN_QUIET quiet ones and at most MAX_ELSEWHERE elsewhere, the hits
// keyed: at least MIN_RUNS separate runs, none longer than MAX_RUN (a steady carrier's edge is one run).
class CwSense {
public:
    static constexpr int WIN = config::FS / 50;
    static constexpr double STEP_HZ = 25.0;
    static constexpr int K = 12, TONE_K = 2, APART_K = 4, FLOOR_K = 6;
    static constexpr double RATIO = 10.0, LEAK = 0.031622776601683794;  // 10^-1.5
    static constexpr double FLOOR_DOWN = 0.1, FLOOR_UP = 0.004;
    static constexpr int WIN_N = 50, MIN_HITS = 12, MIN_QUIET = 4, MAX_ELSEWHERE = 6;
    static constexpr int MIN_RUNS = 2, MAX_RUN = 16;  // a dash at MIN_WPM is 12 windows
    static constexpr int HITS_FROM = 3;

    explicit CwSense(double center = FREQ_HZ);
    void reset();
    int classify(std::span<const double> x);  // 1 + the tone's step + TONE_K for a hit, 0 quiet, -1 elsewhere
    bool feed(std::span<const double> x);     // any block size; whether a CW ID is on air now
    bool active() const { return active_; }
    // the start of the HITS_FROM-th latest hit window while active, samples since reset (-1: fewer yet)
    std::int64_t hits_from() const { return hits_.size() == HITS_FROM ? hits_.front() : -1; }

private:
    std::vector<std::complex<double>> basis_;  // (2K+1) x WIN, Hann-windowed DFT rows
    std::vector<double> buf_;
    std::deque<int> hist_;
    double floor_ = 0.0;
    bool active_ = false;
    std::int64_t n_ = 0;
    std::deque<std::int64_t> hits_;
    std::deque<double> tones_;
};

}  // namespace data2g::cwid
