#include "gui/MiniPanScope.h"

#include "gui/FftHeatMap.h"
#include "gui/MiniPanReslice.h"   // passbandCenterOffsetHz — shared with the re-slice
#include "gui/SpectrumGrid.h"

#include "core/ThemeManager.h"

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QPolygonF>
#include <QFont>
#include <QFontMetricsF>
#include <QStringList>
#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {
// Alpha applied to the themed chrome. These are the parts the main pan has no
// equivalent of — passband wash, grid, centre hairline — so they stay themed
// while the TRACE mirrors the source pan's own FFT Line/Fill settings. Tokens
// carry the hue; no literal colours (the colour ratchet).
constexpr int kPassbandAlpha = 90;
constexpr int kHairlineAlpha = 120;
} // namespace

MiniPanScope::MiniPanScope(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMinimumHeight(90);
    setAccessibleName(tr("Mini-pan spectrum scope"));
    setAccessibleDescription(
        tr("Narrow spectrum trace centred on the active VFO's receive passband, "
           "which is shaded, with a hairline on the carrier frequency."));

    // The render paints through raw QPainter keyed off ThemeManager::color(),
    // so applyStyleSheet's reverse-map never sees these. Declare them so an
    // Inspect-mode click on the scope surfaces the tokens it actually reads.
    auto& tm = ThemeManager::instance();
    tm.declareWidgetTokens(this, QStringList{
        "color.background.spectrum",
        "color.spectrum.trace",
        "color.spectrum.grid",
        "color.slice.a",
        "color.accent",
        "color.text.secondary",
        "color.text.primary",
    });
    connect(&tm, &ThemeManager::themeChanged, this,
            qOverload<>(&QWidget::update));
}

void MiniPanScope::updateSpectrum(const QVector<float>& binsDbm)
{
    // No local auto-scaling: the vertical window is the source pan's own dBm
    // range (see setDbmRange). Re-deriving it here would make the mini-pan
    // disagree with the main pan about how tall a signal is, and would ignore
    // the FFT Floor slider the operator set on that pan.
    m_bins = binsDbm;
    update();
}

void MiniPanScope::setDbmRange(float minDbm, float maxDbm)
{
    if (qFuzzyCompare(m_minDbm, minDbm) && qFuzzyCompare(m_maxDbm, maxDbm))
        return;                          // mirrored per frame — don't repaint on no-op
    m_minDbm = minDbm;
    m_maxDbm = maxDbm;
    update();
}

void MiniPanScope::setTraceAppearance(const QColor& lineColor,
                                      const QColor& fillColor,
                                      float fillAlpha, float lineWidth)
{
    if (m_lineColor == lineColor && m_fillColor == fillColor
        && qFuzzyCompare(m_fillAlpha, fillAlpha)
        && qFuzzyCompare(m_lineWidth, lineWidth))
        return;                          // mirrored per frame — see above
    m_lineColor = lineColor;
    m_fillColor = fillColor;
    m_fillAlpha = fillAlpha;
    m_lineWidth = lineWidth;
    update();
}

void MiniPanScope::setHeatMap(bool on)
{
    if (m_heatMap == on) return;         // mirrored per frame — no-op repaints add up
    m_heatMap = on;
    update();
}

void MiniPanScope::setShowGrid(bool on)
{
    if (m_showGrid == on) return;
    m_showGrid = on;
    update();
}

void MiniPanScope::setSpanKHz(double kHz)
{
    m_spanKHz = kHz > 0.0 ? kHz : m_spanKHz;
    update();
}

void MiniPanScope::setVfoMhz(double mhz)
{
    if (qFuzzyCompare(m_vfoMhz, mhz)) return;
    m_vfoMhz = mhz;
    update();
}

QString MiniPanScope::readoutText() const
{
    return m_vfoMhz > 0.0 ? QString::number(m_vfoMhz, 'f', 6)
                          : QStringLiteral("—.———");
}

void MiniPanScope::setPassbandHz(int lowHz, int highHz)
{
    m_pbLoHz = lowHz;
    m_pbHiHz = highHz;
    update();
}

void MiniPanScope::paintEvent(QPaintEvent*)
{
    auto& tm = ThemeManager::instance();
    const auto tinted = [&tm, this](const char* token, int alpha) {
        QColor c = tm.color(this, QLatin1String(token));
        c.setAlpha(alpha);
        return c;
    };

    QPainter p(this);
    const double w = width(), h = height();
    p.fillRect(rect(), tm.color(this, QStringLiteral("color.background.spectrum")));

    const double halfHz = m_spanKHz * 1000.0 / 2.0;   // e.g. 5000 Hz for ±5 kHz

    // The view is centred on the PASSBAND centre, so a carrier-relative offset
    // (which is what SliceModel reports, and what MainWindow re-slices around)
    // has to be shifted by the carrier's own position before it can be drawn.
    // Same helper MainWindow picks the re-slice window with — one definition, so
    // the trace and this chrome cannot end up centred on different frequencies.
    const double carrierOffHz =
        -MiniPan::passbandCenterOffsetHz(m_pbLoHz, m_pbHiHz);
    const auto xOf = [&](double offHzFromCentre) {
        return w * 0.5 + (offHzFromCentre / halfHz) * (w * 0.5);
    };
    const auto xOfCarrierRel = [&](double offHzFromCarrier) {
        return xOf(offHzFromCarrier + carrierOffHz);
    };

    // Passband band (translucent, brighter than the field). color.slice.a is
    // the same token the main panadapter shades slice A's passband with, so
    // the two views read as one system. It lands symmetrically about the middle
    // by construction — that is what centring on the passband means.
    if (m_pbHiHz > m_pbLoHz) {
        const double x0 = std::clamp(xOfCarrierRel(m_pbLoHz), 0.0, w);
        const double x1 = std::clamp(xOfCarrierRel(m_pbHiHz), 0.0, w);
        p.fillRect(QRectF(x0, 0, x1 - x0, h),
                   tinted("color.slice.a", kPassbandAlpha));
    }

    // dB grid — same token, same dotted pen and the same dB ladder as
    // SpectrumWidget::drawGrid, so a rule means the same level in both views.
    // The lines used to sit at fixed quarters of the height, which looked
    // similar and meant nothing.
    //
    // No VERTICAL rules: the main pan's frequency spacing is chosen for its own
    // span (tens of kHz and up), so inside a 10 kHz window it yields one line or
    // none. The carrier hairline already marks the only frequency of interest.
    if (m_showGrid) {
        const float range = m_maxDbm - m_minDbm;
        if (range > 0.0f) {
            const float step = SpectrumGrid::dbStep(range);
            const float first = std::ceil(m_minDbm / step) * step;
            p.setPen(QPen(tm.color(this, QStringLiteral("color.spectrum.grid")),
                          1, Qt::DotLine));
            for (float dbm = first; dbm <= m_maxDbm; dbm += step) {
                const double y = (m_maxDbm - dbm) / range * h;
                p.drawLine(QPointF(0, y), QPointF(w, y));
            }
        }
    }

    // Carrier hairline — the VFO the readout above it shows. It sits dead centre
    // only on a symmetric passband; on SSB it marks the sideband's edge, which is
    // the point of centring on the passband instead.
    //
    // Skipped entirely when the carrier falls outside the span (a filter wider
    // than the view can push it out): clamping it to the edge would plant the
    // dial frequency on a frequency it is not on, and this is a tuning aid.
    if (std::abs(carrierOffHz) <= halfHz) {
        p.setPen(tinted("color.accent", kHairlineAlpha));
        const double xc = xOf(carrierOffHz);
        p.drawLine(QPointF(xc, 0), QPointF(xc, h));
    }

    // FFT trace: filled polygon + bright line, in the source pan's own colours
    // and dBm window so the two views agree.
    const int n = m_bins.size();
    if (n >= 2) {
        const float hiDbm = m_maxDbm, loDbm = m_minDbm;
        // (std::max) parenthesised so MSVC's windows.h max() macro (pulled in
        // transitively by Qt on Windows) can't mangle the call. (#4562 CI)
        const double span = (std::max)(1.0f, hiDbm - loDbm);
        const auto yOf = [&](float dbm) {
            const double t = (hiDbm - dbm) / span;   // 0 at top (max), 1 at bottom
            return std::clamp(t, 0.0, 1.0) * h;
        };
        const float alpha = std::clamp(m_fillAlpha, 0.0f, 1.0f);
        p.setRenderHint(QPainter::Antialiasing, true);

        if (m_heatMap) {
            // Per-column trapezoid with a vertical gradient from the intensity
            // colour down to the shared base — the same construction the main
            // pan uses, so a signal reads the same colour in both views.
            for (int i = 0; i < n - 1; ++i) {
                const double x0 = (double(i) / (n - 1)) * w;
                const double x1 = (double(i + 1) / (n - 1)) * w;
                const double y0 = yOf(m_bins[i]);
                const double y1 = yOf(m_bins[i + 1]);
                // t: 0 at the top of the dBm window (noise floor end), 1 strong.
                const float t = 1.0f - static_cast<float>(
                    std::clamp(((y0 + y1) * 0.5) / (h > 0 ? h : 1), 0.0, 1.0));
                QColor top = FftHeatMap::heatColor(t);
                top.setAlphaF(alpha * FftHeatMap::kTopAlphaScale);
                QLinearGradient grad(0, std::min(y0, y1), 0, h);
                grad.setColorAt(0.0, top);
                grad.setColorAt(1.0, FftHeatMap::gradientBase(alpha));
                p.setPen(Qt::NoPen);
                p.setBrush(grad);
                QPolygonF col;
                col << QPointF(x0, y0) << QPointF(x1, y1)
                    << QPointF(x1, h)  << QPointF(x0, h);
                p.drawPolygon(col);
            }
            if (m_lineWidth > 0.0f) {
                QPen pen;
                pen.setCosmetic(true);
                pen.setWidthF(m_lineWidth);
                pen.setCapStyle(Qt::RoundCap);
                for (int i = 0; i < n - 1; ++i) {
                    const double x0 = (double(i) / (n - 1)) * w;
                    const double x1 = (double(i + 1) / (n - 1)) * w;
                    const double y0 = yOf(m_bins[i]);
                    const double y1 = yOf(m_bins[i + 1]);
                    const float t = 1.0f - static_cast<float>(
                        std::clamp(((y0 + y1) * 0.5) / (h > 0 ? h : 1), 0.0, 1.0));
                    pen.setColor(FftHeatMap::heatColor(t));
                    p.setPen(pen);
                    p.drawLine(QPointF(x0, y0), QPointF(x1, y1));
                }
            }
        } else {
            QPainterPath line;
            for (int i = 0; i < n; ++i) {
                const double x = (double(i) / (n - 1)) * w;
                const double y = yOf(m_bins[i]);
                if (i == 0) line.moveTo(x, y);
                else        line.lineTo(x, y);
            }
            QPainterPath fill = line;
            fill.lineTo(w, h);
            fill.lineTo(0, h);
            fill.closeSubpath();
            // Mirrored colours win; the theme token is the fallback for a scope
            // with no source pan (nothing to mirror yet).
            const QColor themeTrace =
                tm.color(this, QStringLiteral("color.spectrum.trace"));
            QColor fillC = m_fillColor.isValid() ? m_fillColor : themeTrace;
            fillC.setAlphaF(alpha);
            p.fillPath(fill, fillC);
            if (m_lineWidth > 0.0f) {
                p.setPen(QPen(m_lineColor.isValid() ? m_lineColor : themeTrace,
                              m_lineWidth));
                p.drawPath(line);
            }
        }
    }

    // Top row: ±span labels in the corners, VFO readout centred between them.
    // One row rather than a QLabel above the scope — that label's row was pure
    // dead space, and the trace is what the operator is looking at.
    QFont spanFont = p.font();
    spanFont.setPointSizeF(9.0);
    QFont freqFont = p.font();
    freqFont.setPointSizeF(12.0);
    freqFont.setBold(true);

    const QFontMetricsF spanFm(spanFont);
    const QFontMetricsF freqFm(freqFont);
    const double rowH = std::max(spanFm.height(), freqFm.height()) + 2.0;

    const QString half = QString::number(m_spanKHz / 2.0, 'f', 1);
    const QString spanLo = "-" + half + " kHz";
    const QString spanHi = half + " kHz";
    const QString freq = readoutText();

    // The readout is the important one: if the tile is too narrow to hold all
    // three without collision, the span labels drop rather than overlap it.
    const double spanW = std::max(spanFm.horizontalAdvance(spanLo),
                                  spanFm.horizontalAdvance(spanHi));
    const bool showSpanLabels = (w - freqFm.horizontalAdvance(freq)) / 2.0
                                > spanW + 6.0;

    if (showSpanLabels) {
        p.setFont(spanFont);
        p.setPen(tm.color(this, QStringLiteral("color.text.secondary")));
        p.drawText(QRectF(4, 1, w / 2 - 6, rowH),
                   Qt::AlignLeft | Qt::AlignVCenter, spanLo);
        p.drawText(QRectF(w / 2 + 2, 1, w / 2 - 6, rowH),
                   Qt::AlignRight | Qt::AlignVCenter, spanHi);
    }
    p.setFont(freqFont);
    p.setPen(tm.color(this, QStringLiteral("color.text.primary")));
    p.drawText(QRectF(0, 1, w, rowH), Qt::AlignHCenter | Qt::AlignVCenter, freq);
}

} // namespace AetherSDR
