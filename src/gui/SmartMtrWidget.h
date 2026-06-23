#pragma once

#include "MeterExtremes.h"
#include "MeterSmoother.h"
#include "SmartMtrConfig.h"
#include "SmartMtrGeometry.h"

#include <QElapsedTimer>
#include <QPixmap>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWidget>

class QPainter;

namespace AetherSDR {

// Alternative meter view shown in the VFO flag in place of the standard
// S-meter when the operator selects "SmartMTR" from the meter menu.
//
// It renders the meter end-to-end: the static control body and recessed "hole"
// (cached as pixmaps), an indicator bar driven by analog-needle ballistics, and
// optional peak/trough extremes markers with value labels.
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

    // Extremes (min/max peak-hold markers) options. Widget-local mirrors of the
    // DisplaySettings enums so this widget stays free of the settings/AppSettings
    // layer (the parent translates). "speed" sets the sliding-window length;
    // "values" decides whether the numeric value labels are exposed (drawn by the
    // parent's overlay).
    enum class ExtremesSpeed { Slow, Medium, Fast };
    enum class MeterValues { None, Signal, Extremes };
    void setExtremesOptions(bool show, ExtremesSpeed speed, MeterValues values);

    // One min/max marker's data for the parent's value-label overlay. position is
    // a hole-local UNIT center (SmartMtrUnits::kScaleMin..kScaleMax); the parent
    // maps it through the live geometry. Two text lines: primary = S-unit (e.g.
    // "s6"), secondary = dBm (e.g. "-91dBm"). Empty unless extremes + value labels
    // are active and there is data.
    struct ExtremeMarker {
        double position;   // hole-local UNITS
        QString primary;   // top line — S-unit ("s6", "+10dB"); empty for mic
        QString secondary; // bottom line — dBm ("-91dBm") / dB (mic)
        double opacity;    // 0..1
        bool isMax;        // true = peak (right of line), false = trough (left)
    };
    QVector<ExtremeMarker> extremeLabels() const;

Q_SIGNALS:
    // Emitted at the end of each paint so a companion overlay (the extremes value
    // labels, drawn by the parent) can repaint in lockstep with the markers as
    // they animate. The widget itself stays unaware of any overlay.
    void repainted();

public:
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
    void changeEvent(QEvent*) override;

private:
    // One element per method; all draw in UNITS via SmartMtrGeometry. Called
    // from paintEvent in back-to-front order.
    void drawControl(QPainter& p, const SmartMtrGeometry& g) const;
    void drawHole(QPainter& p, const SmartMtrGeometry& g) const;
    void drawIndicator(QPainter& p, const SmartMtrGeometry& g) const;
    void drawInsetShadow(QPainter& p, const SmartMtrGeometry& g) const;
    void drawMarkers(QPainter& p, const SmartMtrGeometry& g) const;
    void drawExtremes(QPainter& p, const SmartMtrGeometry& g) const;

    // Render the static layers (everything except the moving bar and the
    // extremes markers) into m_belowBar / m_aboveBar at the current size, kind
    // and device-pixel ratio. paintEvent rebuilds these only when one of those
    // changes, then blits them each frame — so the per-frame paint during
    // animation skips the gradient fills (inset shadow) and per-tick font work
    // (markers) entirely.
    void rebuildStaticLayers(const SmartMtrGeometry& g);

    // One animation tick: advance the smoother (and the extremes) by the elapsed
    // wall-clock, stop the timer once both settle, and repaint through the
    // lean-mode gate.
    void advance();

    // Apply the bar ballistics for a meter kind: the analog d'Arsonval sag for
    // signal, a snappy PPM attack/decay for mic. Called at construction and on
    // every RX<->TX kind switch (the smoother set differs per domain).
    void applyBallistics(MeterKind kind);

    // Map a raw value (dBm/dBFS) to a clamped hole-local UNIT position, using the
    // current kind's mapping. Mirrors indicatorPosition() for arbitrary values.
    double mapRawToUnits(double raw) const;

    // Live bar position in hole-local UNITS (denormalised smoother value).
    double needlePosUnits() const;

    // Extremes are active (enabled, have data) for the current kind.
    bool extremesActive() const;

    // Shared marker opacity from the fade rules (signal: proximity x signal-fade;
    // mic: always 1). 0 when nothing should draw.
    double extremesOpacity() const;

    // Signal-fade component only (near-floor signals fade out; mic: always 1).
    // Used for the current-signal value label, which has no min/max spread.
    double signalFade() const;

    // Value-label text per kind: S-unit (signal: "s6" / "+10dB"; mic: empty) and
    // the dB(m) line.
    QString extremeSUnit(double raw) const;
    QString extremeDbm(double raw) const;

    MeterInput m_input; // what to display; default parks at the signal scale min

    // Ballistics: the indicator chases the mapped target position with the
    // SmartMTR analog feel (fast attack, slow lazy decay). The smoother runs on
    // the normalised scale fraction; drawIndicator denormalises it. Isolated to
    // this widget — the standard S-meters keep their own ballistics.
    MeterSmoother m_smooth;
    QTimer m_animTimer;
    QElapsedTimer m_clock;
    MeterKind m_kind = MeterKind::Signal; // last kind, to snap across scale changes

    // Extremes (min/max peak-hold markers): a sliding-window envelope tracker that
    // glides at a constant linear slew (distinct from the bar's exponential
    // ballistic). Ticked alongside the smoother in advance(). Free-running clock
    // is separate from m_clock so the smoother's dt accounting is undisturbed.
    MeterExtremes m_extremes;
    QElapsedTimer m_extremesClock;
    bool m_extremesEnabled = false;
    MeterValues m_showValues = MeterValues::None;

    // Repaint cadence for a returning marker, independent of the bar's lean
    // repaint gate (which throttles to 12 Hz and would step the slow glide).
    // Timestamp (on m_extremesClock) of the last marker-driven repaint; -1 until
    // the first. See SmartMtrExtremes::kExtremesRepaintHz.
    qint64 m_lastExtremesRepaintMs = -1;

    // Cached static layers, split around the moving indicator bar (the inset
    // shadow must overlay the bar, so it can't share one pixmap with the body
    // below it): m_belowBar = control body + recessed hole; m_aboveBar = inset
    // shadow + scale markers/labels. Rebuilt only when the cache key below
    // changes; blitted every frame. See rebuildStaticLayers().
    QPixmap m_belowBar;
    QPixmap m_aboveBar;
    QSize m_cacheSize;                       // logical widget size of the cache
    qreal m_cacheDpr = 0.0;                  // device-pixel ratio of the cache
    MeterKind m_cacheKind = MeterKind::Signal; // kind the markers were built for
    bool m_cacheValid = false;
};

} // namespace AetherSDR
