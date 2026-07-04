#pragma once

#include <QElapsedTimer>
#include <QVector>

namespace AetherSDR {

// Incremental reduction core for the WAVE waveform scope.
//
// The scope used to rescan every raw sample in the visible window on every
// repaint — three O(window) passes (copy, stats, per-pixel columns) per
// frame, which at long windows and high refresh rates dominated the main
// thread (#3283). This model folds samples into fixed-duration bins once,
// as they arrive (O(samples/s) regardless of window length), and each
// repaint only merges bins into pixel columns (O(bins + width)).
//
// The raw ring is kept alongside the bins: re-binning on window/rate
// changes, pause snapshots, and the Bands-mode Goertzel analyzer all read
// it. The model is a value type — pausing the scope is a plain copy.
//
// Phase-2 GPU hook: mergeColumns() fills a contiguous POD ColumnStats
// array a QRhi renderer can upload directly as a 1-D texture, and
// generation() gives it a dirty counter so unchanged frames skip the
// upload.
class WaveformScopeModel {
public:
    // Per-pixel-column reduction — the shape the render paths consume.
    struct ColumnStats {
        float min{0.0f};
        float max{0.0f};
        float peak{0.0f};
        float rms{0.0f};
        int clipped{0};
    };

    // Whole-window reduction for the header readout, computed from the
    // same bins during mergeColumns() so no extra pass is needed.
    struct WindowStats {
        float peak{0.0f};
        float rms{0.0f};
        int clipCount{0};
        bool empty{true};
    };

    // The largest window this model will ever be asked to display. The raw
    // ring is sized to hold it (not merely the current window), so widening
    // the window — live or on a paused snapshot — reveals already-captured
    // history instead of a blank plot that refills over the next N seconds
    // (the invariant the old StripWaveform::ensureCapacity enforced). Set
    // once from the widget's per-profile ceiling; 0 falls back to
    // current-window sizing.
    void setMaxWindowMs(int maxWindowMs);

    // Applies a new sample rate / window; re-bins from the raw ring when
    // either changed. Cheap no-op when both are unchanged.
    void configure(int sampleRate, int windowMs);

    // Folds new samples into the raw ring and the current bin. Samples
    // are clamped to [-1, 1] (non-finite values become 0) on the way in.
    void append(const float* samples, int count);

    void clear();

    // Merges the window's bins into columnCount columns. Mirrors the old
    // buildColumns() index mapping: available data is stretched across the
    // full column range, so a part-filled ring still spans the plot.
    WindowStats mergeColumns(int columnCount, QVector<ColumnStats>& out) const;

    // Whole-window stats without a column merge (Bands mode header).
    WindowStats windowStats() const;

    // Most recent `count` raw samples, oldest first (Bands analyzer).
    void copyTail(int count, QVector<float>& out) const;

    int sampleRate() const { return m_sampleRate; }
    int windowMs() const { return m_windowMs; }
    bool hasSamples() const { return m_rawFilled > 0; }

    // ms since the last append, or -1 if nothing was ever appended.
    // Replaces the old RingBuffer::lastSamples staleness timer.
    qint64 msSinceLastAppend() const;

    // Bumps on every append/clear/configure; a renderer that cached the
    // last merge (or a future GPU upload) can skip work when unchanged.
    quint64 generation() const { return m_generation; }

private:
    struct Bin {
        float min{0.0f};
        float max{0.0f};
        float peak{0.0f};
        double sumSq{0.0};
        int count{0};
        int clipped{0};
    };

    void ensureRawCapacity();
    void rebinFromRaw();
    void resetCurrentBin();
    void commitCurrentBin();
    inline void foldSampleIntoCurrentBin(float clamped);
    int windowSampleCount() const;
    // Bins covering the window right now, including the part-filled
    // current bin; capped at what has actually been recorded.
    int usedBinCount() const;
    // Reads bin `i` of the last `used` bins (oldest first, i in [0, used)).
    const Bin& binAt(int used, int i) const;

    // Raw sample ring — sized to the window plus 1 s headroom, in 4096-
    // sample chunks so slider drags don't realloc per notch (same policy
    // the widget's old ensureCapacity used).
    QVector<float> m_raw;
    int m_rawWrite{0};
    int m_rawFilled{0};

    // Bin ring. kTargetBins per window keeps the merge finer than any
    // plausible plot width while staying O(width)-ish per frame.
    QVector<Bin> m_bins;
    int m_binWrite{0};
    int m_binsFilled{0};
    Bin m_cur;
    int m_curFill{0};        // samples folded into m_cur so far
    int m_samplesPerBin{1};

    int m_sampleRate{24000};
    int m_windowMs{100};
    int m_maxWindowMs{0};   // raw-ring sizing ceiling; 0 = size to current window
    quint64 m_generation{0};
    QElapsedTimer m_lastAppend;
};

} // namespace AetherSDR
