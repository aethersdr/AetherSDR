#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class QSlider;
class QTimer;

namespace AetherSDR {

class AudioEngine;
class ClientCompEditorCanvas;
class ClientCompKnob;
class ClientCompLimiterButton;
class ClientCompMeter;
class ClientCompThresholdFader;

// Floating editor for the TX compressor; showForTx() binds it to
// AudioEngine::clientCompTx(). Geometry persists in `StripCompPanelGeometry`.
// Layout like the gate editor: limiter switches on top, threshold canvas
// (chevron, curve, live ball) with GR/Out meters, knobs in one foot row.
// Makeup is a fader on the Out meter (-12 dB to +24 dB, 0 dB detent).
// Drive and Phase (#2887) are TX-only PAPR conditioning; showForRx() hides them
// and forces both to bypass on the RX ClientComp.
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

    // Drive the toolbar's ceiling slider + its reading from a dB value.
    void setCeilingDb(float db);

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
    void applyDrive(float db);
    void applyPhase(float stages);

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
    QSlider*                 m_ceiling{nullptr};
    QLabel*                  m_ceilingValue{nullptr};
    ClientCompKnob*          m_drive{nullptr};
    ClientCompKnob*          m_phase{nullptr};
    ClientCompThresholdFader* m_threshFader{nullptr};
    ClientCompMeter*         m_grMeter{nullptr};
    ClientCompMeter*         m_outputMeter{nullptr};
    QPushButton*             m_bypass{nullptr};
    ClientCompLimiterButton* m_limiterEnable{nullptr};
    QTimer*                  m_meterTimer{nullptr};
    bool                     m_restoring{false};
};

} // namespace AetherSDR
