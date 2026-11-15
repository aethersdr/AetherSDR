#pragma once

#include <QPointer>
#include <QSize>
#include <QVector>

class QWidget;

namespace AetherSDR {

// Shrinks a panel's graphics (curve views, meters, scopes, wordmark)
// without touching text, so labels stay at their designed size. Originals are
// captured once and every factor applies to the designed size, not the last
// result. Widgets that carry text are skipped — labels, buttons, combos, line
// edits, spin boxes, knobs (each holds an 11 px value edit; narrower than 76 px
// clips "-40.0 dB") and containers of text widgets.
class CompactMetrics {
public:
    // Captures every explicitly-sized graphical widget under `root`.
    explicit CompactMetrics(QWidget* root);

    // Re-applies the captured sizes scaled by `factor` (1.0 restores them).
    // Nothing shrinks below kFloorPx in either axis, or below kMinFactor of
    // what it was: past that a knob stops reading as a knob.
    void apply(qreal factor);

    // Whether anything was found worth scaling.
    bool isEmpty() const { return m_entries.isEmpty(); }

    static constexpr int   kFloorPx = 22;
    static constexpr qreal kMinFactor = 0.6;

private:
    struct Entry {
        QPointer<QWidget> widget;
        QSize minimum;
        QSize maximum;
        bool  fixed{false};
        // A meter keeps its width: the scale figures and the value under it
        // are printed across it, so a narrower meter is a meter with "-8.0 dB"
        // showing as "3.0 dB". Its height is pure graphic and gives way.
        bool  heightOnly{false};
    };

    QVector<Entry> m_entries;
};

} // namespace AetherSDR
