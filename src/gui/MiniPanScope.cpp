#include "gui/MiniPanScope.h"

#include "core/ThemeManager.h"

#include <QPainter>
#include <QPainterPath>
#include <QFont>
#include <QStringList>
#include <algorithm>

namespace AetherSDR {

namespace {
// Alpha applied to the themed chrome. These are the parts the main pan has no
// equivalent of — passband wash, grid, centre hairline — so they stay themed
// while the TRACE mirrors the source pan's own FFT Line/Fill settings. Tokens
// carry the hue; no literal colours (the colour ratchet).
constexpr int kPassbandAlpha = 90;
constexpr int kGridAlpha     = 46;
constexpr int kHairlineAlpha = 120;
} // namespace

MiniPanScope::MiniPanScope(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMinimumHeight(90);
    setAccessibleName(tr("Mini-pan spectrum scope"));
    setAccessibleDescription(
        tr("Narrow spectrum trace centred on the active VFO, with the "
           "receive passband shaded."));

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

void MiniPanScope::setSpanKHz(double kHz)
{
    m_spanKHz = kHz > 0.0 ? kHz : m_spanKHz;
    update();
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
    const auto xOf = [&](double offHz) {
        return w * 0.5 + (offHz / halfHz) * (w * 0.5);
    };

    // Passband band (translucent, brighter than the field). color.slice.a is
    // the same token the main panadapter shades slice A's passband with, so
    // the two views read as one system.
    if (m_pbHiHz > m_pbLoHz) {
        const double x0 = std::clamp(xOf(m_pbLoHz), 0.0, w);
        const double x1 = std::clamp(xOf(m_pbHiHz), 0.0, w);
        p.fillRect(QRectF(x0, 0, x1 - x0, h),
                   tinted("color.slice.a", kPassbandAlpha));
    }

    // Faint dB grid.
    p.setPen(tinted("color.spectrum.grid", kGridAlpha));
    for (int i = 1; i < 4; ++i)
        p.drawLine(QPointF(0, h * i / 4.0), QPointF(w, h * i / 4.0));

    // Centre hairline.
    p.setPen(tinted("color.accent", kHairlineAlpha));
    p.drawLine(QPointF(w * 0.5, 0), QPointF(w * 0.5, h));

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
        // Mirrored colours win; the theme token is the fallback for a scope with
        // no source pan (nothing to mirror yet).
        const QColor themeTrace =
            tm.color(this, QStringLiteral("color.spectrum.trace"));
        QColor fillC = m_fillColor.isValid() ? m_fillColor : themeTrace;
        fillC.setAlphaF(std::clamp(m_fillAlpha, 0.0f, 1.0f));
        p.fillPath(fill, fillC);
        p.setPen(QPen(m_lineColor.isValid() ? m_lineColor : themeTrace,
                      m_lineWidth));
        p.drawPath(line);
    }

    // ±span corner labels (K4 style).
    QFont f = p.font();
    f.setPointSizeF(9.0);
    p.setFont(f);
    p.setPen(tm.color(this, QStringLiteral("color.text.secondary")));
    const QString half = QString::number(m_spanKHz / 2.0, 'f', 1);
    p.drawText(QRectF(4, 2, w / 2 - 6, 16), Qt::AlignLeft  | Qt::AlignVCenter,
               "-" + half + " kHz");
    p.drawText(QRectF(w / 2 + 2, 2, w / 2 - 6, 16), Qt::AlignRight | Qt::AlignVCenter,
               half + " kHz");
}

} // namespace AetherSDR
