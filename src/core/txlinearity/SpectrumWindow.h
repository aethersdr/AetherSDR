#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace AetherSDR::txlin {

// Analysis windows with coherent-gain and equivalent-noise-bandwidth factors.
// The default trades a wider main lobe for lower sidelobes. Usable measurement
// depth also depends on bin offsets, tone spacing, FFT size and receiver noise;
// no window supplies a universal IMD floor. Averaging reduces estimate variance,
// not coherent leakage. Coefficients are specified in SpectrumWindow.cpp.
enum class WindowKind {
    BlackmanHarris7,  // The default; widest Blackman-Harris main lobe here.
    BlackmanHarris4,  // Narrower main lobe than the 7-term window.
    Hann,             // Narrower main lobe with higher sidelobes.
    FlatTop,          // amplitude-accurate, very wide main lobe
};

// A window plus everything needed to convert bin magnitudes into physical
// quantities.
//
//   coherentGain = sum(w)/N        — divide an amplitude spectrum by sum(w) to
//                                    recover a tone's true amplitude.
//   enbwBins     = N*sum(w^2)/sum(w)^2
//                                  — the width, in bins, of the ideal
//                                    rectangular filter with the same noise
//                                    power gain. Integrated band power is
//                                    sum(|S[k]|^2)/enbwBins; that one
//                                    expression is correct for BOTH a discrete
//                                    tone and a noise-like band (Parseval:
//                                    sum_k |W(k)|^2 = N*sum_n w[n]^2), which is
//                                    why ACPR, shoulder and tone power all go
//                                    through it.
struct AnalysisWindow {
    std::vector<double> taps;
    double sumTaps = 0.0;
    double coherentGain = 1.0;
    double enbwBins = 1.0;
    // Descriptive hint only; neither signal arithmetic nor leakage acceptance
    // uses this value to establish a measurement floor.
    double nominalSidelobeDb = 0.0;
    // Conservative exclusion half-width in FFT bins: K for a K-term window.
    // Used to separate fundamentals, exclude skirts from the noise estimate
    // and size each predicted IMD search region.
    int mainLobeHalfBins = 1;
};

// Build a window of `n` taps. `n` must be >= 2; smaller returns an empty
// window with unity factors, which the analyzer rejects up front.
AnalysisWindow makeWindow(WindowKind kind, std::size_t n);

// Stable identifiers for settings persistence and CSV export stamps. Round-trip
// with windowKindFromKey(); an unknown key falls back to the default rather
// than throwing, because a corrupt settings value must not stop the app.
std::string windowKindKey(WindowKind kind);
WindowKind windowKindFromKey(const std::string& key);

// Display names for the UI, matching the on-screen label convention.
std::string windowKindLabel(WindowKind kind);

}  // namespace AetherSDR::txlin
