// data2g/waveform/ofdm.py: DFT-matrix OFDM per band.
//
// Passband modulation generates the real transmit waveform directly.
// Demodulation works on dsp::to_baseband's complex baseband, where a
// carrier at f Hz sits at f - FCENTER.
#pragma once

#include <complex>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "config/config.hpp"
#include "util/mat.hpp"

namespace data2g::waveform {

using cdouble = std::complex<double>;

// exp(sign * 2j*pi * cycles_num / FS) for integer cycles_num, reduced
// modulo FS in integers first (ofdm._phasor), so the argument is under
// one turn and the value is the same on every libm.
cdouble phasor(std::int64_t cycles_num, int sign = 1);

// Everything carrier-specific for one config::Band (ofdm.Band).
struct Band {
    const config::Band* spec = nullptr;
    std::vector<std::int64_t> freqs;  // passband carrier frequencies, Hz
    std::vector<std::int64_t> bb;     // the same at baseband
    Mat<cdouble> mod;                 // (NSYM, nc), phase reference at n = NCP
    Mat<cdouble> demod;               // (nc, M), one useful window
    Mat<double> demod_re, demod_im;   // demod transposed, (M, nc), split (demod_window's layout)
    std::vector<cdouble> pilot;       // (nc,), unit magnitude
    std::vector<cdouble> preamble_template;  // complex baseband preamble replica

    int nc() const { return spec->nc; }
    int preamble_samples() const { return config::preamble_samples(*spec); }
    double preamble_threshold() const;
    // BandSpec.tx_bandpass: the carriers plus 75 Hz each side ("w": TX_BANDPASS).
    std::pair<double, double> tx_bandpass() const;

    // (n_sym, nc) symbols -> real waveform, n_sym * NSYM samples.
    std::vector<double> modulate_symbols(const Mat<cdouble>& symbols) const;
    // One useful window from `start - backoff`, (2/M) demod @ window. A
    // window past the end is zero-padded and a negative start slices as
    // Python's z[s:s+M] does (wrapping), so the two agree everywhere.
    std::vector<cdouble> demod_window(std::span<const cdouble> z, std::int64_t start, std::int64_t backoff = 0) const;
    // Real passband preamble: the pilot symbol, M-periodic over the block.
    std::vector<double> preamble_waveform() const;
};

// The band named `name` (config::BANDS); throws std::out_of_range.
// Built once, then read-only.
const Band& band(std::string_view name);

}  // namespace data2g::waveform
