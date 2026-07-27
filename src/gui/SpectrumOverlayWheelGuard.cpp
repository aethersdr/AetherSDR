#include "SpectrumOverlayWheelGuard.h"

#include "GuardedSlider.h"   // ControlsLock

#include <algorithm>

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

int constrainedDisplayPanelTop(int menuBottom, int panelHeight, int hostHeight)
{
    // Never push the bottom edge past the host: a child is clipped by its
    // parent, so any overhang is dead space the scroll area cannot reveal.
    const int lowestTop = std::max(0, hostHeight - panelHeight);
    return std::clamp(menuBottom - panelHeight, 0, lowestTop);
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
    // Navigation controls always keep the wheel — a list is scrolled by it,
    // never valued by it, so the controls lock has nothing to protect there.
    if (qobject_cast<QAbstractItemView*>(widget) || isScrollAreaViewport(widget)) {
        return true;
    }

    // Value controls yield the wheel while the global controls lock is on
    // (#745): sliders span most of every Display row, so without this the pane's
    // scroll affordance is unreachable over the majority of its surface and a
    // user wheeling to scroll silently retunes Averaging/FPS/Opacity instead.
    // GuardedSlider/GuardedCombo already ignore the wheel when locked, but that
    // only reaches the scroll area via Qt's parent propagation for spontaneous
    // events; deciding it here makes the routing explicit and deterministic at
    // the boundary this guard owns.
    if (qobject_cast<QAbstractSlider*>(widget)
        || qobject_cast<QAbstractSpinBox*>(widget)) {
        return !ControlsLock::isLocked();
    }

    if (QComboBox* combo = qobject_cast<QComboBox*>(widget)) {
        return !ControlsLock::isLocked() && combo->view()
            && combo->view()->isVisible();
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

    // Forward to the scroll BAR rather than the viewport: it applies the wheel
    // directly without re-entering the viewport's own wheel handling.
    QWidget* scrollBar = m_displayScrollArea->verticalScrollBar();
    const QPointF scrollPosition =
        scrollBar->mapFromGlobal(wheelEvent->globalPosition().toPoint());
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
    QCoreApplication::sendEvent(scrollBar, &forwardedEvent);
    m_forwardingDisplayWheel = false;
    wheelEvent->accept();
}

} // namespace AetherSDR
