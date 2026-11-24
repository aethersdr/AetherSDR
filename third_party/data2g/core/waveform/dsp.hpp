// data2g/waveform/dsp.py: heterodyne, frequency correction, the TX clipper.
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "generated/config.hpp"

namespace data2g::waveform {

using cdouble = std::complex<double>;

// Real passband -> complex baseband, a pure heterodyne by FCENTER (16
// exact phasors). `n0`: x[0]'s index in a stream, so chunks share a phase.
std::vector<cdouble> to_baseband(std::span<const double> x, std::int64_t n0 = 0);

// z * exp(-2j pi f n / FS), the phase reduced to [0, 1) turn first.
std::vector<cdouble> freq_correct(std::span<const cdouble> z, double f_hz);

// modem.ace_projector's hook: the data cells back into their regions.
using Projector = std::function<std::vector<double>(std::span<const double>)>;

// Envelope clip-and-filter for PEP control (dsp.tx_condition): one pass
// per `overshoot` factor, each a clip of the analytic signal's magnitude
// (scale ** k) and the firwin(201, bandpass) filter; with `project`, it
// runs after each of those passes, then the `closing` passes. Power, and
// the final unit-RMS level, over x[active_lo, active_hi) only.
std::vector<double> tx_condition(std::span<const double> x, double clip_headroom_db,
                                 std::span<const double> overshoot = config::CLIP_OVERSHOOT,
                                 std::size_t active_lo = 0,
                                 std::size_t active_hi = std::numeric_limits<std::size_t>::max(),
                                 std::pair<double, double> bandpass = {config::TX_BANDPASS[0], config::TX_BANDPASS[1]},
                                 const Projector& project = {}, std::span<const double> closing = {});

// Envelope (PEP) peak-to-average power ratio, dB.
double papr_db(std::span<const double> x);

}  // namespace data2g::waveform
