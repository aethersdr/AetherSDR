#pragma once

// AetherClock alignment display — the differentiator (PRD reference: the
// received-envelope-vs-expected-template view). A scrolling per-second
// oscillogram of the received envelope with the matched-filter template of
// the DECODED symbol overlaid, plus per-second AM-drop edge markers, a
// classification-confidence lane, and a decoded-symbol glyph lane.
//
// Fed exclusively by ClockAlignmentFrame payloads (one per classified
// second) — no decoder, engine, or radio access. Repaints are driven by
// appendFrame (1 Hz signal → inherently ≤ a few Hz; no timers, no
// per-sample painting).

#include "core/ClockAlignmentFrame.h"

#include <QList>
#include <QWidget>

class QPaintEvent;

namespace AetherSDR {

class ClockAlignmentWidget : public QWidget {
    Q_OBJECT

public:
    explicit ClockAlignmentWidget(QWidget* parent = nullptr);

    QSize sizeHint() const override;         // {240, 150}
    QSize minimumSizeHint() const override;  // {220, 110}

    // Visible history window in seconds (newest on the right). Clamped 5-30.
    int windowSeconds() const { return m_windowSeconds; }
    void setWindowSeconds(int seconds);

public slots:
    // One classified second from the engine's alignmentFrame signal.
    void appendFrame(const AetherSDR::ClockAlignmentFrame& frame);
    // Drop all history (engine stop, slice loss, station change).
    void clear();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QList<ClockAlignmentFrame> m_frames;  // ring, newest last, size ≤ m_windowSeconds
    int m_windowSeconds{15};
};

} // namespace AetherSDR
