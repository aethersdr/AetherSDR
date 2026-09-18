#pragma once

#include "PanelTick.h"

#include <QColor>
#include <QTimer>
#include <QVector>
#include <QWidget>

namespace AetherSDR {

// The scrolling noise-reduction trace in the AetherDSP status strip.
//
// A cursor sweeps across the strip and each step plots the gain the active
// method is currently applying, as AudioEngine::nrGainChanged reports it: the
// trace rides high where the reduction is passing signal through (speech) and
// drops into the floor where it is suppressing (the gaps between syllables).
// Speech-height steps draw bright, floor steps dim, so the shape of the
// channel reads at a glance without anyone having to interpret a number.
//
// The sweep runs on its own timer rather than on the audio blocks so the
// horizontal scale stays constant whatever rate the source delivers at; the
// timer samples whatever value was last pushed. With no method running the
// strip draws a flat idle baseline, which is deliberately distinct from a
// method that happens to be passing everything (gain 1.0 rides at the top).
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
