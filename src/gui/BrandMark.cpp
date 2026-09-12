#include "BrandMark.h"

#include "core/ThemeManager.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QtMath>

namespace AetherSDR {

namespace {
constexpr const char* kLogoResource = ":/images/logo-96.png";
constexpr int kLogoTextGap = 8;   // logo → wordmark
constexpr qreal kWordmarkPointless = 15.0;  // px, per the title-bar spec
// "Aether" and "SDR" are painted as two runs of one wordmark, so they are
// separate string constants rather than one label's text.
const QString kWordOne = QStringLiteral("Aether");
const QString kWordTwo = QStringLiteral("SDR");
}

BrandMark::BrandMark(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("brandMark"));
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setFocusPolicy(Qt::NoFocus);
    setAccessibleName(QStringLiteral("AetherSDR"));
    setAccessibleDescription(QStringLiteral("AetherSDR brand mark"));
    setToolTip(QStringLiteral("AetherSDR"));

    // Painter-driven widgets don't get ThemeManager's stylesheet re-apply for
    // free — repaint explicitly so a theme switch retints the wordmark and the
    // gradient without a restart.
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, qOverload<>(&QWidget::update));

    // Load now rather than on first paint.  rebuildLogo() re-runs whenever the
    // device pixel ratio changes, so an early load at DPR 1 is not a HiDPI
    // hazard — and it means hasLogo() answers "is the asset there", not "have
    // we painted yet", which is the question a headless check needs to ask.
    rebuildLogo();
}

QFont BrandMark::wordmarkFont() const
{
    QFont f = ThemeManager::instance().font(this, QStringLiteral("font.family.ui"));
    f.setPixelSize(int(kWordmarkPointless));
    f.setWeight(QFont::Weight(650));
    // −0.02 em tracking.  PercentageSpacing is expressed as a percentage of the
    // natural advance, so 0.98 em of advance == 98 %.
    f.setLetterSpacing(QFont::PercentageSpacing, 98.0);
    return f;
}

QString BrandMark::wordmarkText() const
{
    return kWordOne + kWordTwo;
}

void BrandMark::rebuildLogo()
{
    const qreal dpr = devicePixelRatioF();
    if (!m_logo.isNull() && qFuzzyCompare(dpr, m_logoDpr)) {
        return;
    }

    QPixmap source(QString::fromLatin1(kLogoResource));
    if (source.isNull()) {
        m_logo = QPixmap();
        m_logoDpr = dpr;
        return;
    }

    const int px = int(qRound(kLogoDiameter * dpr));
    QPixmap scaled = source.scaled(px, px, Qt::KeepAspectRatio,
                                   Qt::SmoothTransformation);

    // Clip circular.  The source master is square with square corners, so the
    // rounding has to happen here rather than being baked into the asset — the
    // same PNG is the app icon, where the platform applies its own mask.
    QPixmap circular(px, px);
    circular.fill(Qt::transparent);
    {
        QPainter p(&circular);
        p.setRenderHint(QPainter::Antialiasing, true);
        QPainterPath clip;
        clip.addEllipse(QRectF(0, 0, px, px));
        p.setClipPath(clip);
        p.drawPixmap(0, 0, scaled);
    }
    circular.setDevicePixelRatio(dpr);
    m_logo = circular;
    m_logoDpr = dpr;
}

QSize BrandMark::sizeHint() const
{
    const QFontMetricsF fm(wordmarkFont());
    const int textWidth = int(qCeil(fm.horizontalAdvance(kWordOne)
                                    + fm.horizontalAdvance(kWordTwo)));
    return QSize(kLogoDiameter + kLogoTextGap + textWidth,
                 qMax(kLogoDiameter, int(qCeil(fm.height()))));
}

void BrandMark::paintEvent(QPaintEvent* ev)
{
    Q_UNUSED(ev);
    rebuildLogo();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    qreal x = 0.0;
    if (!m_logo.isNull()) {
        const qreal y = (height() - kLogoDiameter) / 2.0;
        p.drawPixmap(QPointF(x, y), m_logo);
        x += kLogoDiameter + kLogoTextGap;
    }

    const QFont font = wordmarkFont();
    p.setFont(font);
    const QFontMetricsF fm(font);
    const qreal baseline = (height() + fm.capHeight()) / 2.0;

    auto& theme = ThemeManager::instance();

    // "Aether" — flat brand ink.
    p.setPen(theme.color(this, QStringLiteral("color.brand.wordmark")));
    p.drawText(QPointF(x, baseline), kWordOne);
    x += fm.horizontalAdvance(kWordOne);

    // "SDR" — gradient fill.  Painting through a QPainterPath of the glyph
    // outlines is what lets the gradient run *inside* the letterforms; setting
    // a gradient pen would only stroke them.
    const QRectF gradientBounds(x, 0, fm.horizontalAdvance(kWordTwo),
                                qreal(height()));
    QPainterPath glyphs;
    glyphs.addText(QPointF(x, baseline), font, kWordTwo);
    p.setPen(Qt::NoPen);
    p.setBrush(theme.brush(this, QStringLiteral("color.brand.gradient"),
                           gradientBounds.toRect()));
    p.drawPath(glyphs);
}

} // namespace AetherSDR
