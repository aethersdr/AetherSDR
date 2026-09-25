#pragma once

#include "core/PcmFrame.h"

#include <QVector>

#include <array>
#include <cmath>
#include <numbers>

namespace CwPcmFixture {
// Test-only 20 WPM SOS: 60 ms dits, 600 Hz carrier, 300 ms lead/tail silence.
// Stereo contains an opposite-polarity interferer so both channels matter.
inline QVector<float> sos(const AetherSDR::PcmFormat& format)
{
    constexpr std::array<std::array<int, 2>, 9> kMarks{{
        {5, 6}, {7, 8}, {9, 10}, {13, 16}, {17, 20},
        {21, 24}, {27, 28}, {29, 30}, {31, 32}}};
    const int ditFrames = format.sampleRateHz * 3 / 50;
    QVector<float> samples(37 * ditFrames * format.channels(), 0.0f);
    for (const auto& mark : kMarks) {
        for (int frame = mark[0] * ditFrames; frame < mark[1] * ditFrames; ++frame) {
            const double time = double(frame) / format.sampleRateHz;
            const float signal = .4f * std::sin(2 * std::numbers::pi * 600 * time);
            if (format.channels() == 1) {
                samples[frame] = signal;
            } else {
                const float interferer = .1f * std::sin(2 * std::numbers::pi * 1100 * time);
                samples[2 * frame] = 1.5f * signal + interferer;
                samples[2 * frame + 1] = .5f * signal - interferer;
            }
        }
    }
    return samples;
}
} // namespace CwPcmFixture
