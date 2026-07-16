#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// ⚠️  MainWindow is DECOMPOSED (#3351). Add member fields/declarations here ONLY
//     when genuinely cross-cutting — a feature's method bodies live in the
//     matching MainWindow_*.cpp sibling TU (still MainWindow:: members, declared
//     here only because C++ requires it). Don't grow this class for feature work.
//     Map + decision guide: docs/architecture/mainwindow-decomposition.md
// ─────────────────────────────────────────────────────────────────────────────

#include "models/RadioModel.h"
#include "models/BandSettings.h"
#include "models/AntennaGeniusModel.h"
#include "core/AppSettings.h"
#include "core/RadioMessageTypes.h"   // MessageSeverity for onRadioMessage slot
#include "core/RadioDiscovery.h"
#include "core/AudioEngine.h"
#include "core/ReceivePresentationSync.h"
#include "gui/KiwiRebindTracker.h"      // #4158 band-recall Kiwi re-bind policy
#include "core/CatPort.h"
#ifdef HAVE_WEBSOCKETS
#include "core/TciServer.h"
#endif
#include "core/SmartLinkClient.h"
#include "core/WanConnection.h"
#include "core/CwDecoder.h"
#include "core/CwCallsignSpotter.h"
#include "core/RttyDecoder.h"
#include "core/QsoRecorder.h"
#include "core/ClientPuduMonitor.h"
#include "core/AudioOutputRouter.h"
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
#include "gui/FreeDvReporterDialog.h"
#endif
#include <QThread>
#ifdef HAVE_SERIALPORT
#include "core/SerialPortController.h"
#include "core/FlexControlManager.h"
#endif
#include "models/RadioSession.h"

#include <memory>
#include <vector>

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
#include "core/AcomConnection.h"
#include "core/DxccColorProvider.h"

#include <QMainWindow>
#include <QSplitter>
#include <QPointer>
#include <QLabel>
#include <QList>
#include <QMenu>
#include <QSet>
#include <QStatusBar>
#include <QSizeGrip>
#include <QHash>
#include <QJsonObject>
#include <QTimer>
#include <QElapsedTimer>
#include <QEvent>
#include <atomic>
#include <functional>

class QAbstractSlider;
class QMediaDevices;
class QShowEvent;
class QSystemTrayIcon;

#include "gui/ClientEqApplet.h"   // ClientEqApplet::Path enum used in
                                   // onEqCutoffsDragRequested signature.
#include "gui/PersistentDialog.h" // showOrRaisePersistent template needs
                                   // PersistentDialog visible at point of use.

namespace AetherSDR {

class AetherClockApplet;
class AetherClockEngine;
class AetherClockModel;
class AutomationServer;
class ConnectionPanel;
class ContributeDialog;
class TitleBar;
class KiwiSdrManager;
class SpectrumWidget;
class PanadapterApplet;
class PanadapterStack;
class AdaptiveFilterEngine;
class AppletPanel;
class BandPlanManager;
class NetworkDiagnosticsHistory;
class WhatsNewDialog;
class ProfileManagerDialog;
class ProfileImportExportDialog;
class RadioSetupDialog;
class NetworkDiagnosticsDialog;
class AgcCalibrationDialog;
class MemoryDialog;
class NetSchedulerDialog;
class NetReminderBanner;
class NetScheduler;
struct NetEntry;
struct MemoryEntry;
class PropDashboardDialog;
class UpdateChecker;
class TxBandDialog;
class AetherDspDialog;
class MqttSettingsDialog;
class WaveformsDialog;
class DxClusterDialog;
class CallsignLookupDialog;
class Ax25HfPacketDecodeDialog;
class PskReporterMapDialog;
class GpsLocationDialog;
#ifdef AETHER_ASR_ENABLED
class CopyAssistController;
#endif
class FlexControlDialog;
class MidiMappingDialog;
#ifdef HAVE_HIDAPI
class RC28MappingDialog;
#endif
class UlanziDialMapperDialog;
#ifdef Q_OS_LINUX
class EvdevEncoderManager;
#elif defined(Q_OS_WIN) && defined(HAVE_HIDAPI)
class UlanziDialWindowsManager;
#elif defined(Q_OS_MAC)
class UlanziDialMacOSManager;
#else
class UlanziDialBackend;
#endif
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
class WfmDemodulator;

// Wheel mode for FlexControl: determines what the encoder knob adjusts.
//
// MasterAf was previously a separate enum value that routed identically
// to Volume (see issue #2986).  PR #2925 changed Volume to route to
// master-volume as well, making the two modes byte-identical.  MasterAf
// was removed; the "WheelMasterAf" action string is still accepted in
// flexWheelModeForAction() and mapped to Volume so saved FlexControl
// button bindings keep working.
enum class FlexWheelMode {
    Frequency,
    Volume,
    Power,
    Rit,
    Xit,
    SliceAudio,
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

    // Active-session RadioModel. Bound to the current session's model, so it
    // tracks Multi-Flex session switches. Used by the automation bridge
    // (#3646) to read live model state via get(); keep it read-oriented.
    RadioModel& radioModel() { return m_radioModel; }
    const RadioModel& radioModel() const { return m_radioModel; }
    AudioEngine* audioEngine() const { return m_audio; }
    QsoRecorder* qsoRecorder() const { return m_qsoRecorder; }  // automation bridge
    Q_INVOKABLE void showConnectionDialog();
    Q_INVOKABLE void hideConnectionDialog();
    // fireShortcutAction result codes. Plain ints (not enum class) because the
    // value crosses QMetaObject::invokeMethod as an int return.
    static constexpr int ShortcutFireOk              = 0;
    static constexpr int ShortcutFireUnknownId       = 1;
    static constexpr int ShortcutFireNoDirectHandler = 2;  // event-filter action (ptt_hold, CW keys)
    static constexpr int ShortcutFireTxBlocked       = 3;  // keysTx action, allowTx false
    // Fire a registered ShortcutManager action by id — the exact path a MIDI
    // controller mapping takes (see fireShortcut in MainWindow_Controllers.cpp).
    // Used by the automation bridge (#3646) to reach MIDI/shortcut-only actions
    // that carry no default key sequence and no menu entry. allowTx gates
    // actions registered keysTx (the caller decides policy; the registration
    // site declares the data). Returns a ShortcutFire* code.
    Q_INVOKABLE int fireShortcutAction(const QString& id, bool allowTx);
    // Inject one learned VFO-knob CC value through MidiControlManager for
    // automation proof. Returns 0 on acceptance, 1 if MIDI is unavailable,
    // and 2 for an out-of-range MIDI value.
    Q_INVOKABLE int injectMidiVfoCcForAutomation(int value);
    QJsonObject automationSetSliceReceiveSource(const QString& arg);
    QJsonObject automationSetCenterLock(int sliceId, bool enabled);
    QJsonObject automationTune(double mhz, int sliceId = -1);
    QJsonObject automationTargetTune(double mhz);
    QJsonObject automationActivateMemory(int memoryIndex,
                                         const QString& preferredPanId);
    QJsonObject automationReceiveSyncSnapshot() const;
    QJsonObject automationKiwiSdrSnapshot() const;
    // Status-bar TX-timer state for the bridge `get txtimer` verb.
    QJsonObject automationTxTimerSnapshot() const;

    // Agent automation bridge (#3646) lifecycle. Construction + full
    // handler wiring lives in startAutomationBridge() so it can be driven
    // both at launch (AETHER_AUTOMATION env var, from main.cpp) and at
    // runtime from the Radio Setup → Network toggle. Idempotent: starting
    // while running is a no-op; stopping while stopped is a no-op.
    // sockName empty → the default PID-suffixed name. Returns true if the
    // bridge is listening afterwards.
    bool startAutomationBridge(const QString& sockName = QString());
    void stopAutomationBridge();
    // Persist a new shared-secret token and push it to the running bridge
    // (the Radio Setup → Network rotate button). Old tokens stop working
    // immediately.
    void setAutomationBridgeToken(const QString& token);
    // Persist the TX-via-MCP opt-in and push it live (Radio Setup → Network).
    // Enabling arms the force-unkey watchdog; disabling force-unkeys the radio.
    void setAutomationTxAllowed(bool allowed);
    // Persist the observe-only opt-in and push it live (Radio Setup → Network).
    // When set, the bridge refuses every mutating verb (#4188 area 6).
    void setAutomationReadOnly(bool readOnly);

signals:
    // Synchronous per-pan preflight for every radio-authoritative band-stack
    // restore. wirePanadapter() owns the pending dBm handshake state, while the
    // restore can originate in several MainWindow translation units.
    void bandStackRestoreStarting(const QString& panId);

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
#if defined(Q_OS_WIN)
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void applyWindowsCustomFrame();
#endif

private slots:
    // Radio/connection events
    void onConnectionStateChanged(bool connected);
    void adjustCatPortCounts(bool connected);  // called from onConnectionStateChanged
    void onConnectionError(const QString& msg);
    // GHSA-wfx7-w6p8-4jr2 phase 2 (#2951): show mismatch modal and
    // forward the operator's decision back to the WanConnection.
    void onWanCertFingerprintMismatch(const QString& host,
                                      const QString& expectedHex,
                                      const QString& presentedHex);
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
    void updateStatusBarMinimumWidth();

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
    // Lock / SWR-sweep guards shared by every tune source.  Returns true if the
    // tune must be blocked (and, for a locked active slice, restores the VFO
    // readout).  Lets the edge-pan tune path — which bypasses applyTuneRequest
    // to avoid pan-follow — still honour the same lock/sweep affordances.
    bool tuneBlockedByGuards(SliceModel* slice);
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
    void pushSliceFrequencyToOverlays(SliceModel* slice, double mhz);
    static bool isSameDiversityReceivePair(const SliceModel* slice,
                                           const SliceModel* other);
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
    bool reattachSliceVisualsToPanadapter(SliceModel* s);
    void syncTxWaterfallSliceToSpectrums();
    void updateSplitState();
    void disableSplit();
    // Constructor wiring blocks extracted per #3351 Phase 2 — each runs once
    // from the constructor, in original order, defined in its subject TU.
    void wireMeters();              // MainWindow_Wiring.cpp
    void wireSpotSubsystem();       // MainWindow_Spots.cpp
    // RadioSession precursors (#3351 Phase 2c / #3445) — MainWindow_Session.cpp
    void wireDiscovery();
    void wireRadioModel();
    void wirePanLifecycle();
    void wireCatPorts();            // MainWindow_Session.cpp
    void wireDaxIq();               // MainWindow_Session.cpp
    void wirePooDooTiles();         // MainWindow_DspApplets.cpp
    void wireDspApplets();          // MainWindow_DspApplets.cpp
    void wireExternalControllers(); // MainWindow_Controllers.cpp
    void wireKiwiSdr();             // MainWindow_KiwiSdr.cpp
    void refreshKiwiSdrAppletReceivers();
    void refreshKiwiSdrSlices();
    void refreshKiwiSdrWaterfallAvailability();
    void syncKiwiSdrAppletWaterfallState();
    SliceModel* kiwiSdrAudioTargetSlice() const;
    bool setKiwiSdrAudioRouting(bool active);
    bool applyKiwiSdrSliceMute();
    void restoreKiwiSdrSliceMute();
    bool kiwiSdrTransmitMuteRequired() const;
    void syncKiwiSdrTransmitMute();
    void setKiwiSdrVirtualAntennaForSlice(int sliceId, const QString& profileId);
    void clearKiwiSdrVirtualAntennaForSlice(int sliceId);
    void updateKiwiSdrVirtualTrackingForSlice(SliceModel* slice);
    void updateKiwiSdrVirtualAudioControlsForSlice(SliceModel* slice);
    void updateKiwiSdrVirtualReceiverControlsForSlice(SliceModel* slice);
    SliceModel* flexRxPanSourceSlice() const;
    void syncFlexRxPanToAudioEngine();
    void syncActiveSliceSquelchLineToSpectrums();
    bool autoSquelchShouldRunOnSpectrum(const QString& panId,
                                        const SpectrumWidget* spectrum) const;
    void syncActiveSliceAutoSquelchToSpectrums();
    void initReceivePresentationSync(); // MainWindow_ReceiveSync.cpp
    void syncReceivePresentationDelaysToAudioEngine(
        bool clearVisualQueueOnAbruptDelayChange = true);
    ReceivePresentationSettings receivePresentationSettings() const;
    ReceiveDelayBreakdown receivePresentationDelayBreakdown() const;
    QString receivePresentationOverlayStatsText() const;
    void setReceivePresentationSyncEnabled(bool enabled);
    void setReceivePresentationSyncMode(ReceiveSyncMode mode);
    void adjustReceivePresentationManualOffsetMs(int deltaMs);
    void resetReceivePresentationManualOffset();
    void setReceivePresentationLatencyMs(int latencyMs);
    int receivePresentationDelayMs(
        ReceivePresentationSource source,
        ReceivePresentationSurface surface,
        const QString& sourceId = QString()) const;
    void deferReceivePresentation(ReceivePresentationSource source,
                                  ReceivePresentationSurface surface,
                                  std::function<void()> apply,
                                  const QString& sourceId = QString());
    bool receivePresentationHasUsableSyncTarget() const;
    void resetReceivePresentationAudioBuffers();
    void resetReceivePresentationAudioBuffersForKiwiSource(
        const QString& sourceId);
    void clearReceivePresentationVisualQueue();
    void clearReceivePresentationVisualQueueForSource(
        ReceivePresentationSource source,
        const QString& sourceId = QString());
    void scheduleReceivePresentationVisualQueue();
    void drainReceivePresentationVisualQueue();
    struct ReceiveSyncTarget {
        enum class State {
            None,
            Usable,
            Ambiguous,
        };
        State state{State::None};
        QString kiwiProfileId;
        int flexSliceId{-1};
        int kiwiSliceId{-1};
        qint64 frequencyHz{0};
        int audibleFlexCount{0};
        int audibleKiwiCount{0};
        int matchingPairCount{0};
        QString reason;

        bool usable() const { return state == State::Usable; }
        bool ambiguous() const { return state == State::Ambiguous; }
    };
    ReceiveSyncTarget resolveReceiveSyncTarget() const;
    QString receiveSyncKiwiProfileId() const;
    QString receiveSyncDelayKiwiProfileId() const;
    qint64 receiveSyncTunedFrequencyHz() const;
    void holdReceivePresentationAutoAssistLock(bool clearVisualQueue = true);
    void resetReceivePresentationAutoAssistState(bool clearEstimate,
                                                 bool clearVisualQueue = true);
    void feedReceivePresentationSyncAudio(ReceivePresentationSource source,
                                          const QByteArray& pcm24kStereoFloat,
                                          const QString& sourceId = QString(),
                                          int sampleRateHz =
                                              AudioEngine::DEFAULT_SAMPLE_RATE);
    void runReceivePresentationAutoAssist();
    void applyReceivePresentationAutoAssistEstimate(
        const ReceiveAudioDelayEstimate& estimate);
    SliceModel* kiwiSdrDisplaySliceForPan(const QString& panId) const;
    QString kiwiSdrProfileForPan(const QString& panId) const;
    QString kiwiSdrOverlayProfileForPan(const QString& panId) const;
    bool kiwiSdrPanDisplaysKiwi(const QString& panId) const;
    void setKiwiSdrPanDisplaySource(const QString& panId, bool kiwi);
    void clearKiwiSdrPanDisplaySourceOverride(const QString& panId);
    void clearKiwiSdrPanDisplaySourceOverrides();
    void syncKiwiSdrPanadapterTxInhibit(const QString& panId,
                                        const QString& profileId);
    void syncKiwiSdrDiversityEscControls();
    void syncKiwiSdrPanadapterUiState(const QString& panId);
    void syncKiwiSdrPanadapterUiStates();
    enum KiwiSdrUiSyncFlag {
        KiwiSdrUiSyncAppletReceivers = 0x01,
        KiwiSdrUiSyncWaterfallAvailability = 0x02,
        KiwiSdrUiSyncDiversityEsc = 0x04,
        KiwiSdrUiSyncPanadapterStates = 0x08,
    };
    void scheduleKiwiSdrUiSync(int flags);
    void wirePanadapter(PanadapterApplet* applet);
    void wirePanDisplayStatus(PanadapterApplet* applet, PanadapterModel* pan);
    void reassertUnmutedSliceAudioForPan(const QString& panId);
    void onMuteAllSlicesToggle();
    void showPanadapterInterlockNotification(const QString& message,
                                             const QString& key = QString(),
                                             const QString& panId = QString());
    void setActivePanApplet(PanadapterApplet* applet);
    void routeCwDecoderOutput();
    void refreshCwDecodeState();
    // QRZ callsign lookup (MainWindow_Callsign.cpp): CW-spotter → lookup
    // service → contact card on the CW decode panel + lookup dialog.
    void wireCallsignLookup();
    void onCwCallsignSpotted(const QString& call);
    void showCallsignLookupDialog(const QString& call = QString());
    void showGpsLocationDialog();
    void routeRttyDecoderOutput();
    void refreshRttyDecodeState();
    SpectrumWidget* spectrumForSlice(SliceModel* s) const;
    void wireVfoWidget(VfoWidget* w, SliceModel* s);
    void wireVfoTelemetry(VfoWidget* vfo, SliceModel* s);
    // Push the active RX slice's filter passband (converted from
    // protocol offsets to audio-domain low/high) to the RX EQ canvases.
    void pushRxFilterCutoffsToEq();
    void enableNr2WithWisdom();  // Wisdom-gated NR2 enable (shared by VFO + overlay)
    void updateNr2Availability(); // Disable NR2 when Opus is active (#1597)
    void registerShortcutActions();
    void applyUiScale(int pct);
    void stepUiScale(int direction);  // +1 = zoom in, -1 = zoom out
    void reapplyStartupGeometryAfterShow();
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
    // Toggle OS window-chrome on/off. Persists to AppSettings("FramelessWindow").
    // When on, TitleBar provides the drag surface and window-control buttons.
    void setFramelessWindow(bool on);
    void trackPersistentDialog(PersistentDialog* dialog);

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

    // Toggle helper for the AetherDSP Settings dialog: open it when hidden,
    // close it when visible.  Gives the per-slice DSP-tab ADSP button the same
    // press-to-open / press-again-to-close semantics as its sibling AetherVoice
    // button (#3877).  close() deletes the WA_DeleteOnClose dialog and clears
    // the QPointer, so the next press re-creates and re-wires via
    // ensureAetherDspDialog()'s wasFresh path.  Only the DSP-tab button toggles;
    // the menu action and chain/strip launchers keep pure open semantics.
    void toggleAetherDspDialog();

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

    // Net Reminder Scheduler (MainWindow_Nets.cpp).
    void initNetScheduler();
    void showNetSchedulerDialog();
    void persistNetSchedule(const QList<NetEntry>& nets);
    void tuneToNet(const NetEntry& entry);
    void onNetReminderDue(const NetEntry& entry, const QDateTime& occurrenceUtc);
    MemoryEntry captureCurrentNetPreset() const;
    QString netScheduleFilePath() const;
#ifdef HAVE_WEBSOCKETS
    void showFreeDvReporter();
#endif
    void updateKeyerAvailability();
    void showNr2ParamPopup(const QPoint& globalPos);
    void showNr4ParamPopup(const QPoint& globalPos);
    void showDfnrParamPopup(const QPoint& globalPos);
    void showMnrSettings();
#ifdef HAVE_MQTT
    void showMqttSettingsDialog();
    void publishCwDecodeMqtt(const QString& text, float cost, bool rx);
    void publishRadioStateMqtt();
#endif
    void applyPanLayout(const QString& layoutId);
    void createPansSequentially(const QString& layoutId, int total,
                                std::shared_ptr<QStringList> panIds, int created);
    void showPanadapterSliceCapacityMessage();
    void updatePaTempLabel();
    void showNetworkDiagnosticsDialog();
    void showAgcCalibrationDialog(int sliceId);
    void showAx25HfPacketDecodeDialog();
#ifdef AETHER_ASR_ENABLED
    void showCopyAssist();
#endif
    void scheduleDigitalVoiceAutoStart();
    void stopDigitalVoiceService(bool waitForExit);
    void showPskReporterMapDialog();
    void startKissTncOnStartupIfConfigured();
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
    // The lease holder is usually a QAbstractSlider, but MeterSlider (the
    // TCI/DAX combined meter+gain fader) is a plain QWidget that handles its
    // own keys, so the lease is typed as QWidget* and frees the arrow keys
    // for whichever control is focused.
    void beginSliderShortcutLease(QWidget* slider);
    void renewSliderShortcutLease();
    void releaseSliderShortcutLease(bool clearFocus);

    BandSnapshot captureCurrentBandState() const;
    void restoreBandState(const BandSnapshot& snap);
    void startSwrSweep(int requestedSliceId = -1, int sweepPowerWatts = 1,
                       double customLowMhz = 0.0, double customHighMhz = 0.0);
    void clearSwrSweepPlot();
    void saveSwrSweepCsv();
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
    // PTT (Hold) shortcut: resolve the bound key via ShortcutManager (not a
    // hardcoded Qt::Key_Space) so a reassigned PTT-hold key actually keys the
    // radio. Returns true when the bound key was consumed (#3879).
    bool handlePttHoldShortcut(QKeyEvent* keyEvent, QEvent::Type eventType);
    // Fail-safe-to-RX for the momentary-keying family (PTT-hold, CW straight
    // key / paddles). Called when the window/app is deactivated while a
    // momentary key is "held" in our state — the KeyRelease that would un-key
    // goes to whatever now has focus and never reaches our filter, so without
    // this the transmitter stays keyed. Clears every momentary flag and issues
    // the matching un-key. No-ops when nothing is active (#3888, Principle VI).
    void failSafeMomentaryKeyingToRx(const char* reason);
    // True when ev's physical key equals the base key (modifiers stripped) of
    // the action's current binding. Lets a held momentary key un-key on its
    // KeyRelease even when a modifier of a combo binding was released first
    // (#3888, Principle VI).
    bool keyEventMatchesActionBaseKey(const char* actionId,
                                      const QKeyEvent* ev);

    // Core objects
    RadioDiscovery    m_discovery;
    // Radio sessions (#3445 Camp B / #3351). Each session owns the full
    // per-radio aggregate; today there is exactly one. The vector sits at
    // the old `RadioModel m_radioModel` member position so destruction
    // order relative to the surrounding members is unchanged.
    std::vector<std::unique_ptr<RadioSession>> m_sessions;
    RadioSession*     m_session{nullptr};  // active session (m_sessions.front())
    // Alias into the active session's model. Keeps the ~900 m_radioModel
    // call sites across the MainWindow TUs source-compatible; new code
    // should prefer m_session->radioModel().
    RadioModel&       m_radioModel;
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
    std::unique_ptr<AutomationServer> m_automation;  // agent bridge (#3646); nullptr when off
    ClientPuduMonitor* m_finalMonitor{nullptr};
    AudioOutputRouter* m_outputRouter{nullptr};   // registry for output-following sinks (#3306)
    BandSettings      m_bandSettings;
    // CAT ports: up to 8 unified ports (rigctld / TS-2000 / FlexCAT), one per slice.
    // CAT ports are owned by the active RadioSession (#3351 session v2).
    static constexpr int kCatPorts = RadioSession::kCatPorts;
    CatPort* catPort(int i) const { return m_session->catPort(i); }

    // Returns how many CAT ports should be visible in the UI given radio state.
    // 1 when no radio; maxSlicesForModel() when connected.
    int catPortTargetCount() const;
    // Start/stop ports to match CatEnabled master + per-port Enabled flags.
    void applyCatPortCount();
    // One-time settings migration from the old dual-server key schema.
    void migrateCatSettings();
#ifdef HAVE_WEBSOCKETS
    // TciServer is owned by the active RadioSession (#3351 session v2).
    TciServer* tciServer() const { return m_session->tciServer(); }
    FreeDvReporterDialog*  m_freedvReporterDialog{nullptr};
#endif
    SmartLinkClient   m_smartLink;
    WanConnection     m_wanConnection;
    AntennaGeniusModel m_antennaGenius;
    TgxlConnection    m_tgxlConn;        // direct TCP 9010 to TGXL for manual relay control
    PgxlConnection    m_pgxlConn;        // direct TCP 9008 to PGXL for telemetry
    AcomConnection    m_acomConn;        // ACOM S-series amplifier, serial or ser2net
    BandPlanManager*  m_bandPlanMgr{nullptr};
    CwDecoder         m_cwDecoder;
    float             m_cwLastPitchHz{0.0f};
    float             m_cwLastSpeedWpm{0.0f};
    CwDecoder         m_cwDecoderTx;
    CwCallsignSpotter m_cwCallsignSpotter;
    RttyDecoder       m_rttyDecoder;
    DxClusterClient*   m_dxCluster{nullptr};
    DxClusterClient*   m_rbnClient{nullptr};
#ifdef HAVE_MQTT
    MqttClient*        m_mqttClient{nullptr};
    QMetaObject::Connection m_radioStateFreqConn;
    QMetaObject::Connection m_radioStateModeConn;
    QTimer                  m_radioStateCoalesceTimer;
    QMetaObject::Connection m_cwStatsConn;
    QMetaObject::Connection m_cwxSpeedRestoreConn;
    int               m_cwxSavedWpm{0};
    int               m_cwxSavedHz{0};
    int               m_cwxSentWpm{0};
    int               m_cwxSentHz{0};
    bool              m_cwxTransmitting{false};
    bool              m_cwxPublishedTxTrue{false};
    QTimer            m_cwxTxEndTimer;
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
    // Adaptive RX filter — drive the fit engine off the same FFT frames (RFC #3878).
    void onSpectrumReadyForAdaptiveFilter(quint32 streamId, const QVector<float>& bins, qint64 emittedNs);
    AdaptiveFilterEngine* m_adaptiveFilterEngine{nullptr};  // parented to this
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
    // (No client-side band/segment zoom bools: the toggles read the pan's
    // radio-authoritative model state — togglePanZoomModeForPan, #4057.)
    void togglePanZoomMode(bool segmentZoom);
    void togglePanZoomModeForPan(const QString& panId, bool segmentZoom);
#ifdef HAVE_HIDAPI
    HidEncoderManager*   m_hidEncoder{nullptr};
    static QString hidEncoderDefaultAction(int encoderIndex);
    static QString hidEncoderDefaultPushAction(int encoderIndex);
    void refreshStreamDeckLabels();
    void updateRC28Leds();
    bool rc28HoldActionActive(const QString& action) const;
    void dispatchHidAction(const QString& actionName, const QString& gestureLabel);
    QMetaObject::Connection m_sdRitConn;
    QMetaObject::Connection m_sdXitConn;
    // RC-28 F-key LED refresh, rewired to the active slice on each slice change
    QMetaObject::Connection m_rc28RitConn;
    QMetaObject::Connection m_rc28XitConn;
    QMetaObject::Connection m_rc28LockConn;
    // Hold-detection state for RC-28 F1/F2 — one slot per key so the two can be
    // held independently without clobbering each other. Index 0 = F1, 1 = F2. (#3323)
    QTimer* m_rc28HoldTimer[2]{nullptr, nullptr};
    bool    m_rc28HoldConsumed[2]{false, false};
    // RC-28 stateful action flags
    bool    m_rc28PttLatched{false};
    uint8_t m_lastRC28LedByte{0xFF};  // last byte sent; 0xFF forces first write
    bool    m_hidFastTune{false};
    bool    m_hidFineTune{false};
    int     m_hidPulseAccum{0};     // accumulated RC-28 encoder pulses for sensitivity divider
    int     m_hidSensitivity{1};    // RC-28 pulses required per frequency step (1 = off)
    bool    m_hidAutoSnap{false};   // snap to nearest 1 kHz after rotation stops
    QTimer* m_hidSnapTimer{nullptr};
    enum class TMate2Overlay { None, Volume, Power, Speed, Wpm, Rit, Xit, Shift, Agc, Apf, Text };
    TMate2Overlay m_tmate2Overlay{TMate2Overlay::None};
    int     m_tmate2OverlayValue{0};
    QString m_tmate2OverlayText;
    qint64  m_tmate2OverlayUntilMs{0};
    qint64  m_tmate2LastUserInteractionMs{0};
    bool    m_tmate2DisplayBlanked{false};
    QTimer  m_tmate2OverlayTimer;
    QTimer  m_tmate2IdleTimer;
    // Last S-meter reading (dBm) and TX power (watts) from the active slice;
    // cached so updateTMate2Display/Indicators() can re-send without signal args.
    float   m_tmate2SmeterDbm{-140.0f};
    float   m_tmate2TxWatts{0.0f};
    bool tmate2OverlayActive() const;
    QString tmate2OverlayName() const;
    int tmate2IdleTimeoutMs() const;
    void restartTMate2IdleTimer();
    void noteTMate2Interaction();
    void blankTMate2Display();
    void triggerTMate2Overlay(TMate2Overlay overlay, int value);
    void triggerTMate2TextOverlay(const QString& text);
    void updateTMate2Display();
    void updateTMate2Status();
    void updateTMate2Indicators();
    // Per-slice connections rewired in setActiveSlice() so TMate 2 indicators
    // track mode, RIT, XIT, and lock changes on the active slice.
    QMetaObject::Connection m_tmate2LockConn;
    QMetaObject::Connection m_tmate2ModeConn;
    QMetaObject::Connection m_tmate2RitConn;
    QMetaObject::Connection m_tmate2XitConn;
#endif
#ifdef Q_OS_LINUX
    EvdevEncoderManager*       m_dialBackend{nullptr};
#elif defined(Q_OS_WIN) && defined(HAVE_HIDAPI)
    UlanziDialWindowsManager*  m_dialBackend{nullptr};
#elif defined(Q_OS_MAC)
    UlanziDialMacOSManager*    m_dialBackend{nullptr};
#else
    UlanziDialBackend*         m_dialBackend{nullptr};
#endif
    QTimer                     m_dialCoalesceTimer;
    int                        m_dialPendingSteps{0};
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
    QPointer<PanadapterApplet> m_cwDecoderApplet;
    QPointer<PanadapterApplet> m_rttyDecoderApplet;

    // GUI — right applet panel
    AppletPanel*     m_appletPanel{nullptr};
    KiwiSdrManager*  m_kiwiSdrManager{nullptr};
    int              m_kiwiSdrUiSyncFlags{0};
    bool             m_kiwiSdrUiSyncPending{false};
    int              m_kiwiSdrTrackedSliceId{-1};
    int              m_kiwiSdrAudioSliceId{-1};
    bool             m_kiwiSdrAudioPreviousMute{false};
    bool             m_kiwiSdrAudioMuteApplied{false};
    bool             m_kiwiSdrAudioMuteChanging{false};
    bool             m_kiwiSdrAudioTransmitMuted{false};
    QMetaObject::Connection m_kiwiSdrAudioMuteConnection;
    QHash<int, bool> m_kiwiSdrVirtualPreviousMute;
    QSet<QString>    m_kiwiSdrFlexDisplayPans;
    // Retains a KiwiSDR replacement across the slice remove->re-add a FLEX
    // band-stack recall performs (band_persistence drops+re-creates the slice
    // with the same id at the new band). The tracker is the pure policy; the
    // grace window itself is a QTimer::singleShot in onSliceRemoved. (#4158)
    KiwiRebindTracker    m_kiwiRebind;
    static constexpr int kKiwiSdrRebindGraceMs = 1500;
    ReceivePresentationSync m_receivePresentationSync;
    ReceiveAudioDelayEstimator m_receiveAudioDelayEstimator;
    ReceivePresentationQueue<std::function<void()>> m_receivePresentationVisualQueue;
    QHash<QString, qint64> m_receivePresentationVisualLastDueMs;
    QTimer* m_receivePresentationVisualTimer{nullptr};
    quint64 m_receivePresentationVisualSequence{0};
    int m_receivePresentationLastFlexAudioDelayMs{-1};
    int m_receivePresentationLastKiwiAudioDelayMs{-1};
    QVector<float> m_receiveSyncFlexAudio;
    QVector<float> m_receiveSyncKiwiAudio;
    QString m_receiveSyncKiwiProfileId;
    QElapsedTimer m_receiveSyncEstimateTimer;
    QElapsedTimer m_receiveSyncDriftTimer;
    quint64 m_receiveSyncEstimateGeneration{0};
    bool m_receiveSyncEstimateInFlight{false};
    int m_receiveSyncLastEstimateOffsetMs{0};
    int m_receiveSyncStableEstimateCount{0};
    ReceiveAudioDelayEstimate m_receiveSyncLastCandidate;
    int m_receiveSyncLastCandidateAbsoluteOffsetMs{0};
    bool m_receiveSyncLastCandidateAvailable{false};
    bool m_receiveSyncLastAcceptedLock{false};
    bool m_receiveSyncLastNearAppliedLock{false};
    bool m_receiveSyncLastFarRelockEligible{false};
    qint64 m_receiveSyncLastFrequencyHz{0};
    bool m_receiveSyncHaveLastEstimate{false};
    bool m_receiveSyncTargetUnavailable{false};

    // Modeless dialogs
    QPointer<DxClusterDialog> m_spotHubDialog;
    QPointer<CallsignLookupDialog> m_callsignLookupDialog;
    QPointer<RadioSetupDialog> m_radioSetupDialog;
    QPointer<NetworkDiagnosticsDialog> m_networkDiagnosticsDialog;
    QPointer<AgcCalibrationDialog> m_agcCalibrationDialog;
    QPointer<PropDashboardDialog> m_propDashboardDialog;
    QPointer<TxBandDialog> m_txBandDialog;
    QPointer<MemoryDialog> m_memoryDialog;
    QPointer<NetSchedulerDialog> m_netSchedulerDialog;
    NetScheduler* m_netScheduler{nullptr};
    NetReminderBanner* m_netReminderBanner{nullptr};
    QSystemTrayIcon* m_trayIcon{nullptr};
    QPointer<Ax25HfPacketDecodeDialog> m_ax25HfPacketDecodeDialog;
#ifdef AETHER_ASR_ENABLED
    QPointer<CopyAssistController> m_copyAssistController;
    QPointer<PanadapterApplet> m_copyAssistApplet;
    QMetaObject::Connection m_copyAssistFreqConn; // active-slice retune → clear decode
#endif
    QPointer<PskReporterMapDialog> m_pskReporterMapDialog;
    QPointer<GpsLocationDialog> m_gpsLocationDialog;
    QPointer<FlexControlDialog> m_flexControlDialog;
    QPointer<WhatsNewDialog> m_whatsNewDialog;
    QPointer<ContributeDialog> m_contributeDialog;
    QPointer<AetherDspDialog> m_dspDialog;
#ifdef HAVE_MQTT
    QPointer<MqttSettingsDialog> m_mqttSettingsDialog;
#endif
    QPointer<WaveformsDialog> m_waveformsDialog;
    QPointer<ProfileManagerDialog> m_profileManagerDialog;
    QPointer<ProfileImportExportDialog> m_profileImportExportDialog;
#ifdef HAVE_MIDI
    QPointer<MidiMappingDialog> m_midiDialog;
#endif
#ifdef HAVE_HIDAPI
    QPointer<RC28MappingDialog> m_rc28MappingDialog;
#endif
    QPointer<UlanziDialMapperDialog> m_ulanziMapperDialog;

    // Tracks PersistentDialogs so setFramelessWindow() can propagate the
    // frameless toggle without explicit per-dialog branches. Registration
    // prunes null and duplicate QPointers so repeated close/reopen cycles do
    // not grow the list when the frameless setting is never toggled.
    QList<QPointer<PersistentDialog>> m_persistentDialogs;

    // Menus
    QMenu*           m_profilesMenu{nullptr};
    QAction*         m_txBandAction{nullptr};

    // Audio stream re-creation flag (after profile load)
    bool             m_needAudioStream{false};
    qint64           m_profileLoadRadioStateWriteHoldUntilMs{0};
    QSet<QString>    m_pendingProfileLoadPanDimensions;
    QHash<QString, int> m_profileLoadPendingFftYpixels;
    QHash<QString, qint64> m_profileLoadPanDimensionsSettlingUntilMs;

    // Pending WAN radio (between requestConnect and connectReady)
    WanRadioInfo     m_pendingWanRadio;
    QTimer           m_wanReconnectTimer;
    bool             m_wanReconnectAttemptInProgress{false};

    // Status bar labels (SmartSDR-style)
    QLabel* m_connStatusLabel{nullptr};   // hidden, used for connection state logic
    QLabel* m_addPanLabel{nullptr};
    QLabel* m_tnfIndicator{nullptr};
    QLabel* m_cwxIndicator{nullptr};
#ifdef AETHER_ASR_ENABLED
    QLabel* m_asrIndicator{nullptr};  // status-bar ASR (Copy Assist) toggle
#endif
    CwxPanel* m_cwxPanel{nullptr};
    DvkPanel* m_dvkPanel{nullptr};
    QLabel* m_dvkIndicator{nullptr};
    QLabel* m_fdxIndicator{nullptr};
    QLabel* m_radioInfoLabel{nullptr};
    QLabel* m_radioVersionLabel{nullptr};
    QLabel* m_stationLabel{nullptr};
    QLabel* m_stationNickLabel{nullptr};
    QLabel* m_automationChip{nullptr};    // shown only under AETHER_AUTOMATION (#3646)
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
    QLabel*  m_tgxlSeparator{nullptr};
    QWidget* m_tgxlContainer{nullptr};
    QLabel*  m_tgxlIndicator{nullptr};   // top row: "TUN"
    QLabel*  m_tgxlStateLabel{nullptr};  // bottom row: OPERATE / BYPASS / STANDBY
    QLabel*  m_pgxlSeparator{nullptr};
    QWidget* m_pgxlContainer{nullptr};
    QLabel*  m_pgxlIndicator{nullptr};   // top row: "AMP"
    QLabel*  m_pgxlStateLabel{nullptr};  // bottom row: OPERATE / STANDBY
    QLabel* m_txIndicator{nullptr};
    QLabel* m_gpsDateLabel{nullptr};
    QLabel* m_gpsTimeLabel{nullptr};
    QWidget* m_statusBarContainer{nullptr};

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
    QPointer<class ThemeEditorDialog> m_themeEditorDialog; // Phase 5 — lazy, modeless
    void cancelTransmitFromIndicator();
    void beginProfileLoadRadioStateWriteHold(const QString& profileType, const QString& profileName);
    bool profileLoadRadioStateWritesHeld() const;
    bool profileLoadPanDisplaySettling(const QString& panId) const;
    void holdNoiseFloorAutoAdjustForProfileLoad(qint64 untilMs);
    void reacquireNoiseFloorLocksAfterProfileLoad();
    void releaseProfileLoadPanDisplayHold(const QString& panId, SpectrumWidget* sw);
    void markProfileLoadPanDimensionsReady(const QString& panId, int yPixels);
    bool profileLoadPanDimensionsMatchExpected(const QString& panId,
                                               SpectrumWidget* sw) const;
    void retryProfileLoadPanDimensions(const QString& panId, SpectrumWidget* sw);
    void scheduleProfileLoadRecovery(const QString& profileType, const QString& profileName);
    void runProfileLoadRecoveryPass(const QString& profileType, const QString& profileName,
                                    bool rearmDaxIq, bool resetDaxRxStreams);
    void requestPanDimensionsForRadio(const QString& panId,
                                      SpectrumWidget* sw,
                                      bool updateLocalDecoderImmediately = false);
    void sendPanDimensionsToRadio(const QString& panId,
                                  SpectrumWidget* sw,
                                  bool updateLocalDecoderImmediately);
    void flushPendingProfileLoadPanDimensions();
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
    bool m_displaySettingsPushed{false};  // one-shot: push client-rendered settings after pan creation
    bool m_applyingLayout{false};        // true during layout tear-down/recreate — suppresses panadapterAdded handler
    // Live radio status drives the four profile-owned processing controls.
    // Keeping the connections per pan lets reconnect/reclaim replace them
    // atomically without accumulating duplicate status handlers.
    QHash<QString, QVector<QMetaObject::Connection>> m_panDisplayStatusConnections;
    bool m_adaptiveThrottleActive{false}; // fps/wf status held as restore targets while true
    int  m_adaptiveFpsCap{0};             // current cap (> 0 when throttle active); shown in network label
    QTimer* m_layoutRestoreTimer{nullptr}; // debounced layout rearrange after pans added on connect
    qint64 m_layoutRestoreUntilMs{0};
    // User layout choices should suppress startup rearrange, but still allow
    // the pending timer to restore saved floating pan windows.
    bool m_suppressStartupPanLayoutRearrange{false};
    QTimer* m_heartbeatMissTimer{nullptr}; // fires every 1.5s to detect missed discovery beats
    QTimer* m_bsExpiryTimer{nullptr};    // band-stack bookmark auto-expiry, started on connect only (#1471)
    QTimer* m_bsAutoSaveTimer{nullptr};  // band-stack dwell auto-save (single-shot per dwell window)
    QTimer* m_agManualConnectTimer{nullptr}; // deferred AG manual connect — cancelled on disconnect
    std::unique_ptr<class CwxLocalKeyer> m_cwxLocalKeyer;  // local Morse keyer for CWX sidetone (own worker thread)
    std::unique_ptr<class IambicKeyer> m_iambicKeyer;  // local iambic state machine for paddle sidetone
    std::atomic<quint64> m_lastCwPaddleTraceId{0};
    std::atomic<quint64> m_lastCwPaddleSourceMs{0};
    qint64 m_bsConnectGraceUntilMs{0};   // suppress auto-save right after connect
    bool m_keyboardShortcutsEnabled{false}; // global enable for keyboard shortcuts (View menu)
    bool m_pttHoldActive{false};           // true while the PTT-hold key is held (#3879)
    bool m_cwStraightKeyActive{false};
    bool m_cwLeftPaddleActive{false};
    bool m_cwRightPaddleActive{false};
    QPointer<QWidget> m_sliderShortcutLease;
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
    bool m_startupGeometryReapplied{false};
    QByteArray m_startupGeometryForFirstShow;
    QAction* m_minimalModeAction{nullptr};
    bool m_panadapterConnectionAnimationVisible{false};
    bool m_waitingForFirstPanadapterFrame{false};
    QString m_panadapterConnectionAnimationLabel;
    ShortcutManager m_shortcutManager;
    UpdateChecker* m_updateChecker{nullptr};

// AetherClock (MainWindow_AetherClock.cpp)
    AetherClockEngine* m_clockEngine{nullptr};
    AetherClockModel* m_clockModel{nullptr};
    QMetaObject::Connection m_clockDaxConn;  // daxAudioReady feed — live only while the engine runs
    void setupAetherClock();

#ifdef HAVE_RADE
    RADEEngine* m_radeEngine{nullptr};
    QThread*    m_radeThread{nullptr};
    int  m_radeSliceId{-1};
    bool m_radePrevMute{false};
    int m_radeDaxChannel{0};  // DAX channel RADE holds via PanadapterStream (#3305)
    QMetaObject::Connection m_radeDaxReconcileConn;  // RADE slice dax= change → move the Rade hold
    QMetaObject::Connection m_freedvMoxConn;
    QMetaObject::Connection m_radeMoxFallbackConn;
    QString m_lastRadeRxCallsign;
    bool m_radeEooPending{false};
    bool m_radeTxActive{false};
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

    // Center Lock — per-pan mode that keeps a selected slice centered while
    // tuning, so the pan/waterfall scrolls underneath it.
    QHash<QString, int> m_centerLockSliceByPan;  // panId -> sliceId
    // Persist the client-side intent by radio + display slot + slice letter.
    // Radio pan/slice IDs remain radio-authoritative and are never saved.
    QHash<QString, QHash<int, QString>> m_centerLockSliceLetterByRadioPanIndex;
    struct CenterLockTuneHold {
        double targetMhz{0.0};
        qint64 untilMs{0};
    };
    QHash<int, CenterLockTuneHold> m_centerLockTuneHoldBySlice;
    // While a slice is being dragged (in-window tune OR edge auto-pan) Center Lock
    // stands down so it doesn't fight the drag with per-tick recenters. The
    // drag-end handler recenters once so the locked pan re-asserts.
    bool m_sliceDragInProgress{false};
    int m_sliceDragTargetSliceId{-1};
    double m_sliceDragTargetMhz{0.0};
    qint64 m_sliceDragEchoHoldUntilMs{0};
    int centerLockSliceForPan(const QString& panId) const;
    bool centerLockActiveForSlice(const SliceModel* slice) const;
    void loadCenterLockSettings();
    void saveCenterLockSettings() const;
    void persistCenterLockForSlice(const SliceModel* slice);
    void restoreCenterLockForPan(const QString& panId);
    void setCenterLockForSlice(SliceModel* slice, bool on);
    void setCenterLockForPan(const QString& panId, int sliceId, bool on,
                             bool persist = true);
    void clearCenterLockForPan(const QString& panId, bool clearPersistedIntent = false);
    void clearCenterLockForSlice(int sliceId, bool clearPersistedIntent = false);
    // serial + "/" + station: per-client persistence identity so co-located
    // MultiFlex instances don't inherit or clobber each other's lock intent.
    QString centerLockRadioKey() const;
    void syncCenterLockUi(const QString& panId);
    bool snapCenterLockForSlice(SliceModel* slice, double mhz, bool sendCommand);
    void snapCenterLocksForTuningSlice(SliceModel* slice, double mhz,
                                       bool sendCommand);
    void holdCenterLockTuneTarget(SliceModel* slice, double mhz);
    double centerLockDisplayFrequency(const SliceModel* slice, double mhz) const;
    void recenterCenterLockForPan(const QString& panId);
    void recenterCenterLocks();

    WfmDemodulator* m_wfmDemod{nullptr};
    int             m_wfmSliceId{-1};
    bool            m_wfmCooldown{false};
    int             m_wfmPrevFilterLo{0};
    int             m_wfmPrevFilterHi{0};
    QMetaObject::Connection m_wfmFreqConn;
    void activateWFM(int sliceId);
    void deactivateWFM();
    // Push the real WFM demod state onto both UI surfaces (the slice's
    // VfoWidget WFM button and the RxApplet WFM button) so they never lie
    // about whether the demod is running, regardless of which surface (or a
    // mode change) toggled it.
    void reflectWfmButtons(bool on, int sliceId);

#if defined(Q_OS_MAC) || defined(HAVE_PIPEWIRE)
    DaxBridge* m_daxBridge{nullptr};
    QString m_savedMicSelection;  // restore on stopDax
    bool startDax();
    void stopDax();
    // #2895: react to per-slice DAX channel (re)assignment while the bridge is
    // up so the radio registers a DAX client for slices 1-3, not just slice 0.
    void wireDaxSlice(SliceModel* slice);
    void onDaxChannelChanged(SliceModel* slice, int newCh);
    QList<QMetaObject::Connection> m_daxSliceConns;
    QHash<int, int> m_daxSliceLastCh;  // sliceId -> last-known DAX channel
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
        trackPersistentDialog(dlg);
    }
    slot->show();
    slot->raise();
    slot->activateWindow();
}

} // namespace AetherSDR
