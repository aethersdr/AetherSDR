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

float rowSample(const std::array<float, DssRenderer::kCols>& row,
                double srcLeft, double srcRight, double srcCenter,
                float fallback)
{
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
        const float leftValue = std::isfinite(row[left]) ? row[left] : fallback;
        const float rightValue = std::isfinite(row[right]) ? row[right] : leftValue;
        const float frac = static_cast<float>(clampedCenter - left);
        return leftValue + frac * (rightValue - leftValue);
    }

    const int first = std::clamp(static_cast<int>(std::floor(clampedLeft)),
                                 0, DssRenderer::kCols - 1);
    const int last = std::clamp(static_cast<int>(std::ceil(clampedRight)) - 1,
                                0, DssRenderer::kCols - 1);
    double weightedSum = 0.0;
    double totalWeight = 0.0;
    for (int i = first; i <= last; ++i) {
        if (!std::isfinite(row[i])) {
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
    return totalWeight > 0.0
        ? static_cast<float>(weightedSum / totalWeight)
        : fallback;
}

std::array<float, DssRenderer::kCols> reprojectRow(
    const std::array<float, DssRenderer::kCols>& row,
    double oldCenterMhz,
    double oldBandwidthMhz,
    double newCenterMhz,
    double newBandwidthMhz,
    float fallback)
{
    std::array<float, DssRenderer::kCols> remapped;
    remapped.fill(fallback);
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
        remapped[dst] = rowSample(row,
                                  srcCenter - srcWidthBins * 0.5,
                                  srcCenter + srcWidthBins * 0.5,
                                  srcCenter,
                                  fallback);
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

void DssRenderer::clear()
{
    m_head  = 0;
    m_count = 0;
    m_dirty = true;
    m_rawHistCount = 0;
    m_historyWriteRow = 0;
    m_historyRowCount = 0;
    resetHistorySmoothing();
}

quint64 DssRenderer::fixedStorageBytes() const
{
    return sizeof(m_rows)
        + sizeof(m_rawPrev1) + sizeof(m_rawPrev2)
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

void DssRenderer::pushRow(const QVector<float>& binsDbm)
{
    const std::array<float, kCols> raw = resampledRawRow(binsDbm, -200.0f);
    const std::array<float, kCols>* previous = m_count > 0
        ? &m_rows[m_head]
        : nullptr;
    const std::array<float, kCols> nr =
        smoothDssRow(raw, m_rawPrev1, m_rawPrev2, m_rawHistCount, previous);

    m_head = (m_head - 1 + kRows) % kRows;
    m_rows[m_head] = nr;
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
    if (m_historyRowCount > 0) {
        const qfloat16* src = m_historyRows.constData() + m_historyWriteRow * kCols;
        for (int c = 0; c < kCols; ++c) {
            previousRow[c] = static_cast<float>(src[c]);
        }
        previous = &previousRow;
    }
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
        for (int c = 0; c < kCols; ++c) {
            row[c] = static_cast<float>(src[c]);
        }

        const double rowCenterMhz = m_historyRowCenterMhz.value(historyRing, 0.0);
        const double rowBandwidthMhz =
            m_historyRowBandwidthMhz.value(historyRing, 0.0);
        if (rowCenterMhz > 0.0 && rowBandwidthMhz > 0.0
            && centerMhz > 0.0 && bandwidthMhz > 0.0
            && (rowCenterMhz != centerMhz || rowBandwidthMhz != bandwidthMhz)) {
            row = reprojectRow(row,
                               rowCenterMhz, rowBandwidthMhz,
                               centerMhz, bandwidthMhz,
                               fallbackDbm);
        }

        m_rows[age] = row;
    }

    m_dirty = true;
    m_rawHistCount = 0;
    ++m_rowGeneration;
}

void DssRenderer::reprojectFrequencyFrame(double oldCenterMhz,
                                          double oldBandwidthMhz,
                                          double newCenterMhz,
                                          double newBandwidthMhz,
                                          float fallbackDbm)
{
    if (m_count <= 0 || oldBandwidthMhz <= 0.0 || newBandwidthMhz <= 0.0) {
        return;
    }

    for (int age = 0; age < m_count; ++age) {
        const int ring = (m_head + age) % kRows;
        m_rows[ring] = reprojectRow(m_rows[ring],
                                    oldCenterMhz, oldBandwidthMhz,
                                    newCenterMhz, newBandwidthMhz,
                                    fallbackDbm);
    }
    if (m_rawHistCount >= 1) {
        m_rawPrev1 = reprojectRow(m_rawPrev1,
                                  oldCenterMhz, oldBandwidthMhz,
                                  newCenterMhz, newBandwidthMhz,
                                  fallbackDbm);
    }
    if (m_rawHistCount >= 2) {
        m_rawPrev2 = reprojectRow(m_rawPrev2,
                                  oldCenterMhz, oldBandwidthMhz,
                                  newCenterMhz, newBandwidthMhz,
                                  fallbackDbm);
    }

    m_dirty = true;
    ++m_rowGeneration;
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
    const double denom         = kRows;

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
    for (int age = m_count - 1; age >= 0; --age) {
        const double depthFrac    = age / denom;
        const double rowWidthFrac = 1.0 - depthFrac * (1.0 - kBackWidthFrac);
        const double inset        = W * (1.0 - rowWidthFrac) * 0.5;
        const double rowW         = W - 2.0 * inset;
        const double baselineY    = bottomY - depthFrac * depthSpan;
        const double maxRidge     = frontMaxRidge * rowWidthFrac;
        const double dim          = kMinDim + (1.0 - kMinDim) * (1.0 - depthFrac);

        const auto& row = rowAt(age);
        // Pass 1: geometry — noise-floor-anchored ridge heights, with the same
        // pow(s, zCurve) floor-lift the GPU shader applies.
        for (int c = 0; c < kCols; ++c) {
            const double x = inset + (kCols > 1 ? double(c) / (kCols - 1) : 0.0) * rowW;
            const float dbm = row[c];
            double strength = std::clamp((dbm - floorDbm) / rangeDb, 0.0f, 1.0f);
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
            QColor base = QColor(palette(row[c]));
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
            QColor rc = QColor(palette(row[c])).lighter(165);
            rc = lerpColor(rc, bgFill, depthFrac * kHaze);
            ridgePen.setColor(scaled(rc, dim));
            p.setPen(ridgePen);
            p.drawLine(pts[c], pts[c + 1]);
        }
    }
}
