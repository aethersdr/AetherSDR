#include "DssRenderer.h"

#include <QPainter>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

// CPU-only tunables. The perspective geometry (back-width / depth-span /
// front-ridge / haze) lives in DssRenderer.h as shared constants so the GPU
// mesh uses the same values; these are extra CPU-render touches the GPU frag
// doesn't replicate (depth dimming floor, smoothing, slope shading).
constexpr double kMinDim        = 0.50;  // depth dimming never falls below this
constexpr float  kTemporalAlpha = 0.60f; // temporal IIR: fraction of the new row
constexpr double kSlopeGain     = 0.55;  // slope shading strength
constexpr double kShadeLo       = 0.68;
constexpr double kShadeHi       = 1.32;
using CoverageRow = std::array<quint8, DssRenderer::kCols>;

struct ReprojectedRow {
    std::array<float, DssRenderer::kCols> values;
    CoverageRow coverage;
};

inline int chan(double v) { return static_cast<int>(std::clamp(v, 0.0, 255.0)); }

inline float median3(float a, float b, float c)
{
    return std::max(std::min(a, b), std::min(std::max(a, b), c));
}

inline QColor scaled(const QColor& c, double f)
{
    f = std::max(0.0, f);
    return QColor(chan(c.red() * f), chan(c.green() * f), chan(c.blue() * f));
}

// Linear blend c -> t by f in [0,1].
inline QColor lerpColor(const QColor& c, const QColor& t, double f)
{
    f = std::clamp(f, 0.0, 1.0);
    return QColor(chan(c.red()   + (t.red()   - c.red())   * f),
                  chan(c.green() + (t.green() - c.green()) * f),
                  chan(c.blue()  + (t.blue()  - c.blue())  * f));
}

bool frequencyFramesMatch(double firstCenterMhz, double firstBandwidthMhz,
                          double secondCenterMhz, double secondBandwidthMhz)
{
    const auto nearlyEqual = [](double first, double second) {
        const double scale =
            std::max({1.0, std::abs(first), std::abs(second)});
        return std::abs(first - second) <= scale * 1.0e-9;
    };
    return nearlyEqual(firstCenterMhz, secondCenterMhz)
        && nearlyEqual(firstBandwidthMhz, secondBandwidthMhz);
}

float rowSample(const std::array<float, DssRenderer::kCols>& row,
                const CoverageRow* coverage,
                double srcLeft, double srcRight, double srcCenter,
                float fallback, bool& sampledCoverage)
{
    sampledCoverage = false;
    if (srcRight <= 0.0 || srcLeft >= DssRenderer::kCols) {
        return fallback;
    }

    const double clampedLeft =
        std::clamp(srcLeft, 0.0, static_cast<double>(DssRenderer::kCols));
    const double clampedRight =
        std::clamp(srcRight, 0.0, static_cast<double>(DssRenderer::kCols));
    if (clampedRight <= clampedLeft) {
        return fallback;
    }

    if (clampedRight - clampedLeft <= 1.0) {
        const double clampedCenter =
            std::clamp(srcCenter, 0.0, static_cast<double>(DssRenderer::kCols - 1));
        const int left = std::clamp(static_cast<int>(std::floor(clampedCenter)),
                                    0, DssRenderer::kCols - 1);
        const int right = std::min(left + 1, DssRenderer::kCols - 1);
        const float frac = static_cast<float>(clampedCenter - left);
        double weightedSum = 0.0;
        double totalWeight = 0.0;
        const auto add = [&](int index, double weight) {
            if (weight <= 0.0
                || (coverage != nullptr && (*coverage)[index] == 0)
                || !std::isfinite(row[index])) {
                return;
            }
            weightedSum += row[index] * weight;
            totalWeight += weight;
        };
        add(left, 1.0 - frac);
        add(right, frac);
        sampledCoverage = totalWeight > 0.0;
        return sampledCoverage
            ? static_cast<float>(weightedSum / totalWeight)
            : fallback;
    }

    const int first = std::clamp(static_cast<int>(std::floor(clampedLeft)),
                                 0, DssRenderer::kCols - 1);
    const int last = std::clamp(static_cast<int>(std::ceil(clampedRight)) - 1,
                                0, DssRenderer::kCols - 1);
    double weightedSum = 0.0;
    double totalWeight = 0.0;
    for (int i = first; i <= last; ++i) {
        if ((coverage != nullptr && (*coverage)[i] == 0)
            || !std::isfinite(row[i])) {
            continue;
        }
        const double binLeft = static_cast<double>(i);
        const double binRight = binLeft + 1.0;
        const double weight =
            std::max(0.0, std::min(clampedRight, binRight)
                - std::max(clampedLeft, binLeft));
        if (weight <= 0.0) {
            continue;
        }
        weightedSum += row[i] * weight;
        totalWeight += weight;
    }
    sampledCoverage = totalWeight > 0.0;
    return sampledCoverage
        ? static_cast<float>(weightedSum / totalWeight)
        : fallback;
}

ReprojectedRow reprojectRow(
    const std::array<float, DssRenderer::kCols>& row,
    const CoverageRow* sourceCoverage,
    double oldCenterMhz,
    double oldBandwidthMhz,
    double newCenterMhz,
    double newBandwidthMhz,
    float fallback)
{
    ReprojectedRow remapped;
    remapped.values.fill(fallback);
    remapped.coverage.fill(0);
    if (oldBandwidthMhz <= 0.0 || newBandwidthMhz <= 0.0) {
        return remapped;
    }

    const double oldStartMhz = oldCenterMhz - oldBandwidthMhz * 0.5;
    const double oldEndMhz = oldCenterMhz + oldBandwidthMhz * 0.5;
    const double newStartMhz = newCenterMhz - newBandwidthMhz * 0.5;
    const double srcWidthBins = newBandwidthMhz / oldBandwidthMhz;
    for (int dst = 0; dst < DssRenderer::kCols; ++dst) {
        const double dstFrac =
            (static_cast<double>(dst) + 0.5) / DssRenderer::kCols;
        const double freqMhz = newStartMhz + dstFrac * newBandwidthMhz;
        if (freqMhz < oldStartMhz || freqMhz > oldEndMhz) {
            continue;
        }

        const double srcCenter =
            ((freqMhz - oldStartMhz) / oldBandwidthMhz)
                * DssRenderer::kCols - 0.5;
        if (srcCenter < -0.5
            || srcCenter > static_cast<double>(DssRenderer::kCols) - 0.5) {
            continue;
        }
        bool sampledCoverage = false;
        remapped.values[dst] = rowSample(
            row,
            sourceCoverage,
            srcCenter - srcWidthBins * 0.5,
            srcCenter + srcWidthBins * 0.5,
            srcCenter,
            fallback,
            sampledCoverage);
        remapped.coverage[dst] = sampledCoverage ? quint8(1) : quint8(0);
    }
    return remapped;
}

std::array<float, DssRenderer::kCols> resampledRawRow(
    const QVector<float>& binsDbm,
    float fallback)
{
    std::array<float, DssRenderer::kCols> row;
    const int n = binsDbm.size();
    if (n <= 0) {
        row.fill(fallback);
        return row;
    }

    if (n == DssRenderer::kCols) {
        for (int c = 0; c < DssRenderer::kCols; ++c) {
            row[c] = std::isfinite(binsDbm[c]) ? binsDbm[c] : fallback;
        }
    } else {
        const double step = static_cast<double>(n) / DssRenderer::kCols;
        for (int c = 0; c < DssRenderer::kCols; ++c) {
            int i0 = static_cast<int>(std::floor(c * step));
            int i1 = static_cast<int>(std::ceil((c + 1) * step));
            i0 = std::clamp(i0, 0, n - 1);
            i1 = std::clamp(i1, i0 + 1, n);
            float mx = std::isfinite(binsDbm[i0]) ? binsDbm[i0] : fallback;
            for (int i = i0 + 1; i < i1; ++i) {
                if (std::isfinite(binsDbm[i])) {
                    mx = std::max(mx, binsDbm[i]);
                }
            }
            row[c] = mx;
        }
    }

    return row;
}

std::array<float, DssRenderer::kCols> smoothDssRow(
    const std::array<float, DssRenderer::kCols>& raw,
    std::array<float, DssRenderer::kCols>& rawPrev1,
    std::array<float, DssRenderer::kCols>& rawPrev2,
    int& rawHistCount,
    const std::array<float, DssRenderer::kCols>* previousSmoothed)
{
    std::array<float, DssRenderer::kCols> row = raw;
    if (rawHistCount >= 2) {
        for (int c = 0; c < DssRenderer::kCols; ++c) {
            row[c] = median3(raw[c], rawPrev1[c], rawPrev2[c]);
        }
    }

    rawPrev2 = rawPrev1;
    rawPrev1 = raw;
    rawHistCount = std::min(rawHistCount + 1, 2);

    std::array<float, DssRenderer::kCols> smoothed = row;
    for (int c = 0; c < DssRenderer::kCols; ++c) {
        const float a = row[std::max(0, c - 1)];
        const float b = row[c];
        const float d = row[std::min(DssRenderer::kCols - 1, c + 1)];
        smoothed[c] = 0.25f * a + 0.5f * b + 0.25f * d;
    }
    if (previousSmoothed != nullptr) {
        for (int c = 0; c < DssRenderer::kCols; ++c) {
            smoothed[c] = kTemporalAlpha * smoothed[c]
                + (1.0f - kTemporalAlpha) * (*previousSmoothed)[c];
        }
    }
    return smoothed;
}

} // namespace

void DssRenderer::resetInputSmoothing()
{
    m_rawHistCount = 0;
    m_supplementalRawHistCount = 0;
    // Also break the temporal IIR blend for the next row of each path. Zeroing
    // the median-of-3 counters alone does not forget the previous *smoothed*
    // row that pushRow/appendHistoryRow blend the new row against — that row was
    // decoded under the old scale, so without this the first post-reset row is
    // contaminated by it.
    m_skipLiveTemporalBlendOnce = true;
    m_skipSupplementalTemporalBlendOnce = true;
    m_skipHistoryTemporalBlendOnce = true;
    resetHistorySmoothing();
}

void DssRenderer::clear()
{
    m_head  = 0;
    m_count = 0;
    m_rowCenterMhz.fill(0.0);
    m_rowBandwidthMhz.fill(0.0);
    m_rowSupplementalCenterMhz.fill(0.0);
    m_rowSupplementalBandwidthMhz.fill(0.0);
    for (CoverageRow& coverage : m_rowSupplementalCoverage) {
        coverage.fill(0);
    }
    m_dirty = true;
    m_historyWriteRow = 0;
    m_historyRowCount = 0;
    resetInputSmoothing();
}

quint64 DssRenderer::fixedStorageBytes() const
{
    return sizeof(m_rows) + sizeof(m_rowCoverage)
        + sizeof(m_rowSupplemental) + sizeof(m_rowSupplementalCoverage)
        + sizeof(m_rowCenterMhz) + sizeof(m_rowBandwidthMhz)
        + sizeof(m_rowSupplementalCenterMhz)
        + sizeof(m_rowSupplementalBandwidthMhz)
        + sizeof(m_rawPrev1) + sizeof(m_rawPrev2)
        + sizeof(m_supplementalRawPrev1)
        + sizeof(m_supplementalRawPrev2)
        + sizeof(m_historyRawPrev1) + sizeof(m_historyRawPrev2);
}

quint64 DssRenderer::historyStorageBytes() const
{
    return static_cast<quint64>(m_historyRows.capacity()) * sizeof(qfloat16)
        + static_cast<quint64>(m_historyRowCenterMhz.capacity()) * sizeof(double)
        + static_cast<quint64>(m_historyRowBandwidthMhz.capacity()) * sizeof(double);
}

quint64 DssRenderer::cacheStorageBytes() const
{
    return m_cache.isNull() ? 0 : static_cast<quint64>(m_cache.sizeInBytes());
}

quint64 DssRenderer::allocatedBytes() const
{
    return fixedStorageBytes() + historyStorageBytes() + cacheStorageBytes();
}

const std::array<float, DssRenderer::kCols>&
DssRenderer::rowAt(int age) const
{
    const int idx = (m_head + age) % kRows;
    return m_rows[idx];
}

double DssRenderer::newestSupplementalBandwidthMhz(
    double targetBandwidthMhz) const
{
    if (!std::isfinite(targetBandwidthMhz) || targetBandwidthMhz <= 0.0) {
        return 0.0;
    }
    const int rows = visibleRowCount();
    for (int age = 0; age < rows; ++age) {
        const double candidate = rowSupplementalBandwidthMhzAtAge(age);
        if (std::isfinite(candidate) && candidate > targetBandwidthMhz) {
            return candidate;
        }
    }
    return 0.0;
}

DssRenderer::RowStats DssRenderer::rowStats(int age, float epsilonDb) const
{
    RowStats stats;
    if (age < 0 || age >= m_count) {
        return stats;
    }

    const auto& row = rowAt(age);
    stats.minDbm = std::numeric_limits<float>::infinity();
    stats.maxDbm = -std::numeric_limits<float>::infinity();
    for (const float value : row) {
        if (!std::isfinite(value)) {
            continue;
        }
        stats.minDbm = std::min(stats.minDbm, value);
        stats.maxDbm = std::max(stats.maxDbm, value);
        ++stats.finiteBins;
    }
    if (stats.finiteBins == 0) {
        stats.minDbm = 0.0f;
        stats.maxDbm = 0.0f;
        return stats;
    }

    epsilonDb = std::max(0.0f, epsilonDb);
    int currentRun = 0;
    float previous = 0.0f;
    bool havePrevious = false;
    for (const float value : row) {
        if (!std::isfinite(value)) {
            currentRun = 0;
            havePrevious = false;
            continue;
        }
        if (std::abs(value - stats.minDbm) <= epsilonDb) {
            ++stats.minValueBins;
        }
        if (havePrevious && std::abs(value - previous) <= epsilonDb) {
            ++currentRun;
        } else {
            currentRun = 1;
        }
        stats.longestFlatRunBins =
            std::max(stats.longestFlatRunBins, currentRun);
        previous = value;
        havePrevious = true;
    }
    return stats;
}

void DssRenderer::pushRow(const QVector<float>& binsDbm,
                          double centerMhz,
                          double bandwidthMhz)
{
    pushRowWithSupplemental(
        binsDbm, centerMhz, bandwidthMhz,
        QVector<float>{}, 0.0, 0.0);
}

void DssRenderer::pushRowWithSupplemental(
    const QVector<float>& binsDbm,
    double centerMhz,
    double bandwidthMhz,
    const QVector<float>& supplementalBinsDbm,
    double supplementalCenterMhz,
    double supplementalBandwidthMhz)
{
    const std::array<float, kCols> raw = resampledRawRow(binsDbm, -200.0f);
    const bool sameFrameAsPrevious =
        m_count > 0
        && frequencyFramesMatch(
            m_rowCenterMhz[m_head], m_rowBandwidthMhz[m_head],
            centerMhz, bandwidthMhz);
    if (m_count > 0 && !sameFrameAsPrevious) {
        // Adjacent bin indices no longer represent the same frequencies.
        // Blending them would smear a signal in the direction of the pan.
        m_rawHistCount = 0;
    }
    const std::array<float, kCols>* previous =
        (sameFrameAsPrevious && !m_skipLiveTemporalBlendOnce)
            ? &m_rows[m_head]
            : nullptr;
    m_skipLiveTemporalBlendOnce = false;
    const std::array<float, kCols> nr =
        smoothDssRow(raw, m_rawPrev1, m_rawPrev2, m_rawHistCount, previous);

    m_head = (m_head - 1 + kRows) % kRows;
    m_rows[m_head] = nr;
    m_rowCoverage[m_head].fill(1);
    m_rowCenterMhz[m_head] = centerMhz;
    m_rowBandwidthMhz[m_head] = bandwidthMhz;
    const bool supplementalValid =
        !supplementalBinsDbm.isEmpty()
        && std::isfinite(supplementalCenterMhz)
        && std::isfinite(supplementalBandwidthMhz)
        && supplementalCenterMhz > 0.0
        && supplementalBandwidthMhz > 0.0;
    if (supplementalValid) {
        const std::array<float, kCols> supplementalRaw =
            resampledRawRow(supplementalBinsDbm, -200.0f);
        const int previousRing = (m_head + 1) % kRows;
        const bool sameSupplementalFrameAsPrevious =
            m_count > 0
            && frequencyFramesMatch(
                m_rowSupplementalCenterMhz[previousRing],
                m_rowSupplementalBandwidthMhz[previousRing],
                supplementalCenterMhz, supplementalBandwidthMhz);
        if (m_count > 0 && !sameSupplementalFrameAsPrevious) {
            m_supplementalRawHistCount = 0;
        }
        const std::array<float, kCols>* supplementalPrevious =
            (sameSupplementalFrameAsPrevious
             && !m_skipSupplementalTemporalBlendOnce)
                ? &m_rowSupplemental[previousRing]
                : nullptr;
        m_skipSupplementalTemporalBlendOnce = false;
        m_rowSupplemental[m_head] = smoothDssRow(
            supplementalRaw,
            m_supplementalRawPrev1,
            m_supplementalRawPrev2,
            m_supplementalRawHistCount,
            supplementalPrevious);
        m_rowSupplementalCoverage[m_head].fill(1);
        m_rowSupplementalCenterMhz[m_head] = supplementalCenterMhz;
        m_rowSupplementalBandwidthMhz[m_head] =
            supplementalBandwidthMhz;
    } else {
        m_rowSupplemental[m_head].fill(-200.0f);
        m_rowSupplementalCoverage[m_head].fill(0);
        m_rowSupplementalCenterMhz[m_head] = 0.0;
        m_rowSupplementalBandwidthMhz[m_head] = 0.0;
        m_supplementalRawHistCount = 0;
    }
    m_count = std::min(m_count + 1, kRows);
    m_dirty = true;
    ++m_rowGeneration;
}

void DssRenderer::setHistoryCapacityRows(int rows)
{
    rows = std::max(0, rows);
    if (rows == m_historyCapacityRows && historyStorageMatchesCapacity()) {
        return;
    }

    m_historyCapacityRows = rows;
    const int expectedSampleCount = rows * kCols;
    m_historyRows = QVector<qfloat16>(expectedSampleCount);
    m_historyRowCenterMhz = QVector<double>(rows, 0.0);
    m_historyRowBandwidthMhz = QVector<double>(rows, 0.0);
    m_historyWriteRow = 0;
    m_historyRowCount = 0;
    resetHistorySmoothing();
}

bool DssRenderer::historyStorageMatchesCapacity() const
{
    const int expectedSampleCount = m_historyCapacityRows * kCols;
    return m_historyRows.size() == expectedSampleCount
        && m_historyRowCenterMhz.size() == m_historyCapacityRows
        && m_historyRowBandwidthMhz.size() == m_historyCapacityRows;
}

void DssRenderer::resetHistorySmoothing()
{
    m_historyRawPrev1 = {};
    m_historyRawPrev2 = {};
    m_historyRawHistCount = 0;
}

void DssRenderer::appendHistoryRow(const QVector<float>& binsDbm,
                                   double centerMhz, double bandwidthMhz,
                                   float fallbackDbm)
{
    if (m_historyCapacityRows <= 0) {
        return;
    }
    if (!historyStorageMatchesCapacity()) {
        setHistoryCapacityRows(m_historyCapacityRows);
    }

    const std::array<float, kCols> raw = resampledRawRow(binsDbm, fallbackDbm);
    std::array<float, kCols> previousRow;
    const std::array<float, kCols>* previous = nullptr;
    if (m_historyRowCount > 0 && !m_skipHistoryTemporalBlendOnce) {
        const double previousCenterMhz =
            m_historyRowCenterMhz.value(m_historyWriteRow, 0.0);
        const double previousBandwidthMhz =
            m_historyRowBandwidthMhz.value(m_historyWriteRow, 0.0);
        if (frequencyFramesMatch(
                previousCenterMhz, previousBandwidthMhz,
                centerMhz, bandwidthMhz)) {
            const qfloat16* src =
                m_historyRows.constData() + m_historyWriteRow * kCols;
            for (int c = 0; c < kCols; ++c) {
                previousRow[c] = static_cast<float>(src[c]);
            }
            previous = &previousRow;
        } else {
            // Adjacent bin indices represent different frequencies across a
            // zoom or band change. Blending them creates a synthetic low row at
            // the frame boundary that later appears to rise from the floor.
            resetHistorySmoothing();
        }
    }
    m_skipHistoryTemporalBlendOnce = false;
    const std::array<float, kCols> row =
        smoothDssRow(raw, m_historyRawPrev1, m_historyRawPrev2,
                     m_historyRawHistCount, previous);
    m_historyWriteRow =
        (m_historyWriteRow - 1 + m_historyCapacityRows) % m_historyCapacityRows;
    qfloat16* dst = m_historyRows.data() + m_historyWriteRow * kCols;
    for (int c = 0; c < kCols; ++c) {
        dst[c] = qfloat16(row[c]);
    }
    m_historyRowCenterMhz[m_historyWriteRow] = centerMhz;
    m_historyRowBandwidthMhz[m_historyWriteRow] = bandwidthMhz;
    m_historyRowCount = std::min(m_historyRowCount + 1, m_historyCapacityRows);
}

void DssRenderer::rebuildVisibleFromHistory(int offsetRows,
                                            double centerMhz,
                                            double bandwidthMhz,
                                            float fallbackDbm)
{
    if (m_historyCapacityRows <= 0
        || m_historyRowCount <= 0
        || !historyStorageMatchesCapacity()) {
        if (m_historyCapacityRows > 0 && !historyStorageMatchesCapacity()) {
            setHistoryCapacityRows(m_historyCapacityRows);
        }
        m_head = 0;
        m_count = 0;
        m_dirty = true;
        m_rawHistCount = 0;
        ++m_rowGeneration;
        return;
    }

    offsetRows = std::clamp(offsetRows, 0, m_historyRowCount - 1);
    const int rowsToCopy = std::min(kRows, m_historyRowCount - offsetRows);
    m_head = 0;
    m_count = rowsToCopy;

    for (int age = 0; age < rowsToCopy; ++age) {
        const int historyAge = offsetRows + age;
        const int historyRing =
            (m_historyWriteRow + historyAge) % m_historyCapacityRows;
        const qfloat16* src = m_historyRows.constData() + historyRing * kCols;
        std::array<float, kCols> row;
        CoverageRow coverage;
        coverage.fill(1);
        for (int c = 0; c < kCols; ++c) {
            row[c] = static_cast<float>(src[c]);
        }

        const double rowCenterMhz = m_historyRowCenterMhz.value(historyRing, 0.0);
        const double rowBandwidthMhz =
            m_historyRowBandwidthMhz.value(historyRing, 0.0);
        if (rowCenterMhz > 0.0 && rowBandwidthMhz > 0.0
            && centerMhz > 0.0 && bandwidthMhz > 0.0
            && (rowCenterMhz != centerMhz || rowBandwidthMhz != bandwidthMhz)) {
            const ReprojectedRow remapped = reprojectRow(
                row,
                nullptr,
                rowCenterMhz, rowBandwidthMhz,
                centerMhz, bandwidthMhz,
                fallbackDbm);
            row = remapped.values;
            coverage = remapped.coverage;
        }

        m_rows[age] = row;
        m_rowCoverage[age] = coverage;
        m_rowCenterMhz[age] = centerMhz;
        m_rowBandwidthMhz[age] = bandwidthMhz;
        m_rowSupplemental[age].fill(-200.0f);
        m_rowSupplementalCoverage[age].fill(0);
        m_rowSupplementalCenterMhz[age] = 0.0;
        m_rowSupplementalBandwidthMhz[age] = 0.0;
    }

    m_dirty = true;
    m_rawHistCount = 0;
    ++m_rowGeneration;
}

void DssRenderer::reprojectFrequencyFrame(double oldCenterMhz,
                                          double oldBandwidthMhz,
                                          double newCenterMhz,
                                          double newBandwidthMhz,
                                          float fallbackDbm,
                                          bool refreshFromRetainedHistory)
{
    // The compact visible ring is already resampled to kCols. Reprojecting it
    // repeatedly across large zoom changes compounds that loss: zooming far
    // out can collapse a signal into one column, and zooming back in expands
    // that column into a false ridge while uncovered bins become flat floor
    // segments. Refresh each existing ring slot from its frame-stamped retained
    // source instead. This preserves the head, row count, and scroll phase, so
    // the timeline remains continuously populated throughout the zoom.
    if (refreshFromRetainedHistory
        && m_count > 0 && m_historyRowCount >= m_count
        && historyStorageMatchesCapacity()) {
        for (int age = 0; age < m_count; ++age) {
            const int historyRing =
                (m_historyWriteRow + age) % m_historyCapacityRows;
            const qfloat16* src =
                m_historyRows.constData() + historyRing * kCols;
            std::array<float, kCols> row;
            CoverageRow coverage;
            coverage.fill(1);
            for (int c = 0; c < kCols; ++c) {
                row[c] = static_cast<float>(src[c]);
            }

            const double rowCenterMhz =
                m_historyRowCenterMhz.value(historyRing, 0.0);
            const double rowBandwidthMhz =
                m_historyRowBandwidthMhz.value(historyRing, 0.0);
            if (rowCenterMhz > 0.0 && rowBandwidthMhz > 0.0
                && newCenterMhz > 0.0 && newBandwidthMhz > 0.0
                && (rowCenterMhz != newCenterMhz
                    || rowBandwidthMhz != newBandwidthMhz)) {
                const ReprojectedRow remapped = reprojectRow(
                    row,
                    nullptr,
                    rowCenterMhz, rowBandwidthMhz,
                    newCenterMhz, newBandwidthMhz,
                    fallbackDbm);
                row = remapped.values;
                coverage = remapped.coverage;
            }

            const int visibleRing = (m_head + age) % kRows;
            m_rows[visibleRing] = row;
            m_rowCoverage[visibleRing] = coverage;
            m_rowCenterMhz[visibleRing] = newCenterMhz;
            m_rowBandwidthMhz[visibleRing] = newBandwidthMhz;
            m_rowSupplemental[visibleRing].fill(-200.0f);
            m_rowSupplementalCoverage[visibleRing].fill(0);
            m_rowSupplementalCenterMhz[visibleRing] = 0.0;
            m_rowSupplementalBandwidthMhz[visibleRing] = 0.0;
        }
        m_dirty = true;
        // The previous live row was just replaced from retained history.
        // Its bin coordinates are not valid temporal-filter inputs for the
        // first newly decoded target-frame row.
        resetInputSmoothing();
        ++m_rowGeneration;
        return;
    }

    if (m_count <= 0 || oldBandwidthMhz <= 0.0 || newBandwidthMhz <= 0.0) {
        return;
    }

    for (int age = 0; age < m_count; ++age) {
        const int ring = (m_head + age) % kRows;
        const ReprojectedRow remapped = reprojectRow(
            m_rows[ring],
            &m_rowCoverage[ring],
            oldCenterMhz, oldBandwidthMhz,
            newCenterMhz, newBandwidthMhz,
            fallbackDbm);
        m_rows[ring] = remapped.values;
        m_rowCoverage[ring] = remapped.coverage;
        m_rowCenterMhz[ring] = newCenterMhz;
        m_rowBandwidthMhz[ring] = newBandwidthMhz;
        m_rowSupplemental[ring].fill(-200.0f);
        m_rowSupplementalCoverage[ring].fill(0);
        m_rowSupplementalCenterMhz[ring] = 0.0;
        m_rowSupplementalBandwidthMhz[ring] = 0.0;
    }
    m_dirty = true;
    // Even when retained history is unavailable, the first target-frame row
    // must stand on its own. Carrying old-frame filter inputs forward would
    // blend fallback floor into newly covered frequencies.
    resetInputSmoothing();
    ++m_rowGeneration;
}

double DssRenderer::rowCenterMhzAtAge(int age) const
{
    if (age < 0 || age >= m_count) {
        return 0.0;
    }
    return m_rowCenterMhz[(m_head + age) % kRows];
}

double DssRenderer::rowBandwidthMhzAtAge(int age) const
{
    if (age < 0 || age >= m_count) {
        return 0.0;
    }
    return m_rowBandwidthMhz[(m_head + age) % kRows];
}

double DssRenderer::rowSupplementalCenterMhzAtAge(int age) const
{
    if (age < 0 || age >= m_count) {
        return 0.0;
    }
    return m_rowSupplementalCenterMhz[(m_head + age) % kRows];
}

double DssRenderer::rowSupplementalBandwidthMhzAtAge(int age) const
{
    if (age < 0 || age >= m_count) {
        return 0.0;
    }
    return m_rowSupplementalBandwidthMhz[(m_head + age) % kRows];
}

const QImage& DssRenderer::image(const QSize& px, int scaleStripPx,
                                 float floorDbm, float rangeDb, float zCurve,
                                 const PaletteFn& palette,
                                 quint64 paletteToken,
                                 const QColor& bgFill)
{
    const bool changed = m_dirty
        || px != m_cacheSize
        || scaleStripPx != m_cacheScaleStrip
        || floorDbm != m_cacheFloor
        || rangeDb != m_cacheRange
        || zCurve != m_cacheZCurve
        || paletteToken != m_cachePaletteToken;

    if (changed) {
        rebuild(px, scaleStripPx, floorDbm, rangeDb, zCurve, palette, bgFill);
        ++m_generation;
        m_cacheSize         = px;
        m_cacheScaleStrip   = scaleStripPx;
        m_cacheFloor        = floorDbm;
        m_cacheRange        = rangeDb;
        m_cacheZCurve       = zCurve;
        m_cachePaletteToken = paletteToken;
        m_dirty             = false;
    }
    return m_cache;
}

void DssRenderer::rebuild(const QSize& px, int scaleStripPx, float floorDbm,
                          float rangeDb, float zCurve, const PaletteFn& palette,
                          const QColor& bgFill)
{
    const int W = px.width();
    const int Htot = px.height();
    if (W <= 0 || Htot <= 0) {
        m_cache = QImage();
        return;
    }

    if (m_cache.size() != px || m_cache.format() != QImage::Format_RGBA8888_Premultiplied) {
        m_cache = QImage(px, QImage::Format_RGBA8888_Premultiplied);
    }
    m_cache.fill(Qt::transparent);

    // Plot region is everything above the (transparent) scale strip.
    const double H = std::max(1, Htot - std::max(0, scaleStripPx));

    QPainter p(&m_cache);
    p.fillRect(QRectF(0, 0, W, H), bgFill);

    if (m_count <= 0 || !palette || rangeDb <= 0.0f) {
        return;
    }

    const double zc            = std::max(0.05, static_cast<double>(zCurve));
    const double bottomY       = H;                       // plot floor
    const double depthSpan     = H * kDepthSpanFrac;
    const double frontMaxRidge = H * kFrontMaxRidgeFrac;
    // Match dss_mesh.vert's depth parametrization exactly (v = rr / rows), so
    // the CPU fallback and the GPU mesh place rows at the same depth.
    const double denom         = kVisibleRows;

    std::array<QPointF, kCols> pts;
    std::array<QColor, kCols>  cols;   // depth/slope-shaded fill colour per column

    QPolygonF poly;                    // reused (clear keeps capacity → no realloc)
    poly.reserve(4);
    QPen ridgePen;
    ridgePen.setCosmetic(true);
    ridgePen.setCapStyle(Qt::RoundCap);
    ridgePen.setJoinStyle(Qt::RoundJoin);

    // Back (oldest) → front (newest): painter's algorithm. Nearer traces are
    // wider, sit lower, and fill to the floor, so they occlude farther ones.
    for (int age = visibleRowCount() - 1; age >= 0; --age) {
        const double depthFrac    = age / denom;
        const double rowWidthFrac = 1.0 - depthFrac * (1.0 - kBackWidthFrac);
        const double inset        = W * (1.0 - rowWidthFrac) * 0.5;
        const double rowW         = W - 2.0 * inset;
        const double baselineY    = bottomY - depthFrac * depthSpan;
        const double maxRidge     = frontMaxRidge * rowWidthFrac;
        const double dim          = kMinDim + (1.0 - kMinDim) * (1.0 - depthFrac);

        const auto& row = rowAt(age);
        const int ring = (m_head + age) % kRows;
        const auto& coverage = m_rowCoverage[ring];
        // Pass 1: geometry — noise-floor-anchored ridge heights, with the same
        // pow(s, zCurve) floor-lift the GPU shader applies.
        for (int c = 0; c < kCols; ++c) {
            const double x = inset + (kCols > 1 ? double(c) / (kCols - 1) : 0.0) * rowW;
            const float dbm = coverage[c] != 0 ? row[c] : floorDbm;
            double strength = std::clamp(
                (dbm - floorDbm) / rangeDb, 0.0f, 1.0f);
            strength = std::pow(strength, zc);
            pts[c] = QPointF(x, baselineY - strength * maxRidge);
        }
        // Pass 2: colour — palette by amplitude, hazed by depth, lit by slope.
        const double slopeScale = (maxRidge > 1.0) ? maxRidge : 1.0;
        for (int c = 0; c < kCols; ++c) {
            const int cl = std::max(0, c - 1);
            const int cr = std::min(kCols - 1, c + 1);
            const double slope = (pts[cl].y() - pts[cr].y()) / slopeScale; // +: rises to right
            const double shade = std::clamp(1.0 + kSlopeGain * slope, kShadeLo, kShadeHi);
            const float dbm = coverage[c] != 0 ? row[c] : floorDbm;
            QColor base = QColor(palette(dbm));
            base = lerpColor(base, bgFill, depthFrac * kHaze);
            cols[c] = scaled(base, dim * shade);
        }

        // Fill — flat per-column trapezoid to the floor. AA off so adjacent
        // columns tile without seams; the AA ridge line on top hides the
        // jagged upper edge. No per-column gradient → no per-column allocs.
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(Qt::NoPen);
        for (int c = 0; c < kCols - 1; ++c) {
            poly.clear();
            poly << pts[c] << pts[c + 1]
                 << QPointF(pts[c + 1].x(), bottomY)
                 << QPointF(pts[c].x(), bottomY);
            p.setBrush(cols[c]);
            p.drawPolygon(poly);
        }

        // Ridge line — bright per-amplitude rim, AA on for a crisp crest.
        p.setRenderHint(QPainter::Antialiasing, true);
        ridgePen.setWidthF(age == 0 ? 1.6 : 1.0);
        for (int c = 0; c < kCols - 1; ++c) {
            const float dbm = coverage[c] != 0 ? row[c] : floorDbm;
            QColor rc = QColor(palette(dbm)).lighter(165);
            rc = lerpColor(rc, bgFill, depthFrac * kHaze);
            ridgePen.setColor(scaled(rc, dim));
            p.setPen(ridgePen);
            p.drawLine(pts[c], pts[c + 1]);
        }
    }
}

QVector<bool> dssDepthVisibleSegments(const QVector<qreal>& yFrontToBack)
{
    if (yFrontToBack.size() < 2) {
        return {};
    }
    QVector<bool> visible(yFrontToBack.size() - 1, true);
    qreal silhouetteY = std::numeric_limits<qreal>::max();
    for (qsizetype i = 1; i < yFrontToBack.size(); ++i) {
        visible[i - 1] = yFrontToBack.at(i - 1) <= silhouetteY + 0.5
            || yFrontToBack.at(i) <= silhouetteY + 0.5;
        silhouetteY = std::min(silhouetteY, yFrontToBack.at(i - 1));
    }
    return visible;
}
