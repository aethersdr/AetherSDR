#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class QButtonGroup;
class QSlider;
class QTimer;

namespace AetherSDR {

class AudioEngine;
class ClientCompKnob;         // reused — generic rotary knob
class ClientLevelMeter;
class ClientTubeCurveWidget;

// Floating editor for the client-side dynamic tube saturator.
// Laid out like the gate and compressor editors: a toolbar of switches
// along the top, the display filling everything under it, and every knob
// in one row at the foot.
//
//   ┌─ Model: [A][B][C] ─────────── Dry/Wet: ──●── 100 % ─┐
//   │                                              │ OUT │
//   │              transfer curve                  │     │
//   ├──────────────────────────────────────────────┴─────┤
//   │ (Drive)(Tone)(Bias)(Envelope)(Attack)(Release)(Out) │
//   └────────────────────────────────────────────────────┘
class StripTubePanel : public QWidget {
    Q_OBJECT

public:
    enum class Side { Tx, Rx };

    explicit StripTubePanel(AudioEngine* engine, QWidget* parent = nullptr);
    ~StripTubePanel() override;

    void showForTx();
    void showForRx();

    // Pull every knob / button / label state from the bound engine.
    // Called after preset load when the engine is mutated externally.
    void syncControlsFromEngine();

signals:
    void bypassToggled(Side side, bool bypassed);

protected:
    void closeEvent(QCloseEvent* ev) override;
    void moveEvent(QMoveEvent* ev) override;
    void resizeEvent(QResizeEvent* ev) override;
    void showEvent(QShowEvent* ev) override;
    void hideEvent(QHideEvent* ev) override;

private:
    void saveGeometryToSettings();
    void restoreGeometryFromSettings();

    void applyModel(int idx);   // 0=A, 1=B, 2=C
    void applyDrive(float db);
    void applyBias(float v);
    void applyTone(float v);
    void applyOutput(float db);
    void applyDryWet(float v);
    void applyEnvelope(float v);
    void applyRelease(float ms);

    // Drive the toolbar's dry/wet slider + its reading from a 0..1 mix.
    void setDryWetMix(float mix);

    AudioEngine*           m_audio{nullptr};
    Side                   m_side{Side::Tx};
    QWidget*               m_titleBar{nullptr};   // EditorFramelessTitleBar*
    class ClientTube*      tube() const;
    void                   saveTubeSettings() const;
    ClientTubeCurveWidget* m_curve{nullptr};
    QSlider*               m_dryWet{nullptr};
    QLabel*                m_dryWetValue{nullptr};
    ClientCompKnob*        m_output{nullptr};
    ClientCompKnob*        m_drive{nullptr};
    ClientCompKnob*        m_tone{nullptr};
    ClientCompKnob*        m_bias{nullptr};
    ClientCompKnob*        m_envelope{nullptr};
    ClientCompKnob*        m_release{nullptr};
    ClientLevelMeter*      m_outMeter{nullptr};
    QPushButton*           m_modelA{nullptr};
    QPushButton*           m_modelB{nullptr};
    QPushButton*           m_modelC{nullptr};
    QButtonGroup*          m_modelGroup{nullptr};
    QPushButton*           m_bypass{nullptr};
    QTimer*                m_syncTimer{nullptr};   // mirror engine → knobs
    bool                   m_restoring{false};
};

} // namespace AetherSDR
