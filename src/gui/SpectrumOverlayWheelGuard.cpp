#include "SpectrumOverlayWheelGuard.h"

#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QVariant>
#include <QWheelEvent>
#include <QWidget>

namespace AetherSDR {

namespace {

constexpr const char* kBoundaryModeProperty =
    "_aetherSpectrumOverlayWheelBoundaryMode";

} // namespace

QSize constrainedDisplayPanelSize(const QSize& contentHint, int hostHeight,
                                  int scrollBarExtent)
{
    QSize panelSize(contentHint.width() + 2, contentHint.height() + 2);
    if (panelSize.height() > hostHeight) {
        panelSize.setHeight(hostHeight);
        panelSize.rwidth() += scrollBarExtent;
    }
    return panelSize;
}

SpectrumOverlayWheelGuard::SpectrumOverlayWheelGuard(QObject* parent)
    : QObject(parent)
{
}

void SpectrumOverlayWheelGuard::setDisplayScrollArea(QScrollArea* scrollArea)
{
    m_displayScrollArea = scrollArea;
}

void SpectrumOverlayWheelGuard::guardTree(QWidget* root, BoundaryMode mode)
{
    if (!root) {
        return;
    }

    const int modeValue = static_cast<int>(mode);
    root->setProperty(kBoundaryModeProperty, modeValue);
    root->installEventFilter(this);

    const QList<QWidget*> descendants = root->findChildren<QWidget*>();
    for (QWidget* descendant : descendants) {
        descendant->setProperty(kBoundaryModeProperty, modeValue);
        descendant->installEventFilter(this);
    }
}

bool SpectrumOverlayWheelGuard::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() != QEvent::Wheel) {
        return QObject::eventFilter(watched, event);
    }

    QWidget* widget = qobject_cast<QWidget*>(watched);
    QWheelEvent* wheelEvent = static_cast<QWheelEvent*>(event);
    if (!widget || !widget->property(kBoundaryModeProperty).isValid()) {
        return QObject::eventFilter(watched, event);
    }

    // A forwarded Display event may be ignored at a scroll limit. Consume its
    // overlay-parent propagation instead of recursively forwarding it or
    // allowing it to reach SpectrumWidget.
    if (m_forwardingDisplayWheel) {
        if (widget == m_displayScrollArea
            || qobject_cast<QAbstractSlider*>(widget)
            || isScrollAreaViewport(widget)) {
            return QObject::eventFilter(watched, event);
        }
        wheelEvent->accept();
        return true;
    }

    if (intentionallyOwnsWheel(widget)) {
        return QObject::eventFilter(watched, event);
    }

    const BoundaryMode mode = static_cast<BoundaryMode>(
        widget->property(kBoundaryModeProperty).toInt());
    if (mode == BoundaryMode::ScrollDisplay) {
        routeToDisplayScroll(wheelEvent);
    } else {
        wheelEvent->accept();
    }
    return true;
}

bool SpectrumOverlayWheelGuard::intentionallyOwnsWheel(QWidget* widget) const
{
    if (qobject_cast<QAbstractSlider*>(widget)
        || qobject_cast<QAbstractSpinBox*>(widget)
        || qobject_cast<QAbstractItemView*>(widget)
        || isScrollAreaViewport(widget)) {
        return true;
    }

    if (QComboBox* combo = qobject_cast<QComboBox*>(widget)) {
        return combo->view() && combo->view()->isVisible();
    }

    return false;
}

bool SpectrumOverlayWheelGuard::isScrollAreaViewport(QWidget* widget) const
{
    QWidget* parent = widget ? widget->parentWidget() : nullptr;
    const QAbstractScrollArea* scrollArea =
        qobject_cast<QAbstractScrollArea*>(parent);
    return scrollArea && scrollArea->viewport() == widget;
}

void SpectrumOverlayWheelGuard::routeToDisplayScroll(QWheelEvent* wheelEvent)
{
    if (!m_displayScrollArea || !m_displayScrollArea->viewport()) {
        wheelEvent->accept();
        return;
    }

    QWidget* scrollArea = m_displayScrollArea->verticalScrollBar();
    const QPointF scrollPosition =
        scrollArea->mapFromGlobal(wheelEvent->globalPosition().toPoint());
    QWheelEvent forwardedEvent(
        scrollPosition,
        wheelEvent->globalPosition(),
        wheelEvent->pixelDelta(),
        wheelEvent->angleDelta(),
        wheelEvent->buttons(),
        wheelEvent->modifiers(),
        wheelEvent->phase(),
        wheelEvent->inverted(),
        wheelEvent->source(),
        wheelEvent->pointingDevice());

    m_forwardingDisplayWheel = true;
    QCoreApplication::sendEvent(scrollArea, &forwardedEvent);
    m_forwardingDisplayWheel = false;
    wheelEvent->accept();
}

} // namespace AetherSDR
