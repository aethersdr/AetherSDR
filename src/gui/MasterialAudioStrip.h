#pragma once

#include "ClientEqApplet.h"   // ClientEqApplet::Path enum
#include "core/AudioEngine.h" // AudioEngine::TxChainStage in signal sig

#include <QWidget>

class QPushButton;
class QComboBox;
class QLabel;
class QStackedWidget;
class QVBoxLayout;

namespace MasterSDR {

class MasterDspWidget;
class AudioEngine;
class ChannelStripPresets;
class StripChainWidget;
class StripRxChainWidget;
class EditorFramelessTitleBar;
class StripTubePanel;
class StripGatePanel;
class StripEqPanel;
class StripCompPanel;
class StripDeEssPanel;
class StripPuduPanel;
class StripReverbPanel;
class StripWaveformPanel;
class StripFinalOutputPanel;

// Masterial Audio Channel Strip — unified TX DSP window.
//
// First-iteration plumbing for issue #2301.  Toplevel `Qt::Window`
// that embeds all 7 client-side TX DSP stage panels in a single
// view, with a horizontal `ClientChainWidget` at the top for chain
// ordering / bypass.  Per-stage editors and applets continue to
// work alongside this window during iteration; step 6 of the plan
// removes them.
//
// Geometry persists via AppSettings("MasterialStripGeometry").
// Visibility persists via AppSettings("MasterialStripVisible") so
// the strip reopens at last position on startup.
class MasterialAudioStrip : public QWidget {
    Q_OBJECT

public:
    explicit MasterialAudioStrip(AudioEngine* engine, QWidget* parent = nullptr);
    ~MasterialAudioStrip() override;

    void setFramelessMode(bool on);

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


    // MIC endpoint goes green when PC mic is selected and DAX is off
    // (i.e. PooDoo is actually in the TX signal path).  TX endpoint
    // pulses red while the user is transmitting on their own slice.
    // Both forward to the embedded StripChainWidget.
    void setMicInputReady(bool ready);
    void setTxActive(bool active);

    // RX-side status forwarders — feed the embedded StripRxChainWidget's
    // RADIO / ADSP / SPEAK status tiles.  Mirror the docked applet's
    // setRxPcAudioEnabled / setRxClientDspActive / setRxOutputUnmuted.
    void setRxPcAudioEnabled(bool on);
    void setRxClientDspActive(bool on, const QString& label = QString());
    void setRxOutputUnmuted(bool on);

    // Repaint the embedded StripChainWidget — used by MainWindow when
    // the docked Chain applet toggles a stage so the strip's tile
    // visuals stay in sync.  Engine state is the source of truth; this
    // just nudges the widget to repaint from it.
    void refreshChainPaint();

    // Accessor for the embedded Final Output panel — MainWindow wires
    // this to TransmitModel::quindarActiveChanged so the QUIN chip
    // flashes via signal hop instead of a poll.
    StripFinalOutputPanel* finalOutputPanel() const { return m_finalOutput; }

    // Accessor for the embedded RX ADSP widget so MainWindow can call
    // wireMasterDspWidget() on it.  Without that wiring, NR2/NR4/DFNR/
    // BNR/MNR controls emit signals into the void — the MasterDspWidget
    // doesn't talk to the engine directly; every parameter change goes
    // through MainWindow's wire-up.  Same lifecycle as the dialog and
    // docked applet paths.
    MasterDspWidget* adspWidget() const { return m_adspRx; }

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


    // Re-emitted from the embedded StripChainWidget when the user
    // single-clicks a stage tile to toggle its bypass.  MainWindow
    // routes this to the same handler as ClientChainApplet's signal
    // so the docked Chain applet's chain widget repaints in lock-step.
    void stageEnabledChanged(AudioEngine::TxChainStage stage, bool enabled);

    // RX-side equivalent of stageEnabledChanged.  MainWindow routes
    // this to the docked ClientRxChainWidget so its visual state
    // matches the strip's RX chain (#2425).
    void rxStageEnabledChanged(AudioEngine::RxChainStage stage, bool enabled);

    // Emitted when the user clicks the ADSP launcher tile in the
    // strip's RX chain widget.  MainWindow opens the MasterDsp
    // dialog (or focuses an existing instance).
    void rxDspEditRequested();

    // Emitted when the user double-clicks an implemented RX stage
    // tile in the strip's chain widget.  MainWindow routes this to
    // the corresponding RX editor.
    void rxStageEditRequested(AudioEngine::RxChainStage stage);

protected:
    void closeEvent(QCloseEvent* ev) override;
    void moveEvent(QMoveEvent* ev) override;
    void resizeEvent(QResizeEvent* ev) override;
    void showEvent(QShowEvent* ev) override;
    void hideEvent(QHideEvent* ev) override;
    // Frameless 8-axis resize: 4 edges + 4 corners.  Mouse hover on
    // the bare margin around the embedded grid updates the cursor;
    // press starts a compositor-managed resize via startSystemResize.
    void mouseMoveEvent(QMouseEvent* ev) override;
    void mousePressEvent(QMouseEvent* ev) override;
    void leaveEvent(QEvent* ev) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void saveGeometryToSettings();
    void restoreGeometryFromSettings();
    Qt::Edges edgesAt(const QPoint& pos) const;
    void updateResizeCursor(const QPoint& pos);

    // Master bypass — snapshot all enabled TX stages, then disable
    // them.  Restores the snapshot on uncheck.  Mirrors the docked
    // ClientChainApplet's BYPASS button.
    void onBypassToggled(bool checked);

    // Preset combo helpers.
    void rebuildPresetCombo(const QString& selectName = QString());
    void onPresetComboActivated(int idx);
    void doImportPreset();
    void doExportPreset();
    void doExportLibrary();
    void doSavePreset();
    void doDeletePreset();
    void updatePresetButtonEnable();
    // After a preset has been applied to the engine, push fresh values
    // into every embedded panel's UI so labels / knobs / combos stop
    // showing the previous preset's data.
    void refreshAllPanelsFromEngine();

    AudioEngine*         m_audio{nullptr};
    ChannelStripPresets* m_presets{nullptr};
    QWidget*             m_titleBar{nullptr};   // custom inline ContainerTitleBar-styled bar
    QVBoxLayout*         m_bodyLayout{nullptr};
    QLabel*              m_titleLbl{nullptr};   // title text — toggles "— TX" / "— RX" suffix
    StripChainWidget*    m_chain{nullptr};
    StripRxChainWidget*  m_chainRx{nullptr};
    QStackedWidget*      m_chainStack{nullptr}; // page 0 = TX chain, page 1 = RX chain
    QStackedWidget*      m_panelStack{nullptr}; // page 0 = TX panels, page 1 = RX panels
    bool                 m_rxMode{false};       // currently displaying RX chain + panels
    QPushButton*         m_txBtn{nullptr};
    QPushButton*         m_rxBtn{nullptr};
    QPushButton*         m_bypassBtn{nullptr};
    QPushButton*         m_monRecBtn{nullptr};
    QPushButton*         m_monPlayBtn{nullptr};
    QComboBox*           m_presetCombo{nullptr};
    QPushButton*         m_presetSaveBtn{nullptr};
    QPushButton*         m_presetDeleteBtn{nullptr};
    QString              m_currentPresetName;
    bool                 m_buildingCombo{false};
    bool               m_monRecording{false};
    bool               m_monPlaying{false};
    bool               m_monHasRecording{false};
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
    MasterDspWidget*   m_adspRx{nullptr};      // ADSP launcher panel — embeds MasterDspWidget
    StripGatePanel*    m_agcT{nullptr};        // AGC-T (RX gate)
    StripEqPanel*      m_eqRx{nullptr};
    StripCompPanel*    m_agcC{nullptr};        // AGC-C (RX comp)
    StripDeEssPanel*   m_dessRx{nullptr};      // DESS (RX de-esser, #2425)
    StripTubePanel*    m_tubeRx{nullptr};
    StripPuduPanel*    m_evo{nullptr};         // EVO (RX pudu)
    class StripRxOutputPanel* m_outputRx{nullptr};   // RX output meter + mute + boost
    StripWaveformPanel*       m_waveformRx{nullptr}; // RX-side waveform tap
    bool               m_restoring{false};
};

} // namespace MasterSDR
