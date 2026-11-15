#include "CompactMetrics.h"

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QLabel>
#include <QMetaObject>
#include <QString>
#include <QLineEdit>
#include <QWidget>

#include <algorithm>

namespace AetherSDR {

namespace {

// Carries text, so its size is its font. Never touched.
bool isTextWidget(const QWidget* w)
{
    return qobject_cast<const QLabel*>(w) || qobject_cast<const QAbstractButton*>(w)
        || qobject_cast<const QComboBox*>(w) || qobject_cast<const QLineEdit*>(w)
        || qobject_cast<const QAbstractSpinBox*>(w);
}

bool isKnob(const QWidget* w);

// A container whose children are text widgets is a text row, whatever its own
// class says. Its height is three stacked line edits, not a graphic, and
// taking 40% off it clips them -- which is how the EQ's per-band readouts
// ended up with their middle line sliced through.
bool holdsText(const QWidget* w)
{
    const auto children = w->findChildren<QWidget*>();
    return std::any_of(children.begin(), children.end(),
                       [](const QWidget* c) { return isTextWidget(c); });
}

bool hasExplicitWidth(const QWidget* w)
{
    return w->minimumWidth() > 0 || w->maximumWidth() < QWIDGETSIZE_MAX;
}

bool hasExplicitHeight(const QWidget* w)
{
    return w->minimumHeight() > 0 || w->maximumHeight() < QWIDGETSIZE_MAX;
}

int scaled(int value, qreal factor, int floorPx)
{
    if (value <= 0 || value >= QWIDGETSIZE_MAX) return value;
    // Never grows something past its designed size, and never below the floor
    // -- unless it started below it, in which case leave it be.
    return std::clamp(qRound(value * factor), std::min(value, floorPx), value);
}

// Recognised by class name rather than by a base class: knobs live in several
// unrelated widget families (ClientCompKnob is the common one), and what they
// share is the value edit across the middle, not an inheritance.
bool isKnob(const QWidget* w)
{
    return QString::fromLatin1(w->metaObject()->className()).contains(
        QLatin1String("Knob"), Qt::CaseInsensitive);
}

// Same reasoning as a knob, one axis at a time: a meter prints its scale and
// its reading across its width.
bool isMeter(const QWidget* w)
{
    return QString::fromLatin1(w->metaObject()->className()).contains(
        QLatin1String("Meter"), Qt::CaseInsensitive);
}

} // namespace

CompactMetrics::CompactMetrics(QWidget* root)
{
    if (!root) return;
    const auto widgets = root->findChildren<QWidget*>();
    for (QWidget* w : widgets) {
        if (isTextWidget(w) || isKnob(w) || holdsText(w)) continue;
        if (!hasExplicitWidth(w) && !hasExplicitHeight(w)) continue;

        Entry entry;
        entry.widget = w;
        entry.minimum = w->minimumSize();
        entry.maximum = w->maximumSize();
        entry.fixed = (entry.minimum == entry.maximum) && !entry.minimum.isEmpty();
        entry.heightOnly = isMeter(w);
        m_entries.push_back(entry);
    }
}

void CompactMetrics::apply(qreal factor)
{
    const qreal f = std::clamp(factor, kMinFactor, qreal(1.0));
    for (const Entry& entry : m_entries) {
        QWidget* w = entry.widget;
        if (!w) continue;

        if (entry.fixed) {
            w->setFixedSize(entry.heightOnly ? entry.minimum.width()
                                             : scaled(entry.minimum.width(), f, kFloorPx),
                            scaled(entry.minimum.height(), f, kFloorPx));
            continue;
        }
        // Minimums shrink; maximums stay, so a widget that was free to grow
        // still is.
        if (entry.minimum.width() > 0 && !entry.heightOnly) {
            w->setMinimumWidth(scaled(entry.minimum.width(), f, kFloorPx));
        }
        if (entry.minimum.height() > 0) {
            w->setMinimumHeight(scaled(entry.minimum.height(), f, kFloorPx));
        }
        if (entry.maximum.width() < QWIDGETSIZE_MAX && !entry.heightOnly) {
            w->setMaximumWidth(std::max(w->minimumWidth(),
                                        scaled(entry.maximum.width(), f, kFloorPx)));
        }
        if (entry.maximum.height() < QWIDGETSIZE_MAX) {
            w->setMaximumHeight(std::max(w->minimumHeight(),
                                         scaled(entry.maximum.height(), f, kFloorPx)));
        }
    }
}

} // namespace AetherSDR
