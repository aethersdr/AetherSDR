#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class QTimer;

namespace MasterSDR {

class AudioEngine;
class ClientCompEditorCanvas;
class ClientCompKnob;
class ClientCompLimiterButton;
class ClientCompMeter;
class ClientCompThresholdFader;

// Floating editor for the Pro-XL-style TX compressor.  One instance
// lives on MainWindow; calling showForTx() raises the window and
// binds it to AudioEngine::clientCompTx().  Geometry persists via
// AppSettings (`StripCompPanelGeometry` key).
//
// Layout (Ableton-inspired, extended for limiter + chain order):
//   ┌─ bypass │ CMP→EQ | EQ→CMP ──────────────────── × ┐
//   │  ratio  │  [Thresh] [transfer curve]  │GR│Out│L│M│
//   │  attack │                             │  │  │ │a│
//   │  rlease │                             │  │  │ │k│
//   │  knee   │                             │  │  │ │e│
//   └─────────────────────────────────────────────────┘
//
// The canvas (center column) owns the threshold slider + curve + live
// ball.  Meter strips and limiter controls are separate child widgets
// wired to the same ClientComp via signals.
class StripCompPanel : public QWidget {
    Q_OBJECT

public:
    enum class Side { Tx, Rx };

    explicit StripCompPanel(AudioEngine* engine, QWidget* parent = nullptr);
    ~StripCompPanel() override;

    void showForTx();
    void showForRx();

    // Pull every knob / button / label state from the bound engine.
    // Called after preset load when the engine is mutated externally.
    void syncControlsFromEngine();

signals:
    // Fired when the bypass button is toggled inside the editor.  The
    // docked applet subscribes so its Enable toggle stays in sync —
    // both widgets read / write the same ClientComp::enabled flag.
    // side identifies which path (Tx or Rx) was toggled.
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

    // Refresh meter widgets from the latest ClientComp snapshot.
    void tickMeters();

    // Commit a parameter change to the AudioEngine's ClientComp and
    // persist via AppSettings.  All knob/canvas signals land here.
    void applyThreshold(float db);
    void applyRatio(float ratio);
    void applyAttack(float ms);
    void applyRelease(float ms);
    void applyKnee(float db);
    void applyMakeup(float db);
    void applyLimiterEnabled(bool on);
    void applyLimiterCeiling(float db);

    AudioEngine*             m_audio{nullptr};
    Side                     m_side{Side::Tx};
    QWidget*                 m_titleBar{nullptr};   // EditorFramelessTitleBar*
    class ClientComp*        comp() const;
    void                     saveCompSettings() const;
    ClientCompEditorCanvas*  m_canvas{nullptr};
    ClientCompKnob*          m_ratio{nullptr};
    ClientCompKnob*          m_attack{nullptr};
    ClientCompKnob*          m_release{nullptr};
    ClientCompKnob*          m_knee{nullptr};
    ClientCompKnob*          m_makeup{nullptr};
    ClientCompKnob*          m_ceiling{nullptr};
    ClientCompThresholdFader* m_threshFader{nullptr};
    ClientCompMeter*         m_grMeter{nullptr};
    ClientCompMeter*         m_outputMeter{nullptr};
    QPushButton*             m_bypass{nullptr};
    ClientCompLimiterButton* m_limiterEnable{nullptr};
    QTimer*                  m_meterTimer{nullptr};
    bool                     m_restoring{false};
};

} // namespace MasterSDR
