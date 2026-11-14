// Rate conversion and input conditioning between the sound card and the
// modem: data2g/audio.py's Decimator, Interpolator and Blanker. Each keeps its
// state across calls, so a stream cut into chunks comes out as if it had
// been processed whole (per-chunk filtering clicks at every edge).
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace data2g::audio {

// scipy.signal.lfilter(b, 1.0, x, zi=zi) for an FIR b, as direct form II
// transposed. scipy takes a = 1 through np.convolve, whose dot products are
// BLAS's (summation order per CPU), so the two agree to ~1e-15, not bits.
// `zi` (len(b) - 1) is updated in place.
std::vector<double> lfilter_fir(std::span<const double> b, std::span<const double> x, std::vector<double>& zi);

// Device rate (a multiple of FS) -> FS: firwin(32 d + 1, 0.9 FS / 2) then
// every d-th sample, the phase carried across chunks. Only the kept outputs
// are computed (a dot product each), so a stream agrees with lfilter then
// [::d] to rounding, not bits.
class Decimator {
public:
    explicit Decimator(int rate);
    std::vector<double> operator()(std::span<const double> x);
    int factor() const { return d_; }
    const std::vector<double>& taps() const { return taps_; }

private:
    int d_;
    std::vector<double> taps_, rtaps_, hist_;  // rtaps_: taps reversed; hist_: the last len - 1 inputs
    std::int64_t phase_ = 0;
};

// FS -> device rate: zero-stuff by u, then firwin(32 u + 1, 0.9 FS / 2) * u.
// Polyphase: the zero-stuffed samples are never multiplied (agrees with the
// zero-stuffed lfilter to rounding, not bits).
class Interpolator {
public:
    explicit Interpolator(int rate);
    std::vector<double> operator()(std::span<const double> x);
    int factor() const { return u_; }
    const std::vector<double>& taps() const { return taps_; }

private:
    int u_;
    std::vector<double> taps_, phases_, hist_;  // phases_: u rows of PH_LEN reversed polyphase taps
};

// Impulse blanker, after modem73's; audio.Blanker has the reasoning. In
// 10 ms blocks against a slow envelope of |x|: samples over ZERO x env are
// zeroed (with GUARD neighbours), those over LIMIT x env limited to it; a
// block whose median is over RESYNC x env is a level step and resets env.
class Blanker {
public:
    static constexpr int BLOCK = 80;  // FS / 100
    static constexpr double ZERO = 8.0, LIMIT = 6.0, RESYNC = 2.0;
    static constexpr int TAU = 683;
    static constexpr int GUARD = 8;

    std::vector<double> operator()(std::span<const double> x);

    double env = 0.0;
    std::int64_t n_blanked = 0;  // samples zeroed or limited
};

}  // namespace data2g::audio
