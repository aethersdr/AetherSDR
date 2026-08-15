#include "SpectrumWindow.h"

#include <array>
#include <cmath>
#include <numbers>

namespace AetherSDR::txlin {
namespace {

// Cosine-sum window: w[n] = sum_j (-1)^j a_j cos(2*pi*j*n/N).
//
// PERIODIC form (divisor N, not N-1). For spectral analysis the periodic window
// is the correct one — it is the sampled version of the continuous window that
// makes the DFT's implicit periodic extension seamless. The symmetric (N-1)
// form belongs to FIR filter design; using it here leaves a one-sample
// discontinuity that raises the far sidelobes by tens of dB and would quietly
// undo the entire reason for choosing a -92 dB window.
template <std::size_t K>
std::vector<double> cosineSum(const std::array<double, K>& a, std::size_t n)
{
    std::vector<double> w(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        double v = 0.0;
        double sign = 1.0;
        for (std::size_t j = 0; j < K; ++j) {
            v += sign * a[j]
                 * std::cos(2.0 * std::numbers::pi * double(j) * double(i) / double(n));
            sign = -sign;
        }
        w[i] = v;
    }
    return w;
}

// Harris 1978, Table 1 / eq. 33: 4-term minimum-sidelobe Blackman-Harris.
constexpr std::array<double, 4> kBh4{0.35875, 0.48829, 0.14128, 0.01168};

// 7-term minimum-sidelobe extension of the same family. ~-92 dB peak sidelobe
// with a far-sidelobe rolloff steep enough that a tone 60 dB down is not
// contaminated by the neighbouring tone's skirt at the IMD3 offsets.
constexpr std::array<double, 7> kBh7{0.27105140069342,
                                     0.43329793923448,
                                     0.21812299954311,
                                     0.06592544638803,
                                     0.01081174209837,
                                     0.00077658482522,
                                     0.00001388721735};

// Hann as a 2-term cosine sum, for comparison only.
constexpr std::array<double, 2> kHann{0.5, 0.5};

// SRS/HP flat-top: amplitude-flat across a bin at the cost of a very wide main
// lobe. Useful when the operator cares about absolute tone amplitude more than
// about separating close products.
constexpr std::array<double, 5> kFlatTop{0.21557895, 0.41663158, 0.277263158,
                                         0.083578947, 0.006947368};

}  // namespace

AnalysisWindow makeWindow(WindowKind kind, std::size_t n)
{
    AnalysisWindow win;
    if (n < 2) {
        return win;
    }

    switch (kind) {
    case WindowKind::BlackmanHarris7:
        win.taps = cosineSum(kBh7, n);
        win.nominalSidelobeDb = -92.0;
        win.mainLobeHalfBins = int(kBh7.size());
        break;
    case WindowKind::BlackmanHarris4:
        win.taps = cosineSum(kBh4, n);
        win.nominalSidelobeDb = -92.0;
        win.mainLobeHalfBins = int(kBh4.size());
        break;
    case WindowKind::Hann:
        win.taps = cosineSum(kHann, n);
        win.nominalSidelobeDb = -31.5;
        win.mainLobeHalfBins = int(kHann.size());
        break;
    case WindowKind::FlatTop:
        win.taps = cosineSum(kFlatTop, n);
        win.nominalSidelobeDb = -93.6;
        win.mainLobeHalfBins = int(kFlatTop.size());
        break;
    }

    double sum = 0.0;
    double sumSq = 0.0;
    for (double t : win.taps) {
        sum += t;
        sumSq += t * t;
    }
    win.sumTaps = sum;
    win.coherentGain = sum / double(n);
    // Guard against a degenerate window rather than emitting inf/NaN into the
    // measurement arithmetic: every caller divides by enbwBins.
    win.enbwBins = (sum != 0.0) ? (double(n) * sumSq / (sum * sum)) : 1.0;
    return win;
}

std::string windowKindKey(WindowKind kind)
{
    switch (kind) {
    case WindowKind::BlackmanHarris7: return "blackman-harris-7";
    case WindowKind::BlackmanHarris4: return "blackman-harris-4";
    case WindowKind::Hann:            return "hann";
    case WindowKind::FlatTop:         return "flat-top";
    }
    return "blackman-harris-7";
}

WindowKind windowKindFromKey(const std::string& key)
{
    if (key == "blackman-harris-4") { return WindowKind::BlackmanHarris4; }
    if (key == "hann")              { return WindowKind::Hann; }
    if (key == "flat-top")          { return WindowKind::FlatTop; }
    return WindowKind::BlackmanHarris7;
}

std::string windowKindLabel(WindowKind kind)
{
    switch (kind) {
    case WindowKind::BlackmanHarris7: return "Blackman-Harris 7";
    case WindowKind::BlackmanHarris4: return "Blackman-Harris 4";
    case WindowKind::Hann:            return "Hann";
    case WindowKind::FlatTop:         return "Flat Top";
    }
    return "Blackman-Harris 7";
}

}  // namespace AetherSDR::txlin
