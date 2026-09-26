#pragma once

#include "ClientEqApplet.h"   // ClientEqApplet::Path enum
#include "core/AudioEngine.h" // AudioEngine::TxChainStage in signal sig

#include <QWidget>

class QPushButton;
class QComboBox;
class QLabel;
class QStackedWidget;
class QTimer;
class QVBoxLayout;

namespace AetherSDR {


class AudioEngine;
class StageTabBar;
class EditorFramelessTitleBar;
class StripTubePanel;
class StripDeEssPanel;
class StripGatePanel;
class StripEqPanel;
class StripCompPanel;
class StripPuduPanel;
class StripReverbPanel;
class StripWaveformPanel;
class StripFinalOutputPanel;

// AetherTX — the transmit chain in one window.
//
// First-iteration plumbing for issue #2301.  Toplevel `Qt::Window`
// that embeds all 7 client-side TX DSP stage panels in a single
// view, with a horizontal `ClientChainWidget` at the top for chain
// ordering / bypass.  Per-stage editors and applets continue to
// work alongside this window during iteration; step 6 of the plan
// removes them.
//
// Geometry persists via AppSettings("AetherialStripGeometry").
// Visibility persists via AppSettings("AetherialStripVisible") so
// the strip reopens at last position on startup.
class AetherialAudioStrip : public QWidget {
    Q_OBJECT

public:
    // The tabs, in the order they appear and in the order the signal meets
    // them. Used as a stack index, so entries are appended, never inserted.
    enum Stage { Gate = 0, Eq, DeEss, Comp, Tube, Enh, Reverb, Output,
                 StageCount };

    explicit AetherialAudioStrip(AudioEngine* engine, QWidget* parent = nullptr);
    ~AetherialAudioStrip() override;

    void setFramelessMode(bool on);
    void setAudioPathNotice(const QString& text, bool warning);

    // Forward radio TX filter cutoffs to the embedded EQ canvas so the
    // dashed yellow filter-edge guide lines render here too.  MainWindow
    // calls this from its txFilterCutoffChanged subscription, the same
    // way it does for the floating ClientEqEditor.
    void setTxFilterCutoffs(int lowHz, int highHz);

    // Mirrored from ClientChainApplet so the strip's own record / play
    // buttons share the PUDU monitor in MainWindow.
    void setMonitorRecording(bool on);
    void setMonitorPlaying(bool on);
    void setMonitorHasRecording(bool has);


    // MIC goes green when the PC mic is selected and DAX is off (i.e. this
    // chain is actually in the TX signal path).  TX lights while the user is
    // transmitting on their own slice.  Both drive the indicators at the foot
    // of the stage column.
    void setMicInputReady(bool ready);
    void setTxActive(bool active);

    // Pull the stage column back from engine state — used by MainWindow when
    // the docked Chain applet toggles a stage, so the two surfaces agree.
    // Engine state is the source of truth; this just re-reads it.
    void refreshChainPaint();

    // Accessor for the embedded Final Output panel — MainWindow wires
    // this to TransmitModel::quindarActiveChanged so the QUIN chip
    // flashes via signal hop instead of a poll.
    StripFinalOutputPanel* finalOutputPanel() const { return m_finalOutput; }

signals:
    // Re-emitted from the embedded StripEqPanel when the user drags one
    // of the dashed cutoff lines.  MainWindow connects this to the same
    // handler as ClientEqEditor::cutoffsDragRequested so dragging in the
    // strip writes the same TX filter command to the radio.
    void cutoffsDragRequested(ClientEqApplet::Path path,
                              int audioLowHz, int audioHighHz);

    // Record / playback button clicks — same semantics as
    // ClientChainApplet::monitorRecordClicked / monitorPlayClicked.
    void monitorRecordClicked();
    void monitorPlayClicked();


    // Raised from setStageEnabled(), which the stage column's per-row
    // checkbox drives through its host callback.  MainWindow routes this to
    // the same handler as ClientChainApplet's signal so the docked Chain
    // applet repaints in lock-step.  (It used to come from an embedded
    // StripChainWidget; this window has no chain widget in it any more,
    // though the docked applet still uses that class.)
    void stageEnabledChanged(AudioEngine::TxChainStage stage, bool enabled);

protected:
    void closeEvent(QCloseEvent* ev) override;
    void moveEvent(QMoveEvent* ev) override;
    void resizeEvent(QResizeEvent* ev) override;
    void showEvent(QShowEvent* ev) override;
    void hideEvent(QHideEvent* ev) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void refreshIndicators();

    // The REC / PLAY pair on one row at the foot of the stage column, above
    // BYPASS. Owned by the StageTabBar; lit by MainWindow through the
    // setMonitor* setters.
    QPushButton* m_monRecBtn{nullptr};
    QPushButton* m_monPlayBtn{nullptr};

    QLabel* m_micDot{nullptr};
    QLabel* m_pcAudioNotice{nullptr};
    QLabel* m_micLabel{nullptr};
    QLabel* m_txDot{nullptr};
    QLabel* m_txLabel{nullptr};
    bool    m_micReady{false};
    bool    m_txActive{false};

    void saveGeometryToSettings();
    void restoreGeometryFromSettings();

    // Master bypass — snapshot all enabled TX stages, then disable
    // them.  Restores the snapshot on uncheck.  Mirrors the docked
    // ClientChainApplet's BYPASS button.
    void onBypassToggled(bool checked);
    // The BYPASS toggle at the foot of the stage column, beside the Settings
    // gear. Owned by the StageTabBar; follows AudioEngine::txBypassChanged.
    QPushButton* m_bypassBtn{nullptr};

    void addStage(Stage stage, const QString& label, QWidget* page);

    // Commit a checkbox to the engine, and read the engine back into one.
    void setStageEnabled(Stage stage, bool on);
    bool stageEnabled(Stage stage) const;

    // The profile library.
    void showSettings();

    // After a profile has been applied to the engine, push fresh values
    // into every embedded panel's UI so labels / knobs / combos stop
    // showing the previous preset's data.
    void refreshAllPanelsFromEngine();

    AudioEngine*         m_audio{nullptr};
    QWidget*             m_titleBar{nullptr};   // custom inline ContainerTitleBar-styled bar
    QVBoxLayout*         m_bodyLayout{nullptr};
    QLabel*              m_titleLbl{nullptr};   // title text — toggles "— TX" / "— RX" suffix
    StageTabBar*         m_tabs{nullptr};
    QStackedWidget*      m_stack{nullptr};
    // Polls the engine so the enable boxes follow changes made elsewhere —
    // the docked chain applet toggles the same flags.
    QTimer*              m_checkTimer{nullptr};
    bool                 m_buildingCombo{false};
    StripTubePanel*    m_tube{nullptr};
    StripGatePanel*    m_gate{nullptr};
    StripEqPanel*      m_eq{nullptr};
    StripCompPanel*    m_comp{nullptr};
    StripDeEssPanel*   m_dess{nullptr};
    StripPuduPanel*    m_pudu{nullptr};
    StripReverbPanel*  m_reverb{nullptr};
    StripWaveformPanel*    m_waveform{nullptr};
    StripFinalOutputPanel* m_finalOutput{nullptr};
    // RX panel instances (#2425).  Same Strip*Panel classes as the TX
    // grid above, but each one is pinned to its RX side via showForRx
    // / showForPath(Rx) and bound to the engine's RX DSP instances.
    bool               m_restoring{false};
};

} // namespace AetherSDR
