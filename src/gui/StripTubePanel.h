#pragma once

#include <QWidget>

class QPushButton;
class QButtonGroup;
class QTimer;

namespace MasterSDR {

class AudioEngine;
class ClientCompKnob;         // reused — generic rotary knob
class ClientLevelMeter;
class ClientTubeCurveWidget;

// Floating editor for the client-side dynamic tube saturator.
// Layout mirrors Ableton's Dynamic Tube device:
//
//   ┌─ bypass ──────────────────── × ┐
//   │ ┌──────┐ ┌─────────┐ ┌──────┐ │
//   │ │DryWet│ │  curve  │ │ ENV  │ │
//   │ │      │ │         │ │      │ │
//   │ │ Out  │ │         │ │ ATK  │ │
//   │ │      │ │ [A B C] │ │      │ │
//   │ │Drive │ │  Tone   │ │ REL  │ │
//   │ │      │ │  Bias   │ │      │ │
//   │ └──────┘ └─────────┘ └──────┘ │
//   └────────────────────────────────┘
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
    void applyAttack(float ms);
    void applyRelease(float ms);

    AudioEngine*           m_audio{nullptr};
    Side                   m_side{Side::Tx};
    QWidget*               m_titleBar{nullptr};   // EditorFramelessTitleBar*
    class ClientTube*      tube() const;
    void                   saveTubeSettings() const;
    ClientTubeCurveWidget* m_curve{nullptr};
    ClientCompKnob*        m_dryWet{nullptr};
    ClientCompKnob*        m_output{nullptr};
    ClientCompKnob*        m_drive{nullptr};
    ClientCompKnob*        m_tone{nullptr};
    ClientCompKnob*        m_bias{nullptr};
    ClientCompKnob*        m_envelope{nullptr};
    ClientCompKnob*        m_attack{nullptr};
    ClientCompKnob*        m_release{nullptr};
    ClientLevelMeter*      m_outMeter{nullptr};
    // TX mic pre-amp RN2 toggle.  Created in ctor as hidden; flipped
    // visible (and m_outMeter is shortened) only when showForTx() runs.
    // RX side keeps the full-height meter and no RN2 control here —
    // RX already has its own RN2 toggle elsewhere.  (#2813)
    QPushButton*           m_rn2Btn{nullptr};
    QPushButton*           m_modelA{nullptr};
    QPushButton*           m_modelB{nullptr};
    QPushButton*           m_modelC{nullptr};
    QButtonGroup*          m_modelGroup{nullptr};
    QPushButton*           m_bypass{nullptr};
    QTimer*                m_syncTimer{nullptr};   // mirror engine → knobs
    bool                   m_restoring{false};
};

} // namespace MasterSDR
