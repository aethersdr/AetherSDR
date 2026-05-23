#pragma once

#include <QStaticText>
#include <QVector>
#include <QWidget>

class QTimer;

namespace MasterSDR {

class ClientComp;

// Read-only transfer curve for the Pro-XL-style compressor.  Draws the
// static gain curve (input dB on X, output dB on Y) from the live
// ClientComp parameters, with a glowing "ball" sliding along the curve
// at the current input envelope level so you can see the compressor
// working in real time.
//
// Used in two places: inside the docked applet as a compact dashboard,
// and inside the floating editor as the canvas backdrop.  Interactive
// threshold-drag + ratio-handle behaviour lives in the subclass
// ClientCompEditorCanvas, not here.
//
// Thread model: the bound ClientComp is read on the UI thread only.
// Parameter reads and meter reads are already atomic internally, so
// paintEvent + the meter-polling QTimer are both safe from the UI
// thread without any additional locking.
class ClientCompCurveWidget : public QWidget {
    Q_OBJECT

public:
    explicit ClientCompCurveWidget(QWidget* parent = nullptr);

    // Null is allowed — widget draws the grid with no curve or ball.
    void setComp(ClientComp* comp);
    ClientComp* comp() const { return m_comp; }

    // Compact mode: thinner gridlines, no axis labels.  On in the
    // docked applet, off in the editor canvas.
    void setCompactMode(bool on);

    // X / Y extents of the displayed range.  Fixed — compressors are
    // always drawn on an absolute dBFS grid.
    static constexpr float kMinDb =  -60.0f;
    static constexpr float kMaxDb =    0.0f;

protected:
    void paintEvent(QPaintEvent* ev) override;

    // Map dBFS <-> pixel inside the drawing rect.
    float dbToX(float db) const;
    float dbToY(float db) const;
    float xToDb(float x) const;
    float yToDb(float y) const;

    // Shared static-curve math — returns the output level in dB for a
    // given input level in dB, using the current threshold / ratio /
    // knee from m_comp.  Mirrors ClientComp::staticCurveGainDb() but
    // evaluated in the output-level domain so the curve can be traced
    // straight onto the grid.
    float curveOutputDb(float inDb) const;

    // Draws the static transfer curve and the live ball on top.  Split
    // out so the interactive subclass can call the same drawing pass
    // then overlay its handles.
    void drawGrid(QPainter& p, const QRectF& rect) const;
    void drawCurve(QPainter& p, const QRectF& rect) const;
    void drawBall(QPainter& p, const QRectF& rect) const;

    // Polling timer that updates the ball position from the latest
    // ClientComp::inputPeakDb() at ~30 Hz so the motion is smooth but
    // cheap.  Started in setComp() when a non-null comp is bound.
    QTimer*     m_pollTimer{nullptr};

    ClientComp* m_comp{nullptr};
    bool        m_compact{false};
    float       m_lastInputDb{-120.0f};   // smoothed ball position

    // Cached axis labels — one QStaticText per kMajorTicks entry so the
    // HarfBuzz shaper runs once per (string, font) pair instead of every
    // 33 ms paint.  Mutable because drawGrid() is const but the cache is
    // hidden state, not part of the widget's logical model.  Rebuilt on
    // first paint and on compact-mode flip (font px size changes 7 ↔ 9).
    mutable QVector<QStaticText> m_axisLabels;
    mutable bool                 m_labelsDirty{true};
};

} // namespace MasterSDR
