#include "CanonSurface.h"

#include "CanonWindow.h"

#include "core/ThemeManager.h"

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
    const qreal dpr = devicePixelRatioF();
    if (m_ground.isNull() || m_ground.size() != size() * dpr
        || !qFuzzyCompare(m_ground.devicePixelRatio(), dpr)) {
        m_ground = QPixmap(size() * dpr);
        m_ground.setDevicePixelRatio(dpr);
        m_ground.fill(Qt::transparent);
        QPainter p(&m_ground);
        paintCanonGround(p, this, size(), 0);
    }
    QPainter(this).drawPixmap(0, 0, m_ground);
}

} // namespace AetherSDR
