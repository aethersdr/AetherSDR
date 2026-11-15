#pragma once

#include <QStaticText>
#include <QVector>
#include <QWidget>

class QTimer;

namespace AetherSDR {

class ClientComp;

// Read-only compressor transfer curve (input dB X, output dB Y) from live
// ClientComp parameters, with a ball at the current input envelope. Used in the
// docked applet and as the editor's canvas backdrop; drag handles live in
// ClientCompEditorCanvas. UI thread only; ClientComp's parameter and meter
// reads are atomic, so no extra locking.
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

} // namespace AetherSDR
