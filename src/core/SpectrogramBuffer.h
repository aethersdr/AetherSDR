#pragma once

#include <QVector>
#include <array>

namespace MasterSDR {

// Circular ring buffer of the last N FFT frames for one panadapter.
// Used by the CNN signal classifier to extract per-signal spectrogram patches.
// PanadapterStream::spectrumReady is emitted on the network thread and delivered
// to MainWindow via Qt::AutoConnection (queued), so push() and extractPatch()
// run on the GUI thread — no locking needed.
class SpectrogramBuffer {
public:
    static constexpr int kMaxFrames    = 32;  // time axis of the patch
    static constexpr int kPatchFreqBins = 64; // freq axis after resampling

    // Append one FFT frame.  Older frames roll off automatically.
    void push(const QVector<float>& binsDbm,
              double centerMhz, double bandwidthMhz);

    // Extract a normalized [0, 1] float patch of shape [kMaxFrames × kPatchFreqBins]
    // centred on sigFreqMhz and spanning freqWidthMhz (padded to 2× signal width).
    // Rows are ordered oldest → newest.
    // Returns an empty vector when fewer than kMaxFrames frames have been pushed.
    QVector<float> extractPatch(double sigFreqMhz, double freqWidthMhz) const;

    int frameCount() const { return m_count; }

private:
    struct Frame {
        QVector<float> bins;
        double centerMhz{0};
        double bandwidthMhz{0};
    };

    // Ring buffer: m_head is the index of the *next* slot to write.
    std::array<Frame, kMaxFrames> m_frames{};
    int m_head{0};
    int m_count{0};
};

} // namespace MasterSDR
