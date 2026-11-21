#pragma once

#include "GuardedSlider.h"
#include "core/ThemeManager.h"

#include <QAccessible>
#include <QAccessibleWidget>

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QString>
#include <QStyle>
#include <QStyleOptionSlider>

#include <cstdlib>

// Drag readout for a 0-100 pan whose centre is 50: "C", "L30", "R30".
inline QString panText(int value)
{
    if (value == 50)
        return QStringLiteral("C");
    return QStringLiteral("%1%2").arg(value < 50 ? QStringLiteral("L") : QStringLiteral("R"))
                                .arg(std::abs(value - 50));
}

class CenterMarkSlider;
inline QAccessibleInterface* centerMarkSliderAccessibleFactory(const QString& key,
                                                               QObject* object);

// Slider that resets to a default value on double-click.
// Extends GuardedSlider for controls-lock support (#745).
class ResetSlider : public GuardedSlider {
public:
    explicit ResetSlider(int resetVal, Qt::Orientation o, QWidget* parent = nullptr)
        : GuardedSlider(o, parent), m_resetVal(resetVal) {}
protected:
    void mouseDoubleClickEvent(QMouseEvent*) override { setValue(m_resetVal); }
private:
    int m_resetVal;
};

// ResetSlider filled from the centre outward with a centre-mark dot, for
// pan/balance controls whose zero is the midpoint. Overpaints the stylesheet's
// 0→handle sub-page with groove colour, then fills centre→handle in accent,
// clipped to exclude the handle disc. One copy for every pan control: the
// RX applet, the VFO audio tab and the CW sidetone pan (#7260).
class CenterMarkSlider : public ResetSlider {
public:
    explicit CenterMarkSlider(int resetVal, Qt::Orientation o, QWidget* parent = nullptr)
        : ResetSlider(resetVal, o, parent)
    {
        // Every CenterMarkSlider is a 0-100 pan centred on 50, so the drag
        // readout and the accessible value both say C, L30 or R30. Before,
        // only the RX applet and the CW pan set this; the VFO pan read 0-100.
        setDragValueFormatter(panText);

        // A screen reader reads the accessible Value. Qt's own slider
        // interface reports the raw number, so a pan the operator sees as
        // L21 was announced as "29". Install a factory that hands this
        // class its own interface, and evict one Qt may already have cached
        // for this object (the ScrollableLabel lesson in GuardedSlider.h).
        static bool s_factoryInstalled = false;
        if (!s_factoryInstalled) {
            s_factoryInstalled = true;
            QAccessible::installFactory(centerMarkSliderAccessibleFactory);
        }
        if (QAccessibleInterface* cached = QAccessible::queryAccessibleInterface(this)) {
            if (cached->text(QAccessible::Value) != accessibleValueText())
                QAccessible::deleteAccessibleInterface(QAccessible::uniqueId(cached));
        }
    }

    // What the slider shows while dragged, and what assistive tech reads.
    QString accessibleValueText() const { return dragValueText(); }
protected:
    void paintEvent(QPaintEvent* ev) override {
        ResetSlider::paintEvent(ev);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        auto& tm = AetherSDR::ThemeManager::instance();
        const int cx = width() / 2;
        const int cy = height() / 2;
        const int grooveY = cy - 2;

        QStyleOptionSlider opt;
        initStyleOption(&opt);
        const QRect handleRect = style()->subControlRect(
            QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
        const int handleCx = handleRect.center().x();

        // Clip out the handle so the overpaint never touches its pixels.
        QPainterPath clip;
        clip.addRect(rect());
        QPainterPath handlePath;
        handlePath.addEllipse(handleRect.adjusted(-1, -1, 1, 1));
        p.setClipPath(clip.subtracted(handlePath));

        p.setPen(Qt::NoPen);
        if (handleCx < cx) {
            // Sub-page already painted (0 → handle); erase it with groove
            // colour, then add accent (handle → centre).
            p.setBrush(tm.color("color.background.1"));
            p.drawRect(QRect(0, grooveY, handleCx, 4));
            p.setBrush(tm.color("color.accent"));
            p.drawRect(QRect(handleCx, grooveY, cx - handleCx, 4));
        } else {
            // Sub-page already paints (0 → handle); we only want (centre
            // → handle), so erase (0 → centre) with groove colour.
            p.setBrush(tm.color("color.background.1"));
            p.drawRect(QRect(0, grooveY, cx, 4));
        }

        // Centre-mark dot: the landmark for the neutral position, in the
        // label colour so it follows the theme like the L and R labels.
        p.setBrush(tm.color("color.text.label"));
        p.drawEllipse(QPointF(cx, cy), 2.5, 2.5);
    }
};

// Accessible interface for CenterMarkSlider: role Slider, the numeric range
// through QAccessibleValueInterface (assistive tech that wants a number still
// gets 0-100), and the Value text from the slider's own readout, so a screen
// reader says "L21" where the operator sees L21. Setting a value honours the
// sidebar controls lock (#745), as wheel and mouse input do.
class CenterMarkSliderAccessible : public QAccessibleWidget, public QAccessibleValueInterface {
public:
    explicit CenterMarkSliderAccessible(QWidget* widget)
        : QAccessibleWidget(widget, QAccessible::Slider) {}

    void* interface_cast(QAccessible::InterfaceType type) override
    {
        if (type == QAccessible::ValueInterface)
            return static_cast<QAccessibleValueInterface*>(this);
        return QAccessibleWidget::interface_cast(type);
    }

    QString text(QAccessible::Text t) const override
    {
        if (t == QAccessible::Value)
            return slider()->accessibleValueText();
        return QAccessibleWidget::text(t);
    }

    QVariant currentValue() const override { return slider()->value(); }
    void setCurrentValue(const QVariant& value) override
    {
        if (!ControlsLock::isLocked())
            slider()->setValue(value.toInt());
    }
    QVariant maximumValue() const override { return slider()->maximum(); }
    QVariant minimumValue() const override { return slider()->minimum(); }
    QVariant minimumStepSize() const override { return slider()->singleStep(); }

private:
    CenterMarkSlider* slider() const { return static_cast<CenterMarkSlider*>(widget()); }
};

// CenterMarkSlider has no Q_OBJECT, so Qt asks for it under "QSlider":
// match the object itself rather than the key.
inline QAccessibleInterface* centerMarkSliderAccessibleFactory(const QString&, QObject* object)
{
    if (auto* slider = dynamic_cast<CenterMarkSlider*>(object))
        return new CenterMarkSliderAccessible(slider);
    return nullptr;
}
