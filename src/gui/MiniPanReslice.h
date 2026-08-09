#pragma once

// Mini-pan re-slice: take one main-pan FFT frame and cut a narrow window out of
// it around the followed VFO.
//
// This is the whole mini-pan data path. There is no dedicated radio pan and no
// slice — the applet is a VIEW of bins the main pan is already streaming, so
// opening it costs the radio nothing (no pan slot, nothing to leak on quit, no
// phantom slice). Resolution is therefore the MAIN pan's bin width: at a 200 kHz
// pan across ~2800 px that is ~70 Hz/bin, so a ±5 kHz window carries ~140 real
// bins; zoom the main pan out to 2 MHz and the same window carries ~14 and the
// trace goes visibly coarse. That trade is the point of the design.
//
// Kept as a free function over plain values so the mapping can be unit-tested
// without a MainWindow, a radio, or a widget.

#include <QVector>

#include <algorithm>

namespace AetherSDR::MiniPan {

// `bins` spans [panLoMhz, panLoMhz + panBwMhz] linearly, bins.first() at the low
// edge. Returns `outCount` samples spanning [wantLoMhz, wantLoMhz + wantBwMhz].
//
// Samples that fall outside the source pan return `floorDbm` rather than the
// nearest edge bin: padding keeps the output's frequency axis honest, where
// clamping would smear the edge value across the gap and put a signal at a
// frequency it is not on.
//
// Interpolates linearly between adjacent source bins — when the main pan is
// narrow the window is upsampled, and nearest-neighbour stair-steps visibly on
// a filled trace.
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

} // namespace AetherSDR::MiniPan
