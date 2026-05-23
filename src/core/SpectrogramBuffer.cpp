#include "SpectrogramBuffer.h"

#include <algorithm>
#include <cmath>

namespace MasterSDR {

void SpectrogramBuffer::push(const QVector<float>& binsDbm,
                              double centerMhz, double bandwidthMhz)
{
    if (binsDbm.isEmpty() || bandwidthMhz <= 0.0) { return; }
    Frame& f    = m_frames[m_head];
    f.bins        = binsDbm;
    f.centerMhz   = centerMhz;
    f.bandwidthMhz = bandwidthMhz;
    m_head = (m_head + 1) % kMaxFrames;
    if (m_count < kMaxFrames) { ++m_count; }
}

QVector<float> SpectrogramBuffer::extractPatch(double sigFreqMhz,
                                                double freqWidthMhz) const
{
    if (m_count < kMaxFrames) { return {}; }

    // Use 2× signal width as the extraction window so the patch captures
    // enough spectral context for the CNN to detect boundary shape.
    const double halfWin = std::max(freqWidthMhz, 0.003) * 1.0;  // ± 1× width

    QVector<float> patch;
    patch.resize(kMaxFrames * kPatchFreqBins, 0.0f);

    // Iterate frames oldest → newest
    for (int fi = 0; fi < kMaxFrames; ++fi) {
        const int idx = (m_head + fi) % kMaxFrames;  // m_head is next-write, so oldest is m_head
        const Frame& fr = m_frames[idx];
        if (fr.bins.isEmpty() || fr.bandwidthMhz <= 0.0) { continue; }

        const int   N        = fr.bins.size();
        const double startMhz = fr.centerMhz - fr.bandwidthMhz / 2.0;
        const double hzPerBin = fr.bandwidthMhz * 1.0e6 / N;

        // Resample N input bins into kPatchFreqBins output bins
        // covering [sigFreqMhz - halfWin, sigFreqMhz + halfWin].
        const double winStartMhz = sigFreqMhz - halfWin;
        const double winEndMhz   = sigFreqMhz + halfWin;

        for (int pb = 0; pb < kPatchFreqBins; ++pb) {
            const double fMhz = winStartMhz +
                (pb + 0.5) / kPatchFreqBins * (winEndMhz - winStartMhz);
            const double srcBinF = (fMhz - startMhz) / fr.bandwidthMhz * N - 0.5;
            const int    b0      = static_cast<int>(std::floor(srcBinF));
            const int    b1      = b0 + 1;
            const float  t       = static_cast<float>(srcBinF - b0);
            float val = 0.0f;
            if (b0 >= 0 && b1 < N) {
                val = fr.bins[b0] * (1.0f - t) + fr.bins[b1] * t;
            } else if (b0 >= 0 && b0 < N) {
                val = fr.bins[b0];
            } else if (b1 >= 0 && b1 < N) {
                val = fr.bins[b1];
            }
            patch[fi * kPatchFreqBins + pb] = val;
        }
    }

    // Normalize to [0, 1] using the patch min/max
    float minV = patch[0], maxV = patch[0];
    for (float v : patch) {
        minV = std::min(minV, v);
        maxV = std::max(maxV, v);
    }
    const float range = maxV - minV;
    if (range > 0.5f) {
        for (float& v : patch) { v = (v - minV) / range; }
    } else {
        // Flat patch — no signal content; zero it so CNN returns ~0.5
        patch.fill(0.0f);
    }

    return patch;
}

} // namespace MasterSDR
