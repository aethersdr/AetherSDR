#include "gui/MiniPanScope.h"

#include "core/ThemeManager.h"

#include <QPainter>
#include <QPainterPath>
#include <QFont>
#include <QStringList>
#include <algorithm>

namespace AetherSDR {

namespace {
// Alpha applied to the themed base colours. The tokens carry the hue; the
// weights here are the render's own (a translucent passband wash, a barely
// visible grid, a soft hairline, a lightly filled trace) and stay put across
// themes so the K4 look survives a re-tint. (#4562 review — the colour
// ratchet: no literal colours, only token + alpha.)
constexpr int kPassbandAlpha = 90;
constexpr int kGridAlpha     = 46;
constexpr int kHairlineAlpha = 120;
constexpr int kTraceFillAlpha = 60;
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
    m_bins = binsDbm;

    // Auto-scale like the main panadapter's noise-floor tracking: estimate the
    // noise floor (a low percentile of the bins) and park it near the bottom of
    // the box, leaving the upper portion as headroom for signals. Without this a
    // fixed dBm window lets the noise floor float high and the filled trace eats
    // most of the box. EMA-smoothed so the scale doesn't jitter frame to frame.
    if (m_bins.size() >= 8) {
        QVector<float> sorted = m_bins;
        const int k = sorted.size() / 5;   // ~20th percentile
        std::nth_element(sorted.begin(), sorted.begin() + k, sorted.end());
        const float nf = sorted[k];
        m_noiseFloorDbm = m_noiseFloorValid ? (0.85f * m_noiseFloorDbm + 0.15f * nf)
                                            : nf;
        m_noiseFloorValid = true;
    }
    update();
}

void MiniPanScope::setDbmRange(float minDbm, float maxDbm)
{
    m_minDbm = minDbm;
    m_maxDbm = maxDbm;
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

    // FFT trace: filled polygon + bright line.
    const int n = m_bins.size();
    if (n >= 2) {
        // Effective window: noise floor near the bottom (~75% down) with headroom
        // above for signals, or the fixed range before any bins have arrived.
        float hiDbm = m_maxDbm, loDbm = m_minDbm;
        if (m_noiseFloorValid) {
            hiDbm = m_noiseFloorDbm + kHeadroomDb;   // signal headroom above noise
            loDbm = m_noiseFloorDbm - kFloorMarginDb; // noise sits near the bottom
        }
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
        p.fillPath(fill, tinted("color.spectrum.trace", kTraceFillAlpha));
        p.setPen(QPen(tm.color(this, QStringLiteral("color.spectrum.trace")), 1.2));
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
