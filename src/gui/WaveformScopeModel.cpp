#include "WaveformScopeModel.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

// Bins per window. Finer than any plausible plot width (the widest WAVE
// surface is a full-screen strip panel ≈ 1600–3800 px of *plot*, but each
// pixel column merges ≥ 1 bin, and 2048 keeps the merge visually
// indistinguishable from per-sample columns while bounding the per-frame
// walk).
constexpr int kTargetBins = 2048;
// Extra bins beyond one window so the ring never wraps mid-merge.
constexpr int kBinHeadroom = 64;
constexpr float kClipThreshold = 0.98f;

inline float clampSample(float sample)
{
    if (!std::isfinite(sample))
        return 0.0f;
    return std::clamp(sample, -1.0f, 1.0f);
}

} // namespace

void WaveformScopeModel::setMaxWindowMs(int maxWindowMs)
{
    const int clamped = std::max(0, maxWindowMs);
    if (clamped == m_maxWindowMs)
        return;
    m_maxWindowMs = clamped;
    // Grow the raw ring to the new ceiling now so history captured before the
    // first widen is retained rather than starting from the current window.
    ensureRawCapacity();
}

void WaveformScopeModel::configure(int sampleRate, int windowMs)
{
    if (sampleRate == m_sampleRate && windowMs == m_windowMs
        && !m_bins.isEmpty())
        return;

    m_sampleRate = sampleRate;
    m_windowMs = windowMs;
    ensureRawCapacity();
    rebinFromRaw();
    ++m_generation;
}

int WaveformScopeModel::windowSampleCount() const
{
    return std::max(1, static_cast<int>(
        static_cast<int64_t>(m_sampleRate) * m_windowMs / 1000));
}

void WaveformScopeModel::ensureRawCapacity()
{
    // Size to the MAXIMUM window (not the current one) plus 1 s headroom,
    // floored at 1 s, rounded to 4096-sample chunks so slider drags don't
    // realloc per notch. Sizing to the ceiling is what lets a widen reveal
    // already-captured history instead of a blank plot (StripWaveform's old
    // ensureCapacity invariant); m_maxWindowMs==0 falls back to the current
    // window.
    const int sizingWindowMs = std::max({1000, m_windowMs, m_maxWindowMs});
    const int windowSamples = std::max(1, static_cast<int>(
        static_cast<int64_t>(m_sampleRate) * sizingWindowMs / 1000));
    const int needed = std::max(m_sampleRate, windowSamples) + m_sampleRate;
    const int capacity = ((needed + 4095) / 4096) * 4096;
    if (m_raw.size() >= capacity)
        return;

    QVector<float> preserved;
    copyTail(std::min(m_rawFilled, capacity), preserved);

    m_raw = QVector<float>(capacity, 0.0f);
    m_rawWrite = 0;
    m_rawFilled = 0;
    for (float s : preserved) {
        m_raw[m_rawWrite] = s;
        m_rawWrite = (m_rawWrite + 1) % capacity;
        m_rawFilled = std::min(m_rawFilled + 1, capacity);
    }
}

void WaveformScopeModel::resetCurrentBin()
{
    m_cur = Bin{};
    m_cur.min = 1.0f;
    m_cur.max = -1.0f;
    m_curFill = 0;
}

void WaveformScopeModel::commitCurrentBin()
{
    if (m_bins.isEmpty())
        return;
    m_bins[m_binWrite] = m_cur;
    m_binWrite = (m_binWrite + 1) % m_bins.size();
    m_binsFilled = std::min(m_binsFilled + 1, static_cast<int>(m_bins.size()));
    resetCurrentBin();
}

inline void WaveformScopeModel::foldSampleIntoCurrentBin(float clamped)
{
    m_cur.min = std::min(m_cur.min, clamped);
    m_cur.max = std::max(m_cur.max, clamped);
    const float a = std::abs(clamped);
    m_cur.peak = std::max(m_cur.peak, a);
    m_cur.sumSq += static_cast<double>(clamped) * clamped;
    ++m_cur.count;
    if (a >= kClipThreshold)
        ++m_cur.clipped;
    ++m_curFill;
    if (m_curFill >= m_samplesPerBin)
        commitCurrentBin();
}

void WaveformScopeModel::rebinFromRaw()
{
    const int windowSamples = windowSampleCount();
    m_samplesPerBin = std::max(1, windowSamples / kTargetBins);
    const int binCapacity =
        (windowSamples + m_samplesPerBin - 1) / m_samplesPerBin + kBinHeadroom;
    m_bins = QVector<Bin>(binCapacity);
    m_binWrite = 0;
    m_binsFilled = 0;
    resetCurrentBin();

    // Refold the raw tail so a window/rate change keeps the trace instead
    // of blanking it. O(window), but only on user action, never per frame.
    const int tail = std::min(m_rawFilled, windowSamples);
    if (tail <= 0 || m_raw.isEmpty())
        return;
    const int capacity = m_raw.size();
    int start = m_rawWrite - tail;
    while (start < 0)
        start += capacity;
    for (int i = 0; i < tail; ++i)
        foldSampleIntoCurrentBin(m_raw[(start + i) % capacity]);
}

void WaveformScopeModel::append(const float* samples, int count)
{
    if (count <= 0)
        return;
    if (m_raw.isEmpty())
        ensureRawCapacity();
    if (m_bins.isEmpty())
        rebinFromRaw();

    const int capacity = m_raw.size();
    for (int i = 0; i < count; ++i) {
        const float s = clampSample(samples[i]);
        m_raw[m_rawWrite] = s;
        m_rawWrite = (m_rawWrite + 1) % capacity;
        m_rawFilled = std::min(m_rawFilled + 1, capacity);
        foldSampleIntoCurrentBin(s);
    }
    m_lastAppend.restart();
    ++m_generation;
}

void WaveformScopeModel::clear()
{
    m_raw.fill(0.0f);
    m_rawWrite = 0;
    m_rawFilled = 0;
    m_binsFilled = 0;
    m_binWrite = 0;
    resetCurrentBin();
    m_lastAppend.invalidate();
    ++m_generation;
}

int WaveformScopeModel::usedBinCount() const
{
    const int windowSamples = windowSampleCount();
    int windowBins = (windowSamples + m_samplesPerBin - 1) / m_samplesPerBin;
    int available = m_binsFilled + (m_curFill > 0 ? 1 : 0);
    return std::min(windowBins, available);
}

const WaveformScopeModel::Bin& WaveformScopeModel::binAt(int used, int i) const
{
    // Bin i of the last `used` bins, oldest first. The part-filled current
    // bin is the newest (i == used-1) when it holds samples.
    const bool curIsNewest = (m_curFill > 0);
    if (curIsNewest && i == used - 1)
        return m_cur;
    const int committedUsed = used - (curIsNewest ? 1 : 0);
    const int cap = m_bins.size();
    int idx = m_binWrite - committedUsed + i;
    while (idx < 0)
        idx += cap;
    return m_bins[idx % cap];
}

WaveformScopeModel::WindowStats
WaveformScopeModel::mergeColumns(int columnCount, QVector<ColumnStats>& out) const
{
    out.clear();
    WindowStats win;
    const int used = usedBinCount();
    if (columnCount <= 0 || used <= 0 || m_bins.isEmpty())
        return win;

    out.resize(columnCount);
    win.empty = false;
    double winSumSq = 0.0;
    qint64 winCount = 0;

    for (int x = 0; x < columnCount; ++x) {
        // Same stretch mapping the old buildColumns() used over samples:
        // available bins span the full column range.
        int start = (x * used) / columnCount;
        int end = ((x + 1) * used) / columnCount;
        if (end <= start)
            end = start + 1;
        start = std::clamp(start, 0, used - 1);
        end = std::clamp(end, start + 1, used);

        float mn = 1.0f;
        float mx = -1.0f;
        float peak = 0.0f;
        double sumSq = 0.0;
        int count = 0;
        int clipped = 0;
        for (int i = start; i < end; ++i) {
            const Bin& b = binAt(used, i);
            if (b.count <= 0)
                continue;
            mn = std::min(mn, b.min);
            mx = std::max(mx, b.max);
            peak = std::max(peak, b.peak);
            sumSq += b.sumSq;
            count += b.count;
            clipped += b.clipped;
        }

        ColumnStats& c = out[x];
        if (count > 0) {
            c.min = mn;
            c.max = mx;
            c.peak = peak;
            c.rms = static_cast<float>(std::sqrt(sumSq / count));
            c.clipped = clipped;
        } else {
            // Empty column: match the old buildColumns() no-data sentinel
            // (min > max) so the render draws nothing here, not a zero line.
            c.min = 1.0f;
            c.max = -1.0f;
        }

        // Column ranges tile the bin range exactly once when used >=
        // columnCount; when stretched (used < columnCount) several columns
        // share a bin, so accumulate window totals from bins directly
        // below instead of from columns.
    }

    for (int i = 0; i < used; ++i) {
        const Bin& b = binAt(used, i);
        if (b.count <= 0)
            continue;
        win.peak = std::max(win.peak, b.peak);
        winSumSq += b.sumSq;
        winCount += b.count;
        win.clipCount += b.clipped;
    }
    if (winCount > 0)
        win.rms = static_cast<float>(std::sqrt(winSumSq / winCount));
    else
        win.empty = true;
    return win;
}

WaveformScopeModel::WindowStats WaveformScopeModel::windowStats() const
{
    WindowStats win;
    const int used = usedBinCount();
    if (used <= 0 || m_bins.isEmpty())
        return win;
    win.empty = false;
    double sumSq = 0.0;
    qint64 count = 0;
    for (int i = 0; i < used; ++i) {
        const Bin& b = binAt(used, i);
        if (b.count <= 0)
            continue;
        win.peak = std::max(win.peak, b.peak);
        sumSq += b.sumSq;
        count += b.count;
        win.clipCount += b.clipped;
    }
    if (count > 0)
        win.rms = static_cast<float>(std::sqrt(sumSq / count));
    else
        win.empty = true;
    return win;
}

void WaveformScopeModel::copyTail(int count, QVector<float>& out) const
{
    out.clear();
    if (count <= 0 || m_rawFilled <= 0 || m_raw.isEmpty())
        return;

    count = std::min(count, m_rawFilled);
    out.resize(count);

    const int capacity = m_raw.size();
    int start = m_rawWrite - count;
    while (start < 0)
        start += capacity;
    for (int i = 0; i < count; ++i)
        out[i] = m_raw[(start + i) % capacity];
}

qint64 WaveformScopeModel::msSinceLastAppend() const
{
    return m_lastAppend.isValid() ? m_lastAppend.elapsed() : -1;
}

} // namespace AetherSDR
