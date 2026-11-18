#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QTimer;

namespace AetherSDR {

class AudioEngine;
class ClientCompKnob;    // reused — generic rotary knob
class ClientGateLevelView;

// Floating editor for the AetherRX gate / expander, laid out like Ableton's
// Gate: THR/RET/Flip/Lookahead column on the left, level view on the right,
// ATK/HLD/REL/FLR knobs below. Bound to AudioEngine::clientGateRx(); AetherTX
// has no gate (RN2 replaces it). Geometry persists in AppSettings
// (`ClientGateEditorGeometry`).
class ClientGateEditor : public QWidget {
    Q_OBJECT

public:
    explicit ClientGateEditor(AudioEngine* engine, QWidget* parent = nullptr);
    ~ClientGateEditor() override;

    void showForRx();

signals:
    // Fired when bypass toggles.  Docked applet subscribes to keep
    // its Enable button in sync.
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
    void syncControlsFromEngine();

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
    // The RX gate this page edits.
    class ClientGate* gate() const;
    void              saveGateSettings() const;
    QWidget*              m_titleBar{nullptr};   // EditorFramelessTitleBar*
    ClientGateLevelView*  m_levelView{nullptr};
    ClientCompKnob*       m_threshold{nullptr};
    ClientCompKnob*       m_returnKnob{nullptr};
    ClientCompKnob*       m_ratio{nullptr};
    ClientCompKnob*       m_hold{nullptr};
    ClientCompKnob*       m_release{nullptr};
    ClientCompKnob*       m_floor{nullptr};
    QPushButton*          m_flip{nullptr};
    QComboBox*            m_lookahead{nullptr};
    QPushButton*          m_bypass{nullptr};
    QTimer*               m_syncTimer{nullptr};   // mirror engine → knobs
    bool                  m_restoring{false};
};

} // namespace AetherSDR
