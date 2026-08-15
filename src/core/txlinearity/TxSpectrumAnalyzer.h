#pragma once

#include "SpectrumWindow.h"

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace AetherSDR::txlin {

// Welch-averaged complex-baseband spectrum estimator for the linearity
// analyzer.
//
// Distinct from Hl2Spectrum (the HL2 panadapter) on purpose. That one is a
// display path: Hanning window, one frame at a time, float output, tuned for
// frame rate. This one is a measurement path — low-sidelobe window, power
// averaging across frames, double precision throughout, and it keeps the
// linear power spectrum rather than dB so band powers can be integrated
// correctly. Sharing them would force one of the two to compromise, and the
// display can afford to and the instrument cannot.
//
// AVERAGING. Welch averaging of |X[k]|^2 across frames reduces the variance of
// the noise estimate by ~1/sqrt(M) while leaving coherent tones untouched, so
// it lowers the visible noise floor and lets weaker products be resolved. It
// does NOT reduce window leakage, which is coherent — that is the window's job
// and the reason for the default in SpectrumWindow.h.
//
// SCALING. accumulate() stores mean |X[k]/sum(w)|^2. With that normalisation:
//   - a complex exponential of amplitude A peaks at |S[k]|^2 = A^2, so a tone's
//     power reads directly off the peak bin;
//   - integrated band power is sum_{k in band} |S[k]|^2 / enbwBins, which is
//     correct for tones and for noise alike (see AnalysisWindow).
// The capture path delivers IQ normalised to a full-scale magnitude of 1.0, so
// everything downstream is dBFS and every published figure is a ratio of two of
// them.
//
// THREADING. Owns an FFTW plan. Construction and destruction allocate and call
// into FFTW's global planner, which is not thread-safe; fftw_execute itself is.
// Construct on the analysis thread, off the capture thread, and never from two
// threads at once — the same rule Hl2Spectrum documents.
class TxSpectrumAnalyzer {
public:
    TxSpectrumAnalyzer(int fftSize, WindowKind window);
    ~TxSpectrumAnalyzer();
    TxSpectrumAnalyzer(const TxSpectrumAnalyzer&) = delete;
    TxSpectrumAnalyzer& operator=(const TxSpectrumAnalyzer&) = delete;

    [[nodiscard]] int fftSize() const noexcept { return m_fftSize; }
    [[nodiscard]] const AnalysisWindow& window() const noexcept { return m_window; }

    // Transform one frame of exactly fftSize samples and fold it into the
    // running average. A short or long span is rejected (returns false) rather
    // than silently zero-padded: zero-padding changes the effective window and
    // therefore the leakage floor, which is the one property this instrument
    // depends on.
    bool accumulate(std::span<const std::complex<float>> frame);

    void reset() noexcept;

    [[nodiscard]] int averages() const noexcept { return m_averages; }

    // Mean linear power spectrum, DC-CENTRED (DC at index fftSize/2) to match
    // the convention already used by Hl2Spectrum. Empty until the first
    // accumulate() succeeds.
    [[nodiscard]] const std::vector<double>& powerSpectrum() const noexcept { return m_avg; }

    // Hz per bin, given the capture sample rate.
    [[nodiscard]] static double binHz(double sampleRateHz, int fftSize)
    {
        return (fftSize > 0) ? sampleRateHz / double(fftSize) : 0.0;
    }

    // Baseband frequency of a DC-centred bin index, in Hz (negative below DC).
    [[nodiscard]] double binFreqHz(int index, double sampleRateHz) const;

    // Nearest DC-centred bin index to a baseband frequency. Clamped into range;
    // callers that care about out-of-range must check the frequency themselves.
    [[nodiscard]] int freqToBin(double freqHz, double sampleRateHz) const;

    // Convert the averaged power spectrum to dBFS for display. Values at or
    // below the floor are clamped to `floorDb` so an all-zero capture produces
    // a flat trace instead of -inf.
    void toDb(std::vector<float>& out, float floorDb = -200.0f) const;

private:
    int m_fftSize = 0;
    AnalysisWindow m_window;
    int m_averages = 0;
    std::vector<double> m_avg;   // DC-centred mean |S[k]|^2

    void* m_in = nullptr;    // fftw_complex[m_fftSize]
    void* m_out = nullptr;   // fftw_complex[m_fftSize]
    void* m_plan = nullptr;  // fftw_plan
};

// ── Peak estimation ─────────────────────────────────────────────────────────

struct InterpolatedPeak {
    bool valid = false;
    double freqHz = 0.0;
    double powerDb = 0.0;
    // Fractional bin offset actually applied, for diagnostics. |delta| > 0.5
    // means the search picked the wrong bin and the caller should widen.
    double deltaBins = 0.0;
};

// Locate the largest bin within [loBin, hiBin] of a DC-centred power spectrum
// and refine it by parabolic interpolation on the dB magnitudes.
//
// Parabolic-on-log is the right estimator for a tone under a smooth cosine-sum
// window: the main lobe of such a window is very nearly a parabola in dB near
// its peak, so three points recover both the true frequency and the true
// amplitude to a fraction of a dB even when the tone falls between bins.
// Without it, a tone landing on a bin boundary reads up to ~1.4 dB low under
// Blackman-Harris — an error that would propagate straight into every dBc
// figure. (Jacobsen's complex three-point estimator is more accurate for
// frequency alone but needs the complex spectrum, which averaging has already
// discarded; amplitude is what the dBc numbers actually depend on.)
InterpolatedPeak interpolatePeak(const std::vector<double>& powerDcCentred,
                                 int loBin, int hiBin,
                                 double sampleRateHz);

}  // namespace AetherSDR::txlin
