#pragma once

#include "models/RadioModel.h"
#include "models/BandSettings.h"
#include "models/AntennaGeniusModel.h"
#include "core/AppSettings.h"
#include "core/CommandParser.h"   // MessageSeverity for onRadioMessage slot
#include "core/RadioDiscovery.h"
#include "core/AudioEngine.h"
#include "core/RigctlServer.h"
#ifdef HAVE_WEBSOCKETS
#include "core/TciServer.h"
#endif
#include "core/RigctlPty.h"
#include "core/SmartLinkClient.h"
#include "core/WanConnection.h"
#include "core/CwDecoder.h"
#include "core/QsoRecorder.h"
#include "core/ClientPuduMonitor.h"
#include "core/DxClusterClient.h"
#ifdef HAVE_MQTT
#include "core/MqttClient.h"
#endif
#include "core/WsjtxClient.h"
#include "core/SpotCollectorClient.h"
#include "core/PotaClient.h"
#include "core/PropForecastClient.h"
#ifdef HAVE_WEBSOCKETS
#include "core/FreeDvClient.h"
#endif
#include <QThread>
#ifdef HAVE_SERIALPORT
#include "core/SerialPortController.h"
#include "core/FlexControlManager.h"
#endif
#ifdef HAVE_MIDI
#include "core/MidiControlManager.h"
#endif
#ifdef HAVE_HIDAPI
#include "core/HidEncoderManager.h"
#endif
#include "core/ShortcutManager.h"
#include "core/SpectrogramBuffer.h"
#include "core/SignalClassifier.h"
#include "core/TgxlConnection.h"
#include "core/PgxlConnection.h"
#include "core/DxccColorProvider.h"

#include <QMainWindow>
#include <QSplitter>
#include <QPointer>
#include <QLabel>
#include <QList>
#include <QMenu>
#include <QStatusBar>
#include <QSizeGrip>
#include <QHash>
#include <QJsonObject>
#include <QTimer>
#include <QEvent>
#include <atomic>

class QAbstractSlider;
class QMediaDevices;

#include "gui/ClientEqApplet.h"   // ClientEqApplet::Path enum used in
                                   // onEqCutoffsDragRequested signature.
#include "gui/PersistentDialog.h" // showOrRaisePersistent template needs
                                   // PersistentDialog visible at point of use.

namespace AetherSDR {

class ConnectionPanel;
class TitleBar;
class SpectrumWidget;
class PanadapterApplet;
class PanadapterStack;
class AppletPanel;
class BandPlanManager;
class NetworkDiagnosticsHistory;
class WhatsNewDialog;
class ProfileManagerDialog;
class ProfileImportExportDialog;
class RadioSetupDialog;
class NetworkDiagnosticsDialog;
class MemoryDialog;
class PropDashboardDialog;
class TxBandDialog;
class AetherDspDialog;
class WaveformsDialog;
class DxClusterDialog;
class Ax25HfPacketDecodeDialog;
class FlexControlDialog;
class MidiMappingDialog;
class CwxPanel;
class DvkPanel;
#ifdef HAVE_RADE
class RADEEngine;
#endif
#if defined(Q_OS_MAC)
class VirtualAudioBridge;
using DaxBridge = VirtualAudioBridge;
#elif defined(HAVE_PIPEWIRE)
class PipeWireAudioBridge;
using DaxBridge = PipeWireAudioBridge;
#endif
class VfoWidget;

// Wheel mode for FlexControl: determines what the encoder knob adjusts.
enum class FlexWheelMode {
    Frequency,
    Volume,
    Power,
    Rit,
    Xit,
    MasterAf,
    HeadphoneVolume,
    AgcT,
    Apf,
    CwSpeed
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    // Radio/connection events
    void onConnectionStateChanged(bool connected);
    void onConnectionError(const QString& msg);
    void onRadioMessage(const QString& text, MessageSeverity severity);
    void onSliceAdded(SliceModel* slice);
    void onSliceRemoved(int id);

    // Master volume — single entry point used by both the title bar slider
    // (TitleBar::masterVolumeChanged) and TCI clients (TciServer::
    // masterVolumeRequested) so the audio path, persistence, and TCI
    // broadcast all stay in lockstep regardless of which UI changed it.
    // See issue #1764.
    void applyMasterVolume(int pct);

private:
    enum class TuneIntent {
        IncrementalTune,
        AbsoluteJump,
        CommandedTargetCenter,
        ExplicitPan,
        RevealOffscreen,
    };

    enum class BandStackPreselectResult {
        NotNeeded,
        Selected,
        Unsupported,
    };

    struct TuneCenteringResult {
        double oldCenterMhz{0.0};
        double newCenterMhz{0.0};
        double bandwidthMhz{0.0};
        bool followRevealTriggered{false};
        bool hardCenterUsed{false};
        int animationDurationMs{0};
    };

    void buildUI();
    void buildMenuBar();
    void applyDarkTheme();

    // Audio thread helpers — invoke AudioEngine methods on the worker thread (#502)
    void audioStartRx();
    void audioStopRx();
    void audioStartTx(const QHostAddress& addr, quint16 port);
    void audioStopTx();
    void updatePcAudioTooltip();
    void setupAudioDeviceChangeMonitor();
    void scheduleAudioDeviceChangeCheck();
    void handleAudioDeviceListChanged();
    void applyAudioDeviceSelection(const QAudioDevice& inputDevice,
                                   const QAudioDevice& outputDevice,
                                   bool reinitializePcInput);
    void resetMissingAudioDevicesToDefault(bool resetInput,
                                           bool resetOutput,
                                           bool reinitializePcInput);
    SliceModel* activeSlice() const;
    static const char* tuneIntentName(TuneIntent intent);
    bool panFollowEnabled() const;
    BandStackPreselectResult preselectBandStackForTune(SliceModel* slice, double mhz,
                                                       const char* source);
    void applyTuneRequest(SliceModel* slice, double mhz,
                          TuneIntent intent, const char* source);
    void applyPanRangeRequest(const QString& panId, double centerMhz,
                              double bandwidthMhz, const char* source);
    // leftFlagEdgeOffsetMhz / rightFlagEdgeOffsetMhz extend the trigger
    // comparison out to the VFO flag's outer edges so the flag panel never
    // clips a pan edge.  Only IncrementalTune consumes the offsets; other
    // intents (CommandedTargetCenter, RevealOffscreen) ignore them.  Default
    // 0.0 preserves the original slice-frequency comparison for non-flag
    // callers.  See #2761 + panFollowVfo() for the integration site.
    TuneCenteringResult revealFrequencyIfNeeded(SliceModel* slice, double mhz,
                                                TuneIntent intent, const char* source,
                                                double leftFlagEdgeOffsetMhz = 0.0,
                                                double rightFlagEdgeOffsetMhz = 0.0);
    void logTunePolicyDecision(const char* source, TuneIntent intent,
                               double oldFreqMhz, double newFreqMhz,
                               const TuneCenteringResult& result) const;
    void mirrorDiversityChildFrequency(SliceModel* slice, double mhz);
    // Pan-follow-VFO (#989): if mhz is outside the visible pan window, apply
    // the new center locally (immediate repaint) and send the radio command.
    TuneCenteringResult panFollowVfo(SliceModel* s, double mhz, const char* source);
    SpectrumWidget* spectrum() const;
    void setActiveSlice(int sliceId);
    void setActiveSliceInternal(int sliceId, bool revealOffscreen);
    void queueActiveSliceForSpectrumTarget(int sliceId);
    void updateFilterLimitsForMode(const QString& mode);
    void centerActiveSliceInPanadapter(bool forceRadioCenter, double centerMhz = -1.0);
    void pushSliceOverlay(SliceModel* s);
    void updateSplitState();
    void disableSplit();
    void wirePanadapter(PanadapterApplet* applet);
    void schedulePanFpsReconcile(const QString& panId, int reportedFps);
    void scheduleWaterfallLineDurationReconcile(const QString& panId, int reportedMs);
    void reassertUnmutedSliceAudioForPan(const QString& panId);
    void onMuteAllSlicesToggle();
    void showPanadapterInterlockNotification(const QString& message);
    void setActivePanApplet(PanadapterApplet* applet);
    void routeCwDecoderOutput();  // wire CW decoder to the pan owning the active slice
    // Recompute the CW decoder's run state, panel visibility, and the
    // AudioEngine TX tap based on current AppSettings + MOX + slice mode.
    // Called on MOX change, RX/TX toggle change, and dialog close (#2417).
    void refreshCwDecodeState();
    SpectrumWidget* spectrumForSlice(SliceModel* s) const;
    void wireVfoWidget(VfoWidget* w, SliceModel* s);
    // Push the active RX slice's filter passband (converted from
    // protocol offsets to audio-domain low/high) to the RX EQ canvases.
    void pushRxFilterCutoffsToEq();
    void enableNr2WithWisdom();  // Wisdom-gated NR2 enable (shared by VFO + overlay)
    void updateNr2Availability(); // Disable NR2 when Opus is active (#1597)
    void registerShortcutActions();
    void applyUiScale(int pct);
    void stepUiScale(int direction);  // +1 = zoom in, -1 = zoom out
    void toggleMinimalMode(bool on);
    // Toggle the Aetherial Audio Channel Strip — unified TX DSP window.
    // Stubbed in step 1 of #2301; step 4 lazy-creates the strip window
    // and persists visibility via AppSettings("AetherialStripVisible").
    void toggleAetherialStrip();
    // Cutoff-line drag handler shared between the floating ClientEqEditor
    // and the embedded EQ panel inside AetherialAudioStrip.  Writes TX
    // filter cutoffs to TransmitModel, or RX filter offsets to the
    // active SliceModel (with mode-aware audio→slice conversion).
    void onEqCutoffsDragRequested(ClientEqApplet::Path path,
                                  int audioLow, int audioHigh);
    // Single handler for TX chain-stage enable/bypass changes from
    // ANY widget (docked Chain applet OR channel strip).  Refreshes
    // the matching applet's enable indicator and forces a repaint on
    // both chain widgets so they stay in lock-step.
    void onTxChainStageEnabledChanged(AudioEngine::TxChainStage stage,
                                      bool enabled);
    // Toggle OS window-chrome on/off (Qt::FramelessWindowHint).  Persists
    // to AppSettings("FramelessWindow"). When on, users move/close the
    // window via keyboard shortcuts or taskbar.
    void setFramelessWindow(bool on);

    // Lazy-construct + show + raise + activate for a PersistentDialog
    // subclass.  Collapses the ~10-line "if slot raise else new+setAttribute+
    // setFramelessMode+assign+show" boilerplate at every menu callback into
    // a one-liner.  Auto-registers the dialog so setFramelessWindow() can
    // propagate the frameless toggle without an explicit qobject_cast branch.
    //
    // Slot must be typed QPointer<ConcreteDialog> so the ctor args match.
    // Example: showOrRaisePersistent(m_profileManagerDialog, &m_radioModel);
    template <class T, class... Args>
    void showOrRaisePersistent(QPointer<T>& slot, Args&&... ctorArgs);

    // Create-or-raise helper for the AetherDSP Settings dialog.  Centralizes
    // the ~17 audio-parameter signal connections that every call site was
    // duplicating; on first construction wires them once, on subsequent calls
    // just raises the existing instance.  Returns nullptr only if construction
    // failed (e.g. allocation failure).
    AetherDspDialog* ensureAetherDspDialog();

    // Wire the txBandSettingsRequested, serialSettingsChanged (HAVE_SERIALPORT),
    // sliceLetterDisplayModeChanged, and QDialog::finished handlers on a freshly-
    // constructed RadioSetupDialog.  Called from every entry point (Settings →
    // Radio Setup, FlexControl, USB Cables, XVTR overlay) so all four sites share
    // identical wiring once they converge on the single PersistentDialog instance
    // (#2781).  prevComp is the audio-compression value captured at open time so
    // the finished handler can detect a change and recreate the RX audio stream.
    void wireRadioSetupDialogSignals(RadioSetupDialog* dlg, const QString& prevComp);

    // Reorder the main splitter so the applet panel sits on the left or
    // right of the panadapter stack.  Wired from the dock-side icons in
    // the title bar and persisted via "AppletPanelDockedLeft".
    void setAppletPanelDockedLeft(bool left);

    // Show/hide the applet panel — single source of truth that updates the
    // title-bar dock icons and the persisted "AppletPanelVisible" setting.
    void setAppletPanelVisible(bool visible);

    // Toggle the applet panel between docked-in-splitter and floating in
    // its own Qt::Window.  Persists "AppletPanelFloating" and updates the
    // title-bar pop-out icon highlight.
    void toggleAppletPanelFloating(bool floating);

    void showMemoryDialog();
    void showQuickAddMemoryDialog(const QString& preferredPanId = {});
    void updateKeyerAvailability(const QString& mode);
    void showNr2ParamPopup(const QPoint& globalPos);
    void showNr4ParamPopup(const QPoint& globalPos);
    void showDfnrParamPopup(const QPoint& globalPos);
    void showMnrSettings();
    void applyPanLayout(const QString& layoutId);
    void createPansSequentially(const QString& layoutId, int total,
                                std::shared_ptr<QStringList> panIds, int created);
    void showPanadapterSliceCapacityMessage();
    void updatePaTempLabel();
    void showNetworkDiagnosticsDialog();
    void showAx25HfPacketDecodeDialog();
    void showFlexControlDialog();
    void handleFlexControlTuneSteps(int steps);
    void handleFlexControlButton(int button, int action);
    void handleVirtualFlexControlWheel(const QString& actionId, int steps);
    void applyFlexControlWheelAction(const QString& actionId, int steps);
    void syncFlexControlDialog();
    void syncFlexControlIndicatorForSettings();
    void setFlexControlHardwareIndicator(int button);
    QJsonObject buildControlDevicesSnapshot() const;
    void showPropDashboard();
    void showMultiFlexDialog();
    void handleMultiFlexClientDisconnect(quint32 handle, const QString& displayName);
    bool confirmClientSlotAvailability(const RadioInfo& info, QList<quint32>* disconnectHandles);
    bool confirmClientSlotAvailability(const WanRadioInfo& info, QList<quint32>* disconnectHandles);
    bool sendWanRadioClientDisconnects(const QString& serial, const QList<quint32>& handles);
    void disconnectWanRadioClients(const WanRadioInfo& info);
    void startWanRadioConnect(const WanRadioInfo& info, bool promptForClientSlots = true);
    void requestWanReconnect();
    void showForcedDisconnectDialog(bool wasWan, const RadioInfo& radioInfo, const WanRadioInfo& wanInfo);
    void setPaTempDisplayUnit(bool useFahrenheit);
    void setPanadapterConnectionAnimation(bool visible, const QString& label = {});
    void finishPanadapterConnectionAnimation();
    void syncMemorySpot(int memoryIndex);
    void removeMemorySpot(int memoryIndex);
    void clearMemorySpotFeed();
    void rebuildMemorySpotFeed();
    void refreshMemoryBrowsePanel();
    void updateBandStackIndicator();
    SliceModel* preferredMemorySlice(const QString& preferredPanId) const;
    bool activateMemorySpot(int memoryIndex, const QString& preferredPanId = {});
    void beginSliderShortcutLease(QAbstractSlider* slider);
    void renewSliderShortcutLease();
    void releaseSliderShortcutLease(bool clearFocus);

    BandSnapshot captureCurrentBandState() const;
    void restoreBandState(const BandSnapshot& snap);
    void startSwrSweep(int requestedSliceId = -1, int sweepPowerWatts = 1);
    void clearSwrSweepPlot();
    void advanceSwrSweep();
    void finishSwrSweep(bool aborted, const QString& reason = {});
    void beginSwrSweepRf();
    void finishSwrSweepAfterTuneStopped();
    void completeSwrSweepFinish();
    void commandSwrSweepFrequency(double freqMhz, int settleMs);
    void updateSwrSweepOverlay(double currentFreqMhz = -1.0);
    void setSwrSweepInputsLocked(bool locked);
    void clearSwrSweepForBandChange(int sliceId, const QString& panId,
                                    const QString& newBandName);
    SliceModel* swrSweepTargetSlice(int requestedSliceId = -1) const;
    void setCwStraightKeyState(bool down, const QString& source = {},
                               quint64 traceId = 0, quint64 sourceMs = 0);
    void setCwLeftPaddleState(bool down, const QString& source = {},
                              quint64 traceId = 0, quint64 sourceMs = 0);
    void setCwRightPaddleState(bool down, const QString& source = {},
                               quint64 traceId = 0, quint64 sourceMs = 0);
    void pushCwPaddleState(const QString& source = {},
                           quint64 traceId = 0, quint64 sourceMs = 0);
    bool handleCwMomentaryShortcut(QKeyEvent* keyEvent, QEvent::Type eventType);

    // Core objects
    RadioDiscovery    m_discovery;
    RadioModel        m_radioModel;
    DxccColorProvider m_dxccProvider;
    AudioEngine*      m_audio{nullptr};
    QThread*          m_audioThread{nullptr};
    QMediaDevices*    m_audioDeviceMonitor{nullptr};
    QTimer            m_audioDeviceChangeTimer;
    QList<QByteArray> m_knownAudioInputIds;
    QList<QByteArray> m_knownAudioOutputIds;
    QByteArray        m_knownDefaultAudioInputId;
    QByteArray        m_knownDefaultAudioOutputId;
    bool              m_audioDeviceDialogOpen{false};
    NetworkDiagnosticsHistory* m_networkDiagnosticsHistory{nullptr};
    QsoRecorder*      m_qsoRecorder{nullptr};
    ClientPuduMonitor* m_finalMonitor{nullptr};
    BandSettings      m_bandSettings;
    // 8-channel CAT: each channel (A-H) binds to a slice index (0-7)
    static constexpr int kCatChannels = 8;
    RigctlServer*     m_rigctlServers[kCatChannels]{};
    RigctlPty*        m_rigctlPtys[kCatChannels]{};
#ifdef HAVE_WEBSOCKETS
    TciServer*        m_tciServer{nullptr};
#endif
    SmartLinkClient   m_smartLink;
    WanConnection     m_wanConnection;
    AntennaGeniusModel m_antennaGenius;
    TgxlConnection    m_tgxlConn;        // direct TCP 9010 to TGXL for manual relay control
    PgxlConnection    m_pgxlConn;        // direct TCP 9008 to PGXL for telemetry
    BandPlanManager*  m_bandPlanMgr{nullptr};
    CwDecoder         m_cwDecoder;
    // Dedicated TX-side decoder (#2417).  Fed from AudioEngine's
    // sidetone mirror; emits decoded text routed to the panel via
    // PanadapterApplet::appendCwTextTx so the operator can tell TX
    // self-decode apart from RX in the shared text view.
    CwDecoder         m_cwDecoderTx;
    DxClusterClient*   m_dxCluster{nullptr};
    DxClusterClient*   m_rbnClient{nullptr};
#ifdef HAVE_MQTT
    MqttClient*        m_mqttClient{nullptr};
#endif
    WsjtxClient*       m_wsjtxClient{nullptr};
    SpotCollectorClient* m_spotCollectorClient{nullptr};
    PotaClient*          m_potaClient{nullptr};
    PropForecastClient*  m_propForecast{nullptr};
#ifdef HAVE_WEBSOCKETS
    FreeDvClient*      m_freedvClient{nullptr};
#endif
    QThread*           m_spotThread{nullptr};

    // Spot deduplication: callsign → {freqMhz, timestamp ms}
    struct SpotDedup {
        double freqMhz;
        qint64 addedMs;
    };
    QHash<QString, SpotDedup> m_spotDedup;

    // S History Markers — auto-detected voice signals per panadapter
    struct SHistoryEntry {
        double          freqMhz;
        float           peakDbm;
        QString         mode;
        qint64          firstDetectedMs{0};
        qint64          lastSeenMs{0};
        double          widthHz{0.0};
        bool            suspectQrm{false};
        // Hit timestamps for the last 10 seconds. Used to detect both
        // qualification streaks and QRM persistence (>90% occupancy).
        QVector<qint64> hitTimestamps;
        bool            visible{false};
        bool            confirmedVoice{false}; // true once shown as a gold voice marker
        qint64          lastGapMs{0};          // last time a ≥1 s gap was detected (epoch ms)
        // CNN classifier: exponential moving average of carrier probability.
        // 0.0 = strongly voice, 1.0 = strongly carrier. 0.5 = unknown (ONNX absent).
        float           carrierScore{0.5f};
        // Last time a voice-width (1.8–8 kHz) signal was detected while this
        // entry was already QRM-classified.  Drives the "voice over QRM"
        // double-marker — shows both a red QRM marker and a gold voice marker.
        qint64          voiceOverQrmLastMs{0};
    };
    struct SHistoryPanState {
        double centerMhz{0.0};
        double bandwidthMhz{0.0};
        qint64 suppressUntilMs{0};
        qint64 lastFrameMs{0};
        float  fpsEwma{25.0f};  // EWMA of observed frames/sec; starts at 25 fps
    };
    QHash<QString, QVector<SHistoryEntry>> m_sHistoryData;  // panId → entries
    QHash<QString, SHistoryPanState> m_sHistoryPanState;
    QTimer* m_sHistoryExpireTimer{nullptr};
    bool    m_sHistoryEnabled{false};
    bool    m_sHistoryQrmEnabled{false};
    bool    m_smartSpotFilterEnabled{false};
    qint64  m_smartSpotFilterEnabledMs{0};
    // Single apply path used by the SpotHub Display tab toggles (no
    // View-menu duplicate).  Updates the member flag, persists via
    // AppSettings, pushes to all spectrum widgets, and clears the data
    // hashes when both markers go off.
    void applySHistoryEnabled(bool on);
    void applySHistoryQrmEnabled(bool on);
    void rebuildSHistoryForPan(const QString& panId);
    void expireSHistoryMarkers();
    void onSpectrumReadyForSHistory(quint32 streamId, const QVector<float>& bins, qint64 emittedNs);
    // Per-pan spectrogram ring buffer for CNN classification.
    // shared_ptr so QHash COW can copy the pointer on detach without deep-copying
    // the 32-frame ring buffer (unique_ptr is non-copyable, which breaks QHash::operator[]).
    QHash<QString, std::shared_ptr<AetherSDR::SpectrogramBuffer>> m_spectrogramBuffers;
    AetherSDR::SignalClassifier m_signalClassifier;

    // Batched spot add commands (flushed 1/sec)
    QStringList m_spotCmdBatch;
    int m_nextPassiveSpotId{-2000000};
    QHash<int, qint64> m_passiveSpotExpiryMs;
    // External controllers run on a dedicated worker thread (#502)
    QThread*             m_extCtrlThread{nullptr};
#ifdef HAVE_SERIALPORT
    SerialPortController* m_serialPort{nullptr};
    FlexControlManager*   m_flexControl{nullptr};
    bool                  m_flexControlConnected{false};
#endif
    QTimer               m_flexCoalesceTimer;
    double               m_flexTargetMhz{-1.0};
    FlexWheelMode        m_flexWheelMode{FlexWheelMode::Frequency};
    int                  m_flexActiveLedButton{0};
    bool                 m_flexVirtualBandZoomOn{false};
    bool                 m_flexVirtualSegmentZoomOn{false};
#ifdef HAVE_HIDAPI
    HidEncoderManager*   m_hidEncoder{nullptr};
    QTimer               m_hidCoalesceTimer;
    int                  m_hidPendingSteps{0};
#endif
#ifdef HAVE_MIDI
    MidiControlManager*  m_midiControl{nullptr};
    QTimer               m_midiTuneIdleTimer;
    double               m_midiTuneTargetMhz{-1.0};
    void registerMidiParams();
    struct MidiActionTrace {
        QString paramId;
        quint64 traceId{0};
        quint64 callbackMs{0};
        quint64 dispatchMs{0};
    };
    MidiActionTrace m_currentMidiTrace;
    // MIDI param setters indexed by ID — called on main thread from
    // paramActionTrace signal (worker thread cannot call them directly). (#502)
    QHash<QString, std::function<void(float)>> m_midiSetters;
    QHash<QString, std::function<float()>>     m_midiGetters;
#endif

    // GUI — left sidebar
    ConnectionPanel* m_connPanel{nullptr};

    // GUI — main area
    TitleBar*         m_titleBar{nullptr};
    ::QSizeGrip*      m_sizeGrip{nullptr};
    QSplitter*        m_splitter{nullptr};
    PanadapterStack*  m_panStack{nullptr};
    QPointer<PanadapterApplet> m_panApplet;  // backward compat alias to active applet
    QPointer<PanadapterApplet> m_cwDecoderApplet;  // applet receiving CW decoder output

    // GUI — right applet panel
    AppletPanel*     m_appletPanel{nullptr};

    // Modeless dialogs
    QPointer<DxClusterDialog> m_spotHubDialog;
    QPointer<RadioSetupDialog> m_radioSetupDialog;
    QPointer<NetworkDiagnosticsDialog> m_networkDiagnosticsDialog;
    QPointer<PropDashboardDialog> m_propDashboardDialog;
    QPointer<TxBandDialog> m_txBandDialog;
    QPointer<MemoryDialog> m_memoryDialog;
    QPointer<Ax25HfPacketDecodeDialog> m_ax25HfPacketDecodeDialog;
    QPointer<FlexControlDialog> m_flexControlDialog;
    QPointer<WhatsNewDialog> m_whatsNewDialog;
    QPointer<AetherDspDialog> m_dspDialog;
    QPointer<WaveformsDialog> m_waveformsDialog;
    QPointer<ProfileManagerDialog> m_profileManagerDialog;
    QPointer<ProfileImportExportDialog> m_profileImportExportDialog;
#ifdef HAVE_MIDI
    QPointer<MidiMappingDialog> m_midiDialog;
#endif

    // Tracks every PersistentDialog created via showOrRaisePersistent() so
    // setFramelessWindow() can propagate the frameless toggle without an
    // explicit per-dialog qobject_cast branch.  QPointer entries auto-null on
    // dialog destruction (QSet::insert handles deduplication on null QPointer
    // re-creation by removing them on iteration via removeIf below).
    QList<QPointer<PersistentDialog>> m_persistentDialogs;

    // Menus
    QMenu*           m_profilesMenu{nullptr};
    QAction*         m_txBandAction{nullptr};

    // Audio stream re-creation flag (after profile load)
    bool             m_needAudioStream{false};

    // Pending WAN radio (between requestConnect and connectReady)
    WanRadioInfo     m_pendingWanRadio;
    QTimer           m_wanReconnectTimer;
    bool             m_wanReconnectAttemptInProgress{false};

    // Status bar labels (SmartSDR-style)
    QLabel* m_connStatusLabel{nullptr};   // hidden, used for connection state logic
    QLabel* m_addPanLabel{nullptr};
    QLabel* m_tnfIndicator{nullptr};
    QLabel* m_cwxIndicator{nullptr};
    CwxPanel* m_cwxPanel{nullptr};
    DvkPanel* m_dvkPanel{nullptr};
    QLabel* m_dvkIndicator{nullptr};
    QLabel* m_fdxIndicator{nullptr};
    QLabel* m_radioInfoLabel{nullptr};
    QLabel* m_radioVersionLabel{nullptr};
    QLabel* m_stationLabel{nullptr};
    QLabel* m_stationNickLabel{nullptr};
    QLabel* m_gpsLabel{nullptr};
    QLabel* m_gpsStatusLabel{nullptr};
    QLabel* m_bandStackIndicator{nullptr};
    QLabel* m_cpuLabel{nullptr};
    QLabel* m_memLabel{nullptr};
    QTimer* m_cpuTimer{nullptr};
    QLabel* m_paTempLabel{nullptr};
    QLabel* m_supplyVoltLabel{nullptr};
    QLabel* m_networkLabel{nullptr};
    QTimer m_networkTooltipRefreshTimer;
    QTimer m_perfHeartbeatTimer;
    QLabel* m_tgxlSeparator{nullptr};
    QLabel* m_tgxlIndicator{nullptr};
    QLabel* m_pgxlSeparator{nullptr};
    QLabel* m_pgxlIndicator{nullptr};
    QLabel* m_txIndicator{nullptr};
    QLabel* m_gpsDateLabel{nullptr};
    QLabel* m_gpsTimeLabel{nullptr};

    // Active slice tracking for multi-slice support
    int m_activeSliceId{-1};
    bool m_splitActive{false};
    int  m_splitRxSliceId{-1};
    int  m_splitTxSliceId{-1};
    int  m_pendingMemoryRevealSliceId{-1};
    double m_pendingMemoryRevealTargetMhz{0.0};
    int  m_pendingSpectrumTargetSliceId{-1};

    // Guard: set true while updating controls from the model so shared tune
    // helpers do not echo model-driven changes back to the radio.
    bool m_updatingFromModel{false};
    bool m_shuttingDown{false};
    bool m_panadapterUiPreparedForShutdown{false};
    void preparePanadapterUiForShutdown();
    void toggleConnectionDialog();
    bool m_useSystemClock{true};     // true when no GPS installed
    bool m_paTempUseFahrenheit{true};
    bool m_hasPaTempTelemetry{false};
    float m_lastPaTempC{0.0f};
    bool m_userDisconnected{false};  // true after explicit disconnect, blocks auto-connect
    QDialog* m_reconnectDlg{nullptr}; // shown on unexpected disconnect, dismissed on reconnect
    void cancelTransmitFromIndicator();
    class ClientEqEditor* m_clientEqEditor{nullptr}; // lazy — created on first Edit… click
    // Lazy-construct the floating EQ editor on first access, with all
    // bypass-toggled wiring set up once.  Used from every site that
    // wants to open the editor (CEQ-TX applet, CEQ-RX applet, TX
    // chain widget Eq stage, RX chain widget Eq stage).
    class ClientEqEditor* ensureClientEqEditor();
    class ClientGateEditor* ensureClientGateEditor();
    class ClientCompEditor* ensureClientCompEditor();
    class ClientTubeEditor* ensureClientTubeEditor();
    class ClientPuduEditor* ensureClientPuduEditor();

    // Wire AetherDspWidget parameter signals to AudioEngine setters.  Used
    // by both the modeless AetherDspDialog and the docked ClientRxDspApplet
    // so they push every change into the engine identically.
    void wireAetherDspWidget(class AetherDspWidget* widget);
    class ClientCompEditor* m_clientCompEditor{nullptr}; // lazy — created on first Edit… click
    class ClientGateEditor* m_clientGateEditor{nullptr}; // lazy — created on first Edit… click
    class ClientTubeEditor* m_clientTubeEditor{nullptr}; // lazy — created on first Edit… click
    class ClientPuduEditor* m_clientPuduEditor{nullptr}; // lazy — created on first Edit… click
    class AetherialAudioStrip* m_aetherialStrip{nullptr};    // lazy — created on first egg-nub click (#2301)

    // Applet-panel pop-out support (#1713 Phase 6).  When floating,
    // the panel lives inside m_appletPanelFloatWindow and its splitter
    // slot is removed; re-dock appends a fresh slot and re-applies the
    // canonical {0, 0, width-260, 260} sizing.
    QWidget*    m_appletPanelFloatWindow{nullptr};
    void floatAppletPanel();
    void dockAppletPanel();
    bool m_displaySettingsPushed{false};  // one-shot: push saved display settings after pan created
    bool m_applyingLayout{false};        // true during layout tear-down/recreate — suppresses panadapterAdded handler
    struct PanFpsReconcileState {
        QTimer* timer{nullptr};
        QPointer<SpectrumWidget> spectrum;
        qint64 lastSentMs{0};
        int lastSentDesired{-1};
    };
    QHash<QString, PanFpsReconcileState> m_panFpsReconcile;
    QHash<QString, QMetaObject::Connection> m_panFpsReconcileConnections;
    bool m_adaptiveThrottleActive{false}; // fps/wf reconcile suppressed while true
    int  m_adaptiveFpsCap{0};             // current cap (> 0 when throttle active); shown in network label
    struct WaterfallLineDurationReconcileState {
        QTimer* timer{nullptr};
        QPointer<SpectrumWidget> spectrum;
        qint64 lastSentMs{0};
        int lastSentDesired{-1};
    };
    QHash<QString, WaterfallLineDurationReconcileState> m_wfLineDurationReconcile;
    QHash<QString, QMetaObject::Connection> m_wfLineDurationReconcileConnections;
    QTimer* m_layoutRestoreTimer{nullptr}; // debounced layout rearrange after pans added on connect
    qint64 m_layoutRestoreUntilMs{0};
    // User layout choices should suppress startup rearrange, but still allow
    // the pending timer to restore saved floating pan windows.
    bool m_suppressStartupPanLayoutRearrange{false};
    QTimer* m_heartbeatMissTimer{nullptr}; // fires every 1.5s to detect missed discovery beats
    QTimer* m_bsExpiryTimer{nullptr};    // band-stack bookmark auto-expiry, started on connect only (#1471)
    QTimer* m_bsAutoSaveTimer{nullptr};  // band-stack dwell auto-save (single-shot per dwell window)
    QTimer* m_agManualConnectTimer{nullptr}; // deferred AG manual connect — cancelled on disconnect
    class CwxLocalKeyer* m_cwxLocalKeyer{nullptr};  // local Morse keyer for CWX sidetone
    std::unique_ptr<class IambicKeyer> m_iambicKeyer;  // local iambic state machine for paddle sidetone
    std::atomic<quint64> m_lastCwPaddleTraceId{0};
    std::atomic<quint64> m_lastCwPaddleSourceMs{0};
    qint64 m_bsConnectGraceUntilMs{0};   // suppress auto-save right after connect
    bool m_keyboardShortcutsEnabled{false}; // global enable for keyboard shortcuts (View menu)
    bool m_spacePttActive{false};          // true while Space is held for PTT
    bool m_cwStraightKeyActive{false};
    bool m_cwLeftPaddleActive{false};
    bool m_cwRightPaddleActive{false};
    QPointer<QAbstractSlider> m_sliderShortcutLease;
    QTimer m_sliderShortcutLeaseTimer;
    struct SwrSweepSample {
        double freqMhz{0.0};
        float swr{1.0f};
    };
    enum class SwrSweepPhase {
        Idle,
        WaitingForTgxlBypass,
        TgxlBypassSettle,
        Sweeping,
        StoppingTune,
        RestoringTgxl,
    };
    enum class SwrSweepMeterSource {
        Radio,
        Tgxl,
    };
    struct SwrSweepState {
        bool running{false};
        SwrSweepPhase phase{SwrSweepPhase::Idle};
        SwrSweepMeterSource meterSource{SwrSweepMeterSource::Radio};
        int sliceId{-1};
        QString panId;
        double originalFreqMhz{0.0};
        double originalPanCenterMhz{0.0};
        double originalPanBandwidthMhz{0.0};
        QVector<double> frequencies;
        QVector<SwrSweepSample> samples;
        int currentIndex{-1};
        qint64 commandIssuedAtMs{0};
        qint64 sampleNotBeforeMs{0};
        qint64 phaseStartedAtMs{0};
        float minimumForwardPowerW{0.0f};
        int originalTunePower{0};
        int sweepTunePower{1};
        bool tuneStarted{false};
        bool finalAborted{false};
        bool clearPlotOnFinish{false};
        bool tgxlOriginalOperate{false};
        bool tgxlOriginalBypass{false};
        bool tgxlBypassRequested{false};
        bool tgxlRestoreNeeded{false};
        bool tgxlRestoreTimedOut{false};
        QString finalReason;
        QString sourceLabel;
        QString originalBandName;
        bool preserveBandSwitchOnFinish{false};
        bool appletPanelWasEnabled{true};
        bool panStackWasEnabled{true};
    };
    SwrSweepState m_swrSweep;
    QTimer m_swrSweepTimer;
    bool m_minimalMode{false};             // true when spectrum is hidden (#208)
    bool m_exitingMinimalMode{false};      // re-entry guard for changeEvent → toggleMinimalMode(false)
    bool m_enteringMinimalMode{false};     // suppress changeEvent during enter (macOS deferred WindowStateChange, #2365)
    QAction* m_minimalModeAction{nullptr};
    bool m_panadapterConnectionAnimationVisible{false};
    bool m_waitingForFirstPanadapterFrame{false};
    QString m_panadapterConnectionAnimationLabel;
    ShortcutManager m_shortcutManager;

#ifdef HAVE_RADE
    RADEEngine* m_radeEngine{nullptr};
    QThread*    m_radeThread{nullptr};
    int  m_radeSliceId{-1};
    bool m_radePrevMute{false};
    quint32 m_radeDaxStreamId{0};
    QMetaObject::Connection m_radeDaxStreamConn;
    QMetaObject::Connection m_freedvMoxConn;
    QString m_lastRadeRxCallsign;
    void activateRADE(int sliceId);
    void deactivateRADE();
    void onRadeSliceModeChanged(const QString& mode);
    void startFreeDvReporting(int sliceId);
    void stopFreeDvReporting(int sliceId);
    // FreeDV Docker waveform sync/SNR display state
    int  m_fdvDisplaySliceId{-1};
    int  m_fdvSnrMeterIndex{-1};
    bool m_fdvSynced{false};
    void activateFdvDisplay(int sliceId);
    void deactivateFdvDisplay();
    void onFdvMeterUpdated(int index, float value);
    void onFdvMetersChanged();
#endif

#if defined(Q_OS_MAC) || defined(HAVE_PIPEWIRE)
    DaxBridge* m_daxBridge{nullptr};
    QString m_savedMicSelection;  // restore on stopDax
    bool startDax();
    void stopDax();
#endif
};

template <class T, class... Args>
void MainWindow::showOrRaisePersistent(QPointer<T>& slot, Args&&... ctorArgs)
{
    if (!slot) {
        auto* dlg = new T(std::forward<Args>(ctorArgs)..., this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setFramelessMode(
            AppSettings::instance().value("FramelessWindow", "True").toString() == "True");
        slot = dlg;
        m_persistentDialogs.append(QPointer<PersistentDialog>(dlg));
    }
    slot->show();
    slot->raise();
    slot->activateWindow();
}

} // namespace AetherSDR
