#include "CanonSurface.h"

#include "CanonWindow.h"

#include "core/ThemeManager.h"

#include <QLinearGradient>
#include <QPainter>
#include <QToolButton>
#include <QVBoxLayout>

namespace AetherSDR {

CanonSurface::CanonSurface(QWidget* parent)
    : QWidget(parent)
{
    // A parent's stylesheet (the applet panel's, the main window's) may give
    // every QWidget a background, which would cover the ground under the body.
    ThemeManager::instance().applyStyleSheet(
        this, "QWidget#canonBody { background: transparent; }");

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(kInset, kInset, kInset, kInset);
    m_body = new QWidget(this);
    m_body->setObjectName(QStringLiteral("canonBody"));
    outer->addWidget(m_body);

    m_close = makeCanonCloseButton(this);
    connect(m_close, &QToolButton::clicked, this, &CanonSurface::closeRequested);

    // The ground is painted from tokens; rebuild it when the theme changes.
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] {
        m_ground = QPixmap();
        update();
    });
}

void CanonSurface::setCloseButtonVisible(bool visible)
{
    m_close->setVisible(visible);
}

void CanonSurface::placeCloseButtonIn(QBoxLayout* row)
{
    m_closeInCorner = false;
    row->addWidget(m_close);
}

bool CanonSurface::isCloseButtonVisible() const
{
    return !m_close->isHidden();
}

void CanonSurface::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    m_ground = QPixmap();
    if (m_closeInCorner) {
        placeCanonCloseButton(m_close);
    }
}

void CanonSurface::paintEvent(QPaintEvent*)
{
    // As in CanonWindow: the ground only changes with size, DPR or theme, so
    // paint it once into a pixmap and blit that.
    QPainter(this).drawPixmap(0, 0, ground());
}

const QPixmap& CanonSurface::ground()
{
    const qreal dpr = devicePixelRatioF();
    if (m_ground.isNull() || m_ground.size() != size() * dpr
        || !qFuzzyCompare(m_ground.devicePixelRatio(), dpr)) {
        m_ground = QPixmap(size() * dpr);
        m_ground.setDevicePixelRatio(dpr);
        m_ground.fill(Qt::transparent);
        QPainter p(&m_ground);
        paintCanonGround(p, this, size(), 0);
    }
    return m_ground;
}

QImage canonFadeStrip(const QImage& strip, const QSize& devicePx)
{
    if (strip.isNull() || devicePx.isEmpty()) {
        return {};
    }
    QImage fade = strip.scaled(devicePx, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                      .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    // The strip's device pixel ratio would turn the gradient below into
    // logical coordinates and stretch it past the bottom edge.
    fade.setDevicePixelRatio(1.0);
    QPainter mask(&fade);
    mask.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    QLinearGradient ramp(0, 0, 0, devicePx.height());
    ramp.setColorAt(0.0, QColor(0, 0, 0, 255));
    ramp.setColorAt(1.0, QColor(0, 0, 0, 0));
    mask.fillRect(QRect(QPoint(0, 0), devicePx), ramp);
    return fade;
}

} // namespace AetherSDR
