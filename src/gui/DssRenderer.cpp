#include "DssRenderer.h"

#include <QPainter>
#include <QPolygonF>

#include <algorithm>
#include <cmath>

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

} // namespace

void DssRenderer::clear()
{
    m_head  = 0;
    m_count = 0;
    m_dirty = true;
    m_rawHistCount = 0;
}

const std::array<float, DssRenderer::kCols>&
DssRenderer::rowAt(int age) const
{
    const int idx = (m_head + age) % kRows;
    return m_rows[idx];
}

void DssRenderer::pushRow(const QVector<float>& binsDbm)
{
    const int n = binsDbm.size();
    std::array<float, kCols> nr;

    if (n <= 0) {
        nr.fill(-200.0f);
    } else {
        std::array<float, kCols> raw;
        const double step = static_cast<double>(n) / kCols;
        for (int c = 0; c < kCols; ++c) {
            // Peak-preserving downsample: take the strongest bin in the source
            // span so signals survive as ridges. Upsampling (n < kCols)
            // collapses the span to a single source bin.
            int i0 = static_cast<int>(std::floor(c * step));
            int i1 = static_cast<int>(std::ceil((c + 1) * step));
            i0 = std::clamp(i0, 0, n - 1);
            i1 = std::clamp(i1, i0 + 1, n);
            float mx = binsDbm[i0];
            for (int i = i0 + 1; i < i1; ++i) {
                mx = std::max(mx, binsDbm[i]);
            }
            raw[c] = mx;
        }

        // Temporal median-of-3 impulse rejection. A strong broadband
        // interference burst lasts only ~1 FFT frame but, once stored, becomes a
        // full-height "wall" that recedes (and flickers) across the whole
        // surface. As the outlier of {this, prev, prev2}, such a 1-frame spike
        // is discarded here before it ever enters the height history. Steady
        // signals are the median of ~equal values, so they pass through
        // unchanged (this is an outlier rejector, not a low-pass).
        if (m_rawHistCount >= 2) {
            for (int c = 0; c < kCols; ++c) {
                nr[c] = median3(raw[c], m_rawPrev1[c], m_rawPrev2[c]);
            }
        } else {
            nr = raw;
        }
        // Shift the raw history (store the ORIGINAL raw row, not the median).
        m_rawPrev2 = m_rawPrev1;
        m_rawPrev1 = raw;
        m_rawHistCount = std::min(m_rawHistCount + 1, 2);

        // Spatial 3-tap low-pass — the textbook cure for the peak-detector
        // "comb" striping (a video-bandwidth analogue).
        std::array<float, kCols> sm = nr;
        for (int c = 0; c < kCols; ++c) {
            const float a = nr[std::max(0, c - 1)];
            const float b = nr[c];
            const float d = nr[std::min(kCols - 1, c + 1)];
            sm[c] = 0.25f * a + 0.5f * b + 0.25f * d;
        }
        nr = sm;
    }

    // Temporal IIR against the current newest row → fluid scroll + denoise.
    if (m_count > 0) {
        const auto& prev = m_rows[m_head];
        for (int c = 0; c < kCols; ++c) {
            nr[c] = kTemporalAlpha * nr[c] + (1.0f - kTemporalAlpha) * prev[c];
        }
    }

    m_head = (m_head - 1 + kRows) % kRows;
    m_rows[m_head] = nr;
    m_count = std::min(m_count + 1, kRows);
    m_dirty = true;
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
