#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QStackedWidget;
class QTimer;

namespace AetherSDR {

class AudioEngine;
class ClientCompKnob;    // reused — generic rotary knob
class ClientGateLevelView;
class ClientGateCurveWidget;

// Floating editor for the client-side TX gate / expander — layout
// modelled on Ableton Live's Gate device.  One instance lives on
// MainWindow; calling showForTx() raises the window and binds it to
// AudioEngine::clientGateTx().  Geometry persists via AppSettings
// (`StripGatePanelGeometry` key).
//
// Layout:
//   ┌─ bypass ──────────────────── × ┐
//   │ ┌──────┐  ┌──────────────────┐ │
//   │ │ THR  │  │                  │ │
//   │ │      │  │   level view     │ │
//   │ │ RET  │  │   (Ableton-style)│ │
//   │ │      │  │                  │ │
//   │ │ Flip │  │                  │ │
//   │ │ Look │  │                  │ │
//   │ └──────┘  └──────────────────┘ │
//   │   [ATK] [HLD] [REL] [FLR]      │
//   └────────────────────────────────┘
class StripGatePanel : public QWidget {
    Q_OBJECT

public:
    enum class Side { Tx, Rx };

    explicit StripGatePanel(AudioEngine* engine, QWidget* parent = nullptr);
    ~StripGatePanel() override;

    void showForTx();
    void showForRx();

    // Pull every knob / button / label state from the bound engine.
    // Called after preset load when the engine is mutated externally.
    void syncControlsFromEngine();

signals:
    // Fired when bypass toggles.  Docked applet subscribes to keep
    // its Enable button in sync.  side identifies which path (Tx or
    // Rx) was toggled — both share the editor instance.
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

    // Parameter commit — all control signals land here; each writes
    // the engine + persists via AppSettings.
    void applyThreshold(float db);
    void applyReturn(float db);
    void applyRatio(float ratio);
    void applyHold(float ms);
    void applyRelease(float ms);
    void applyFloor(float db);
    void applyLookahead(float ms);
    void applyMode(int modeIdx);   // 0=Expander, 1=Gate

    AudioEngine*          m_audio{nullptr};
    Side                  m_side{Side::Tx};
    // Side-aware accessor — picks Tx or Rx instance based on m_side.
    // Saves dispatch logic at every parameter setter.
    class ClientGate* gate() const;
    void              saveGateSettings() const;
    QWidget*              m_titleBar{nullptr};   // EditorFramelessTitleBar*
    ClientGateLevelView*    m_levelView{nullptr};
    ClientGateCurveWidget*  m_curveView{nullptr};
    QStackedWidget*         m_viewStack{nullptr};
    // Level / Curve, and Gate / Expander: four buttons, both pairs always on
    // screen. A single button that renamed itself said what you would get if
    // you pressed it, which is the opposite of what a control's label should
    // say, and left the other state invisible until you did.
    QPushButton*            m_viewLevelBtn{nullptr};
    QPushButton*            m_viewCurveBtn{nullptr};
    ClientCompKnob*       m_threshold{nullptr};
    ClientCompKnob*       m_returnKnob{nullptr};
    ClientCompKnob*       m_ratio{nullptr};
    ClientCompKnob*       m_hold{nullptr};
    ClientCompKnob*       m_release{nullptr};
    ClientCompKnob*       m_floor{nullptr};
    // TX mic pre-amp RN2 toggle.  It denoises the mic ahead of every
    // chain stage, so it lives on the gate — the default head of the
    // chain — rather than on the tube at the far end of it.  Created
    // hidden; only showForTx() reveals it, because RX has its own RN2
    // toggle elsewhere.  (#2813)
    QPushButton*          m_rn2Btn{nullptr};
    QPushButton*          m_gateBtn{nullptr};
    QPushButton*          m_expanderBtn{nullptr};
    QSlider*              m_lookahead{nullptr};
    QLabel*               m_lookaheadValue{nullptr};
    QPushButton*          m_bypass{nullptr};
    QTimer*               m_syncTimer{nullptr};   // mirror engine → knobs
    bool                  m_restoring{false};
};

} // namespace AetherSDR
