#pragma once

#include <QWidget>

class QTimer;

namespace AetherSDR {

class AudioEngine;
class ClientCompKnob;

// Floating editor for the client-side TX reverb.  Five 76×76 knobs in
// a single row — Size, Decay, Damping, Pre-delay, Mix.  No preset
// picker (small room / hall / plate are a future iteration).
// Geometry persists via AppSettings (`StripReverbPanelGeometry`).
class StripReverbPanel : public QWidget {
    Q_OBJECT

public:
    explicit StripReverbPanel(AudioEngine* engine, QWidget* parent = nullptr);
    ~StripReverbPanel() override;

    void showForTx();

    // Pull every knob / button / label state from the bound engine.
    // Called after preset load when the engine is mutated externally.
    void syncControlsFromEngine();

signals:
    // Present for API symmetry with the other editors; unused now that
    // bypass lives on the CHAIN widget.
    void bypassToggled(bool bypassed);

protected:
    void closeEvent(QCloseEvent* ev) override;
    void moveEvent(QMoveEvent* ev) override;
    void resizeEvent(QResizeEvent* ev) override;
    void showEvent(QShowEvent* ev) override;
    void hideEvent(QHideEvent* ev) override;

private:
    void saveGeometryToSettings();
    void restoreGeometryFromSettings();

    AudioEngine*    m_audio{nullptr};
    ClientCompKnob* m_size{nullptr};
    ClientCompKnob* m_decay{nullptr};
    ClientCompKnob* m_damping{nullptr};
    ClientCompKnob* m_preDly{nullptr};
    ClientCompKnob* m_mix{nullptr};
    QWidget*        m_viz{nullptr};   // GridBox (defined in .cpp)
    bool            m_restoring{false};
};

} // namespace AetherSDR
