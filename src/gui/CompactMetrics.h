#pragma once

#include <QPointer>
#include <QSize>
#include <QVector>

class QWidget;

namespace AetherSDR {

// Shrinks a panel's graphics without touching its type.
//
// The RX stage panels were drawn for a window twice the size of the one they
// live in now. Scaling the whole panel — a graphics-view transform over the
// lot — fits it, but takes the text down with it, and a 9 px label at 0.6 is
// not a label any more. Almost all of the space those panels spend is not
// text: 76 px knobs, a 180 px tube curve, 42 px meters, an 80 px wordmark.
// Taking those down leaves the labels at the size they were designed to be
// read at.
//
// Originals are captured once, so a factor is always applied to the designed
// size rather than to the last result — applying 0.8 twice would otherwise
// leave a 76 px knob at 49 px rather than 61.
//
// A widget is left alone if it carries text (QLabel, buttons, combos, line
// edits, spin boxes): their size IS their font, and changing it is the one
// thing this must not do. Knobs are in that group too, even though they look
// like pure graphics -- each holds an 11 px value edit across its middle, and
// a knob narrower than 76 px delivers "-40.0 dB" as "0.0 dB". So is any
// container holding text widgets, such as the EQ's per-band readout row:
// its height is three stacked line edits. What gives way instead is the
// stretchy graphics around them: curve views, level meters, scopes, the
// wordmark.
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
