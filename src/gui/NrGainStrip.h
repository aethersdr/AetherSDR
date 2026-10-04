#pragma once

#include "PanelTick.h"

#include <QColor>
#include <QTimer>
#include <QVector>
#include <QWidget>

namespace AetherSDR {

// Scrolling NR trace in the AetherDSP status strip: a cursor sweeps and plots
// the active method's gain (AudioEngine::nrGainChanged), bright at speech
// height, dim at the floor. The sweep has its own timer so the horizontal scale
// is constant; it samples the last pushed value. No method → flat idle
// baseline, distinct from gain 1.0 at the top.
class NrGainStrip final : public QWidget {
    Q_OBJECT

public:
    explicit NrGainStrip(QWidget* parent = nullptr);

    // Latest reading from the engine. `active` false means no method is
    // engaged (or the chain is bypassed for TX).
    void setGain(float gain, bool active);

    // Drop the history — used when the operator switches method, so the new
    // method's trace is not read as a continuation of the old one's.
    void reset();

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void resizeBuffers(int width);
    void advance();

    static constexpr int kFrameMs = kPanelTickMs;
    // Below this the step is drawn as floor rather than signal. Chosen to sit
    // under the gain a spectral method leaves on speech and above the gain it
    // leaves on the noise between syllables.
    static constexpr float kSignalGain = 0.55f;

    QTimer          m_sweep;
    QVector<float>  m_values;
    QVector<quint8> m_signal;
    int             m_cursor{0};
    float           m_pending{1.0f};
    bool            m_active{false};
};

} // namespace AetherSDR
