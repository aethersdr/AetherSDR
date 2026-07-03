#pragma once

#include "DragValuePopup.h"

#include <QSlider>
#include <QComboBox>
#include <QAbstractItemView>
#include <QLabel>
#include <QMouseEvent>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QWheelEvent>
#include <functional>
#include <utility>

// Global lock for sidebar controls — when locked, sliders, combo boxes,
// and scrollable labels ignore wheel/mouse events so the user can scroll
// the applet panel without accidentally changing values. (#745)
class ControlsLock {
public:
    static bool isLocked() { return s_locked; }
    static void setLocked(bool locked) { s_locked = locked; }
private:
    static inline bool s_locked = false;
};

// QSlider subclass that always consumes wheel events, even at min/max
// boundaries. Prevents scroll from propagating to parent widgets (e.g.
// SpectrumWidget tuning the VFO when a slider bottoms out). (#570)
// When controls are locked (#745), ignores wheel events and lets the
// parent scroll area handle them.
class GuardedSlider : public QSlider {
public:
    using DragValueFormatter = std::function<QString(int)>;

    explicit GuardedSlider(QWidget* parent = nullptr)
        : QSlider(parent)
    {
    }

    explicit GuardedSlider(Qt::Orientation orientation, QWidget* parent = nullptr)
        : QSlider(orientation, parent)
    {
    }

    void setDragValueFormatter(DragValueFormatter formatter) {
        m_dragValueFormatter = std::move(formatter);
    }

    void setDragValuePopupEnabled(bool enabled) {
        m_dragValuePopupEnabled = enabled;
        if (!enabled && m_dragValuePopup)
            m_dragValuePopup->hideNow();
    }

    // Flash the value badge in response to a keyboard step, then let it
    // linger and fade with the same timeout as a mouse release.  Keyboard
    // nudges for these sliders are routed through MainWindow's shortcut
    // lease (so global operating shortcuts can resume), so the lease handler
    // calls this to mirror the mouse-drag readout. (#3303 follow-up)
    void flashDragValue() {
        if (!m_dragValuePopupEnabled)
            return;
        showDragValuePopup(mapToGlobal(rect().center()));
        if (m_dragValuePopup)
            m_dragValuePopup->linger();
    }

    void mousePressEvent(QMouseEvent* ev) override {
        if (ControlsLock::isLocked()) {
            ev->ignore();
            return;
        }
        QSlider::mousePressEvent(ev);
        if (ev->button() == Qt::LeftButton) {
            m_dragValueActive = true;
            showDragValuePopup(ev->globalPosition().toPoint());
        }
    }
    void mouseMoveEvent(QMouseEvent* ev) override {
        if (ControlsLock::isLocked()) {
            ev->ignore();
            return;
        }
        QSlider::mouseMoveEvent(ev);
        if (m_dragValueActive || isSliderDown())
            showDragValuePopup(ev->globalPosition().toPoint());
    }
    void mouseReleaseEvent(QMouseEvent* ev) override {
        const bool wasActive = m_dragValueActive;
        QSlider::mouseReleaseEvent(ev);
        if (wasActive && ev->button() == Qt::LeftButton) {
            showDragValuePopup(ev->globalPosition().toPoint());
            m_dragValueActive = false;
            if (m_dragValuePopup)
                m_dragValuePopup->linger();
        }
    }
    void wheelEvent(QWheelEvent* ev) override {
        if (ControlsLock::isLocked()) {
            ev->ignore();
            return;
        }
        // Use singleStep (default 1) instead of pageStep (default 10) so
        // that mouse-wheel adjustments are fine-grained (#1026).
        int delta = ev->angleDelta().y();
        if (delta != 0)
            setValue(value() + (delta > 0 ? singleStep() : -singleStep()));
        ev->accept();
    }

protected:
    // Below is protected (not private) so subclasses that override the
    // mouse handlers for custom drag behaviour — e.g. WaterfallRateSlider's
    // click-to-jump positioning — can still drive the same drag-value popup
    // instead of silently losing it.
    QString dragValueText() const {
        if (m_dragValueFormatter)
            return m_dragValueFormatter(value());
        return QString::number(value());
    }

    QPoint dragValueAnchor(const QPoint& fallbackGlobal) const {
        QStyleOptionSlider opt;
        initStyleOption(&opt);
        const QRect handle = style()->subControlRect(
            QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
        if (handle.isValid())
            return mapToGlobal(handle.center());
        return fallbackGlobal;
    }

    void showDragValuePopup(const QPoint& fallbackGlobal) {
        if (!m_dragValuePopupEnabled)
            return;
        if (!m_dragValuePopup)
            m_dragValuePopup = new AetherSDR::DragValuePopup(this);
        m_dragValuePopup->showValue(dragValueAnchor(fallbackGlobal),
                                    dragValueText());
    }

    DragValueFormatter m_dragValueFormatter;
    AetherSDR::DragValuePopup* m_dragValuePopup{nullptr};
    bool m_dragValuePopupEnabled{true};
    bool m_dragValueActive{false};
};

// QComboBox subclass that only responds to wheel events when the dropdown
// popup is open. Prevents accidental value changes when scrolling the applet
// panel, but allows normal wheel scrolling through the list when the user
// has clicked to open the dropdown. (#570, #676)
// When controls are locked (#745), also blocks mouse press to prevent
// opening the dropdown.
class GuardedComboBox : public QComboBox {
public:
    using QComboBox::QComboBox;
    void wheelEvent(QWheelEvent* ev) override {
        if (ControlsLock::isLocked()) {
            ev->ignore();
            return;
        }
        if (view() && view()->isVisible())
            QComboBox::wheelEvent(ev);  // popup open — scroll the list
        else
            ev->ignore();  // popup closed — let parent handle scroll
    }
    void mousePressEvent(QMouseEvent* ev) override {
        if (ControlsLock::isLocked()) {
            ev->ignore();
            return;
        }
        QComboBox::mousePressEvent(ev);
    }
};

// QLabel subclass that emits scrolled(int steps) on wheel events and
// always consumes them. Used for RIT/XIT/pitch numeric displays. (#619)
// When controls are locked (#745), ignores wheel events.
class ScrollableLabel : public QLabel {
    Q_OBJECT
public:
    using QLabel::QLabel;
    void wheelEvent(QWheelEvent* ev) override {
        if (ControlsLock::isLocked()) {
            ev->ignore();
            return;
        }
        int delta = ev->angleDelta().y();
        if (delta > 0) emit scrolled(1);
        else if (delta < 0) emit scrolled(-1);
        ev->accept();
    }
signals:
    void scrolled(int direction);
};
