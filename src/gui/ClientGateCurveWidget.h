#pragma once

#include <QStaticText>
#include <QVector>
#include <QWidget>

class QTimer;

namespace MasterSDR {

class ClientGate;

// Read-only transfer curve for the client-side gate / expander.  Draws
// the static gain curve — unity above threshold, sloped attenuation
// below threshold clamped to `floor` — with a glowing ball sliding
// along the curve at the current input envelope level.  Visual
// language mirrors ClientCompCurveWidget so the two tiles read as a
// family.
//
// Thread model: ClientGate parameter + meter reads are already atomic
// internally, so paintEvent + the polling QTimer are both safe on the
// UI thread without extra locking.
class ClientGateCurveWidget : public QWidget {
    Q_OBJECT

public:
    explicit ClientGateCurveWidget(QWidget* parent = nullptr);

    void setGate(ClientGate* gate);
    ClientGate* gate() const { return m_gate; }

    // Compact mode: thinner gridlines, no axis labels.  On in the
    // docked applet, off in the editor canvas.
    void setCompactMode(bool on);

    static constexpr float kMinDb = -80.0f;
    static constexpr float kMaxDb =   0.0f;

protected:
    void paintEvent(QPaintEvent* ev) override;

    float dbToX(float db) const;
    float dbToY(float db) const;

    // Static curve: output level in dB for a given input level in dB,
    // using the current threshold / ratio / floor / return from m_gate.
    float curveOutputDb(float inDb) const;

    void drawGrid(QPainter& p, const QRectF& rect) const;
    void drawHysteresisBand(QPainter& p, const QRectF& rect) const;
    void drawCurve(QPainter& p, const QRectF& rect) const;
    void drawBall(QPainter& p, const QRectF& rect) const;

    QTimer*     m_pollTimer{nullptr};
    ClientGate* m_gate{nullptr};
    bool        m_compact{false};
    float       m_lastInputDb{-120.0f};

    // Cached axis labels — one QStaticText per kMajorTicks entry.  See
    // ClientCompCurveWidget for the same pattern; mutable because
    // drawGrid() is const but the cache is hidden state.
    mutable QVector<QStaticText> m_axisLabels;
    mutable bool                 m_labelsDirty{true};
};

} // namespace MasterSDR
