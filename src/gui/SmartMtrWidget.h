#pragma once

#include "MeterSmoother.h"
#include "SmartMtrConfig.h"
#include "SmartMtrGeometry.h"

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

class QPainter;

namespace AetherSDR {

// Alternative meter view shown in the VFO flag in place of the standard
// S-meter when the operator selects "SmartMTR" from the meter menu.
//
// This is the first step of a multi-step build. For now it paints only the
// static control body, the recessed "hole", and a single indicator bar pinned
// at SmartMtrUnits::kIndicatorFraction — no signal processing yet.
//
// Organization: design tokens (colors + UNIT proportions) live in
// SmartMtrStyle.h; the UNITS->pixel mapping lives in SmartMtrGeometry.h; this
// class only orchestrates drawing. Each visual element is one small draw method
// taking the geometry, so future elements slot in as new methods + one call in
// paintEvent, in z-order.
class SmartMtrWidget : public QWidget {
    Q_OBJECT
public:
    explicit SmartMtrWidget(QWidget* parent = nullptr);

    // Push what to display. The parent owns the data (kind + value + range);
    // the widget just renders it. Triggers a repaint.
    void setMeterInput(const MeterInput& input);

    // The control fills the full available parent width and keeps its design
    // aspect ratio (kControlW:kControlH) by deriving its height from that width.
    // The VfoWidget meter area is a size-to-current-page stack, so it adopts
    // whatever these report — S-Meter and SmartMTR can therefore differ in
    // height and the flag resizes when switching.
    QSize sizeHint() const override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    // One element per method; all draw in UNITS via SmartMtrGeometry. Called
    // from paintEvent in back-to-front order.
    void drawControl(QPainter& p, const SmartMtrGeometry& g) const;
    void drawHole(QPainter& p, const SmartMtrGeometry& g) const;
    void drawIndicator(QPainter& p, const SmartMtrGeometry& g) const;
    void drawInsetShadow(QPainter& p, const SmartMtrGeometry& g) const;
    void drawMarkers(QPainter& p, const SmartMtrGeometry& g) const;

    // One animation tick: advance the smoother by the elapsed wall-clock, stop
    // the timer once settled, and repaint through the lean-mode gate.
    void advance();

    MeterInput m_input; // what to display; default parks at the signal scale min

    // Ballistics: the indicator chases the mapped target position with the
    // SmartMTR analog feel (fast attack, slow lazy decay). The smoother runs on
    // the normalised scale fraction; drawIndicator denormalises it. Isolated to
    // this widget — the standard S-meters keep their own ballistics.
    MeterSmoother m_smooth;
    QTimer m_animTimer;
    QElapsedTimer m_clock;
    MeterKind m_kind = MeterKind::Signal; // last kind, to snap across scale changes
};

} // namespace AetherSDR
