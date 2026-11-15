#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace AetherSDR::txlin {

// Analysis windows for the TX linearity analyzer, with the two correction
// factors that make a bin magnitude mean something.
//
// WHY THIS IS ITS OWN FILE, AND WHY THE DEFAULT IS WHAT IT IS.
//
// Window choice matters more than any other single decision in this analyzer.
// The measurement is "how far below the tones do the distortion products sit",
// and a window's own spectral leakage puts a skirt around each tone that reads
// exactly like a distortion product. Hann's first sidelobe is -31 dB and its
// skirt falls at 18 dB/octave; against a -40 dBc IMD3 that is not a small
// error, it is the entire measurement. Anything below roughly -50 dBc simply
// cannot be seen through a Hann window no matter how many averages are taken,
// because leakage is coherent and averaging does not reduce it.
//
// The default is therefore the 7-term Blackman-Harris (about -92 dB peak
// sidelobe), which puts the window's own contribution far below the -60 to
// -70 dBc region where a well-behaved amplifier's IMD3 actually lives. It costs
// resolution — the main lobe is wide — but resolution is cheap here (11.7 Hz
// bins at 192 kHz with a 16k transform) and dynamic range is not.
//
// The window is selectable because a wider main lobe genuinely hurts when tone
// spacing is small, but the default exists for the reason above: nobody should
// "optimize" it back to Hann and quietly cap the instrument at -50 dBc.
//
// Coefficients: Harris, "On the Use of Windows for Harmonic Analysis with the
// Discrete Fourier Transform", Proc. IEEE 66(1), 1978, and the 7-term minimum
// sidelobe extension tabulated in Rabiner & Gold / used throughout the SDR
// literature. Derived here from the published cosine-sum coefficients rather
// than copied from any implementation.
enum class WindowKind {
    BlackmanHarris7,  // ~-92 dB sidelobes. The default; see above.
    BlackmanHarris4,  // ~-92 dB nominal, narrower main lobe than the 7-term
    Hann,             // ~-31 dB. Offered for comparison, never as the default.
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
    // Nominal peak sidelobe level, dB. Documentation and the leakage
    // regression's expectation — not used in any signal-path arithmetic.
    double nominalSidelobeDb = 0.0;
    // Half-width of the main lobe in bins. For a K-term cosine-sum window the
    // main lobe spans exactly +/-K bins, which is why this is an integer and
    // not a measurement. Everything that needs "how far from this tone is still
    // this tone" uses it: separating the two fundamentals, excluding tone
    // skirts from the noise-floor estimate, and sizing the search window around
    // each predicted IMD frequency.
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
