// numpy / scipy primitives the waveform needs, replicated closely enough
// that the Python reference and the port agree to the last bit where IEEE
// allows, and to an FFT's rounding where it does not. No data2g module
// corresponds: data2g/waveform/dsp.py is native/core/waveform/dsp.*.
//
// firwin, hilbert, fftconvolve and convolve_same are lifted from SSTVAE's
// native/core/dsp/dsp.cpp; pairwise_sum and quantile are new.
#pragma once

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace data2g::dsp {

using cdouble = std::complex<double>;

// np.sum of a contiguous float64 / complex128 array: numpy's pairwise
// summation (8 accumulators, blocks of 128), from an initial 0. Not a
// plain loop: it rounds differently, and sums feed thresholds.
double pairwise_sum(std::span<const double> a);
cdouble pairwise_sum(std::span<const cdouble> a);

// np.quantile(v, q), method "linear" (numpy's _lerp included).
double quantile(std::vector<double> v, double q);

// scipy.signal.firwin(numtaps, (lo, hi), fs=fs, pass_zero=False): Hamming
// window, scale=True. Frequencies in Hz.
std::vector<double> firwin_bandpass(int numtaps, double lo_hz, double hi_hz, double fs);

// scipy.signal.firwin(numtaps, cutoff, fs=fs): lowpass, Hamming, scale=True.
std::vector<double> firwin_lowpass(int numtaps, double cutoff_hz, double fs);

// scipy.signal.hilbert: the analytic signal, via FFT.
std::vector<cdouble> hilbert(std::span<const double> x);

// np.convolve(a, v, mode="same"), direct sum, for len(a) >= len(v).
std::vector<double> convolve_same(std::span<const double> a, std::span<const double> v);

// scipy.fft.next_fast_len(n) (complex): pocketfft's good_size_cmplx.
std::size_t next_fast_len(std::size_t n);

// scipy.signal.fftconvolve(a, v, mode="valid") for complex a, v with
// len(a) >= len(v): FFT-based like scipy, so the two round alike.
std::vector<cdouble> fftconvolve_valid(std::span<const cdouble> a, std::span<const cdouble> v);

}  // namespace data2g::dsp
