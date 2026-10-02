#pragma once

// Mini-pan re-slice: cut a narrow window around the followed VFO's passband
// from one main-pan FFT frame. No pan or slice of its own, so resolution is the
// main pan's bin width (200 kHz over ~2800 px ≈ 70 Hz/bin → ~140 bins in ±5 kHz;
// at 2 MHz only ~14). Free functions over plain values for unit tests.

#include <QVector>

#include <algorithm>

namespace AetherSDR::MiniPan {

// `bins` spans [panLoMhz, panLoMhz + panBwMhz], first bin at the low edge.
// Returns `outCount` samples over [wantLoMhz, wantLoMhz + wantBwMhz], linearly
// interpolated. Samples outside the source pan are `floorDbm`, not the edge
// bin, so no signal is smeared onto a frequency it is not on.
inline QVector<float> resliceWindow(const QVector<float>& bins,
                                    double panLoMhz, double panBwMhz,
                                    double wantLoMhz, double wantBwMhz,
                                    int outCount, float floorDbm)
{
    if (bins.size() < 2 || panBwMhz <= 0.0 || wantBwMhz <= 0.0 || outCount < 2)
        return {};

    const int n = bins.size();
    QVector<float> out(outCount);
    for (int i = 0; i < outCount; ++i) {
        const double mhz = wantLoMhz + (wantBwMhz * i) / (outCount - 1);
        const double pos = (mhz - panLoMhz) / panBwMhz * (n - 1);
        if (pos < 0.0 || pos > n - 1) {
            out[i] = floorDbm;
            continue;
        }
        const int    i0 = static_cast<int>(pos);
        const int    i1 = std::min(i0 + 1, n - 1);
        const double f  = pos - i0;
        out[i] = static_cast<float>(bins[i0] * (1.0 - f) + bins[i1] * f);
    }
    return out;
}

// Output width for the re-sliced trace. Enough to render smoothly at any applet
// width; a wide main pan simply repeats its few in-range bins across it.
inline constexpr int kResliceOutputBins = 512;

// Passband centre offset from the carrier, in Hz, from the filter edges (USB
// 100..2800 → +1450); 0 for symmetric modes and when hi <= lo (no slice or
// edges unknown). The mini-pan centres here, not on the carrier. Shared by
// MainWindow (window cut) and MiniPanScope (hairline, passband wash) so they
// agree.
inline double passbandCenterOffsetHz(int lowHz, int highHz)
{
    return highHz > lowHz ? (lowHz + highHz) / 2.0 : 0.0;
}

} // namespace AetherSDR::MiniPan
