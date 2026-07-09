#pragma once

#include <QObject>
#include <QHash>
#include <QPointer>
#include <QByteArray>
#include <QElapsedTimer>
#include <QMutex>
#include <QJsonObject>
#include <QString>

#ifdef HAVE_WEBSOCKETS
class QWebSocket;
#endif

#include <deque>
#include <functional>
#include <memory>
#include <vector>

#include "IConnectionAutomation.h"  // complete type: inline setter calls asQObject()

class QLocalServer;
class QLocalSocket;
class QWidget;
class QTimer;

namespace AetherSDR {

class RadioModel;
class AudioEngine;
class QsoRecorder;

// In-app, agent-first automation bridge (issue #3646, Phases 0-1).
//
// Exposes a tiny line/JSON command channel over a QLocalServer so an external
// agent can introspect, drive, and capture the GUI without driving OS
// accessibility APIs or pixel-hunting through VNC. It is *off* in production
// and only starts when the AETHER_AUTOMATION environment variable is set, so
// it adds no attack surface or overhead to normal runs.
//
// Phase 0 verbs (read-only introspection + capture):
//
//   dumpTree                       -> ARIA-style JSON snapshot of every
//                                     top-level QWidget hierarchy (objectName,
//                                     class, accessibleName, role/value,
//                                     enabled, visible, global geometry).
//   grab <target> [path]           -> PNG capture of a single widget, resolved
//                                     by objectName, class name, or
//                                     accessibleName. Reads back the GPU
//                                     framebuffer for the QRhi panadapter so
//                                     the live spectrum is captured correctly.
//
// Phase 1 verbs (drive + assert):
//
//   invoke <target> <action> [v]   -> drive a control deterministically:
//                                     click / toggle / setChecked / setValue /
//                                     setText / setCurrentText / setCurrentIndex /
//                                     selectRow. SAFETY: refuses any control
//                                     marked as transmit-keying (markTxKeying() /
//                                     the "aetherTxKeying" property — MOX/PTT,
//                                     TUNE, ATU, CWX send, packet/APRS send)
//                                     unless AETHER_AUTOMATION_ALLOW_TX is set, so
//                                     the bridge can never key a live radio by
//                                     accident. A button-scoped name heuristic is
//                                     a logged fallback, kept narrow (mox/ptt/
//                                     transmit/cwx) so RX-only buttons like "Tune
//                                     Now" aren't false-blocked (#3918); setpoint
//                                     sliders/combos are never blocked.
//   invoke <view> selectRow <n>    -> select row n of an item view (QTableWidget /
//                                     QTreeWidget / QListWidget) so the dialog's
//                                     row-scoped buttons (Tune/Edit/Remove/Disable)
//                                     become drivable; echoes selectedRow[Text].
//   get <model> [selector] [prop]  -> live JSON snapshot of a model:
//                                     audio | dsp | radio | transmit |
//                                     slice <id|active|tx> | slices |
//                                     pan <panId|active> | pans |
//                                     kiwi. With a trailing property name,
//                                     returns just that field.
//                                     Assert on state without screenshots.
//                                     `dsp` is the client-side AetherDSP state:
//                                     the six AudioEngine noise-reduction modules
//                                     (NR2/NR4/MNR/DFNR/RN2/BNR) with active
//                                     method, per-module enabled/available, and
//                                     tuning values — the client-side counterpart
//                                     to the radio-side nr/nb/anf in `get slice`.
//   connect list                   -> list currently discovered local radios
//   connect show                   -> show/raise the Connect to Radio dialog
//   connect hide                   -> hide the Connect to Radio dialog
//   connect local first            -> request a real local-radio connection via
//                                     ConnectionPanel/MainWindow/RadioModel
//   connect local serial <serial>  -> same, selecting by discovered serial
//   connect ip <host-or-ip>        -> route through the manual Connect by IP
//                                     probe path, then connect if the probe finds
//                                     a radio
//   connect wait <timeout_ms>      -> hold the response until RadioModel reports
//                                     connected or the timeout expires
//   disconnect                     -> request the normal user disconnect path
//
// Phase 2 verbs (fidelity — reach code paths invoke/get can't, #3646):
//
//   invoke <le> submit [value]     -> commit a QLineEdit: optional setText then
//                                     fire returnPressed (the retune/login/send
//                                     trigger). setText alone stays side-effect-
//                                     free, so a plain value-set never logs in /
//                                     connects / sends to a live cluster.
//   invoke <label> trigger         -> now resolves a QAction anywhere in the menu
//                                     bar even while its menu is CLOSED, so
//                                     menu-launched dialogs (AetherControl…,
//                                     Network…, MQTT…, Radio Setup…, Connect…)
//                                     and Zoom are drivable headlessly.
//   slice tx <id>                  -> make slice <id> the TX slice (the external-
//                                     split transition). Set-only, radio-auth.
//   key ptt on|off | key mox       -> drive PTT / MOX via the model — the space-
//                                     bar PTT filter and mox_toggle shortcut that
//                                     invoke can't target. KEYING is gated by
//                                     AETHER_AUTOMATION_ALLOW_TX (unkey is not).
//   station <name>                 -> set the per-GUI-client station name shown
//                                     to other MultiFlex clients (never the radio
//                                     callsign). Auto-applied to the agent name
//                                     on connect, restored on stop.
//   resize <w> <h> [target]        -> resize a top-level window (default full
//                                     size) so the panadapter x_pixels reaches a
//                                     realistic value for headless render tests.
//   window <state> [target]        -> drive a window's state: maximize | restore
//                                     | minimize | fullscreen. resize only set
//                                     explicit geometry, so an un-maximize was
//                                     unprovable; dumpTree now carries
//                                     `windowState` to assert it (#3918).
//   menu list | menu open <name>   -> enumerate the menu bar / pop a menu for a
//                                     follow-up grab/dumpTree.
//   whoami                         -> {pid, socket, label, station} — identify
//                                     THIS instance among concurrent bridges.
//
// Phase 2b verbs (observability + reach, this batch — #3646):
//
//   grab pan <index> [path]        -> capture a SPECIFIC pan's raw spectrum
//                                     surface (by SpectrumWidget::panIndex) in
//                                     a multi-pan layout; plain `grab
//                                     SpectrumWidget` only ever returns the
//                                     first one.
//   grab pan-visible <index> [path]-> capture the operator-visible pan applet,
//                                     including VFO/flag child overlays above
//                                     the GPU surface.
//   close <target>                 -> close the target's top-level window
//                                     (deferred; reaches the frameless title-bar
//                                     close that invoke-click can't).
//   drag <target> <dx> <dy>        -> synthesize press→move→release so resize
//                                     grips / slider handles are provable end-to-
//                                     end. `mouse` is an alias.
//   showMenu <target>              -> pop a QToolButton/QPushButton drop-down,
//                                     posted onto the GUI loop with the window
//                                     raised (crash-safe on backgrounded macOS).
//                                     `openMenu` is an alias.
//   contextMenu <target> [x y]     -> trigger a custom right-click context menu
//                                     (CustomContextMenu / overridden
//                                     contextMenuEvent) via a synthesized
//                                     QContextMenuEvent; deferred, then dumpTree
//                                     to read it and invoke to drive it.
//   hitTest <target> [x y]         -> report Qt's widgetAt()/childAt() owner for
//                                     a target-local point. Read-only proof for
//                                     transparent overlays and input masks.
//   pan add                        -> create a new panadapter (panafall); the
//                                     only UI path is an unaddressable QLabel.
//   pan close <id|index|active|all>-> tear down a panadapter regardless of how it
//                                     was opened (sends display pan remove AND
//                                     display panafall remove).
//   panmessage add|remove|clear|list
//                                  -> inject/read panadapter overlay messages
//                                     for deterministic UI screenshots; add
//                                     accepts tone=info|warning, timed messages
//                                     expose countdown in snapshots.
//   dss snapshot|reset|inject|scrollback|live
//                                  -> automation-only 3D stacked-trace /
//                                     waterfall scrollback proof surface.
//                                     Injects synthetic RX rows through the
//                                     normal SpectrumWidget row paths and reads
//                                     compact DSS/waterfall counters.
//   dumpTree (extended)            -> nodes now carry toolTip, and QComboBox
//                                     nodes carry items[]/currentIndex and pans
//                                     carry panIndex, all assertable without
//                                     stepping a control. A checkable button
//                                     also carries its text + a checked bool, so
//                                     the six DSP method buttons (NR2 … BNR) are
//                                     identifiable and readable from the tree
//                                     instead of every one reporting only
//                                     "checked"/"unchecked" (#3856).
//
// Requests are newline-delimited. Each line is either a bare command
// ("dumpTree", "grab SpectrumWidget /tmp/pan.png", "invoke masterVolume
// setValue 30", "get slice active") or a JSON object ({"cmd":"invoke",
// "target":"masterVolume","action":"setValue","value":"30"}). Each request
// yields exactly one compact-JSON response line.
//
// Keeping this separate from TciServer is deliberate — TCI has external
// protocol-compat constraints (eesdr-tci aborts on unknown commands) and test
// verbs must never leak into a radio-control protocol.
class AutomationServer : public QObject {
    Q_OBJECT

public:
    explicit AutomationServer(QObject* parent = nullptr);
    ~AutomationServer() override;

    // Start listening on the given QLocalServer name. Returns false if the
    // server could not bind (e.g. a stale socket that could not be removed).
    // On success the resolved socket path is written to a discovery file
    // (<temp>/aethersdr-automation.json) so a driver can find it without
    // guessing the platform-specific endpoint.
    bool start(const QString& serverName);
    void stop();

    bool isRunning() const;
    QString serverName() const { return m_serverName; }
    QString fullServerName() const;  // resolved socket path / pipe name

    // Live model handle for the get() verb. Set once at startup from the
    // MainWindow's active-session RadioModel; may be null (get() then reports
    // "no radio model" rather than crashing).
    void setRadioModel(RadioModel* model) { m_radioModel = model; }
    void setAudioEngine(AudioEngine* audio) { m_audioEngine = audio; }
    // QSO recorder handle for the record() verb (start/stop/status/path).
    void setQsoRecorder(QsoRecorder* rec) { m_qsoRecorder = rec; }
    // Real connection hook for the connect/disconnect/dialog verbs. The bridge
    // asks the implementor (the GUI's ConnectionPanel) to drive the same path
    // the visible buttons do, so automation exercises the normal
    // MainWindow/RadioModel connection flow. The engine holds only the
    // gui-free IConnectionAutomation interface (aetherd RFC step 1 / EB1
    // boundary); lifetime across deferred calls is guarded via asQObject().
    void setConnectionAutomation(IConnectionAutomation* conn)
    {
        m_connection = conn;
        // Guard on the implementor's QObject so a destroyed panel reads back as
        // null, preserving the old QPointer<ConnectionPanel> safety net (the
        // raw interface pointer alone cannot auto-null). See connection().
        m_connectionGuard = conn ? conn->asQObject() : nullptr;
    }
    void setConnectionDialogHost(QObject* host) { m_connectionDialogHost = host; }
    void setSliceReceiveSourceHandler(
        std::function<QJsonObject(const QString&)> handler)
    {
        m_sliceReceiveSourceHandler = std::move(handler);
    }
    void setReceiveSyncSnapshotHandler(std::function<QJsonObject()> handler)
    {
        m_receiveSyncSnapshotHandler = std::move(handler);
    }
    void setKiwiSdrSnapshotHandler(std::function<QJsonObject()> handler)
    {
        m_kiwiSdrSnapshotHandler = std::move(handler);
    }

private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();

    // TX safety watchdog (#3646): polls TX state and force-unkeys the radio if
    // it has been keyed continuously past the limit, so a hung/abandoned
    // automation script can never leave a live transmitter on.
    void onTxWatchdog();
    // Push queued log events to subscribed clients (log subscribe). Runs on the
    // main thread so QLocalSocket writes are thread-confined; the tap that fills
    // the ring runs on arbitrary logging threads.
    void onLogDrain();

private:
    // Dispatch a single request line and return the response object. The socket
    // is needed for stateful per-client verbs (log subscribe/unsubscribe).
    QJsonObject handleLine(const QByteArray& line, QLocalSocket* sock);

    QJsonObject doDumpTree() const;
    QJsonObject doFloors() const;
    QJsonObject doGrab(const QString& target, const QString& path) const;
    // grab pan <index> [path]: capture the raw SpectrumWidget framebuffer for a
    // specific pan (by SpectrumWidget::panIndex) in a multi-pan layout — plain
    // `grab SpectrumWidget` only ever resolves the first one (#3646).
    QJsonObject doGrabPan(const QString& indexStr, const QString& path) const;
    // grab pan-visible <index> [path]: capture the enclosing PanadapterApplet so
    // overlay child widgets such as VFO flags appear in the PNG too.
    QJsonObject doGrabPanVisible(const QString& indexStr, const QString& path) const;
    // Shared "save this widget to a PNG and describe it" tail for grab/grab pan.
    QJsonObject saveWidgetGrab(QWidget* w, const QString& label,
                               const QString& path) const;
    QJsonObject doInvoke(const QString& target, const QString& action,
                         const QString& value) const;
    // close <target>: close the target's top-level window (deferred to a clean
    // main-loop turn so a confirm-dialog closeEvent can't re-enter the socket
    // callback). Reaches the custom frameless title-bar close that `invoke …
    // click` can't, and works for any window. (#3646 fidelity)
    QJsonObject doClose(const QString& target) const;
    // drag <target> <dx> <dy> | mouse <target> <dx> <dy>: synthesize a
    // press → move → release gesture so resize grips and slider handles are
    // provable end-to-end, not just via seed + read-back. (#3646 fidelity)
    QJsonObject doDrag(const QString& target, const QString& value) const;
    // hover <target> [leave]: synthesize pointer hover over a widget so
    // hover-driven UI (e.g. the HGauge mouse-over value readout on the TX
    // SWR/power/ALC meters) is provable end-to-end. Bare form sends a
    // QEnterEvent + QMouseMove at the widget centre; the 'leave' form sends a
    // QEvent::Leave so the fade-after-exit timer can be observed. Unlike drag,
    // no button is pressed, matching a real hover.
    QJsonObject doHover(const QString& target, const QString& action) const;
    // scrollTo <target> (alias ensureVisible): scroll the nearest QScrollArea
    // ancestor so the target widget sits in its viewport. Widgets parked below
    // the fold of a scroll area (e.g. the Aetherial strip's waveform panel)
    // receive no paint events until scrolled into view, so a driver must be
    // able to bring them on screen before measuring or grabbing them.
    QJsonObject doScrollTo(const QString& target) const;
    // showMenu <target>: pop a QToolButton/QPushButton drop-down menu, posted
    // onto the GUI event loop with the owning window raised — showing the native
    // popup from inside the socket-read callback re-enters Cocoa and segfaults on
    // a backgrounded macOS instance. (#3646 fidelity)
    QJsonObject doShowMenu(const QString& target) const;
    // contextMenu <target> [x y]: trigger a widget's custom right-click context
    // menu by synthesizing a QContextMenuEvent (routed through event() so the
    // CustomContextMenu / overridden contextMenuEvent paths both fire). Posted
    // onto the GUI loop with the owning window raised, like showMenu, because the
    // handler pops a QMenu that runs its own event loop. The popped menu is read
    // via dumpTree and driven via invoke, no extra inspection code needed. (#3858)
    QJsonObject doContextMenu(const QString& target, const QString& value) const;
    // hitTest <target> [x y]: read-only Qt hit-test probe. Reports the widget
    // under a target-local point according to childAt() and QApplication::widgetAt().
    QJsonObject doHitTest(const QString& target, const QString& value) const;
    // clickAt [<target>] <x> <y>: synthesize a real left-click at a point. With no
    // target, x/y are GLOBAL screen coordinates (matching dumpTree geometry); with
    // a target they are LOCAL to that widget. Generic fallback for when name/text
    // matching is ambiguous (e.g. several tiles share accessibleName
    // "containerClose" and only the first is reachable by invoke). TX-gated on the
    // whole ancestor chain; disabled widgets and (with the power ceiling armed)
    // the RF/Tune power sliders are refused.
    QJsonObject doClickAt(const QString& target, const QString& value) const;
    // pan close <panId|index|active|all>: tear down a panadapter regardless of
    // how it was opened. Sends `display pan remove` AND `display panafall remove`
    // (the FlexLib-correct pair) so a panafall-created pan closes too. The
    // production GUI close path now does the same via RadioModel::removePanadapter
    // (#3843). (#3646)
    QJsonObject doPan(const QString& action, const QString& arg);
    // panmessage add|remove|clear|list <pan-index|active>: inject/read
    // panadapter overlay messages for deterministic UI verification. UI-only;
    // never sends radio commands and never keys TX. `add` accepts optional
    // tone=info|warning for visual-state coverage.
    QJsonObject doPanMessage(const QString& action,
                             const QString& target,
                             const QString& id,
                             const QString& title,
                             const QString& detail,
                             int timeoutMs,
                             const QString& tone) const;
    QJsonObject doDss(const QString& action,
                      const QString& target,
                      const QString& value) const;
    // Radio-side display-stream inventory / leak detector (#3856).
    //   streams        — Layer A: registered pan/wf streams + UDP "orphan"
    //                     streams the radio is still transmitting that we let go.
    //   streams radio   — Layer B: the radio-authoritative display-object set
    //                     (pans + waterfalls) classified ours/foreign/orphan,
    //                     plus leaked waterfalls (parent pan gone) — catches the
    //                     resource-level lingering Layer A can't see.
    //   streams resync  — re-subscribe (sub pan all) to force the radio to
    //                     re-dump every allocated display object, refreshing the
    //                     Layer-B maps to the radio's present-tense set; re-poll
    //                     `streams radio` after it settles to confirm a lingering
    //                     waterfall the client view had already purged.
    //   streams reset   — clear the Layer-A orphan tally to re-baseline.
    QJsonObject doStreams(const QString& action);
    QJsonObject doTci(const QString& action, const QString& value);
    QJsonObject doAudioCapture(const QString& action,
                               const QString& arg,
                               const QString& path) const;
    QJsonObject doGet(const QString& model, const QString& selector,
                      const QString& property) const;
    QJsonObject doConnect(const QString& action, const QString& arg, QLocalSocket* sock);
    QJsonObject doConnectDialog(const QString& action);
    QJsonObject doDisconnect();
    // record start|stop|status|path|dir <path> — drive the Client-Side QSO
    // recorder, read the WAV path, or point recordings at a path (for live
    // capture-file verification on TCC-restricted boxes).
    QJsonObject doRecord(const QString& action, const QString& value);
    // testtone on [freqHz] [levelDb] | off — drive the client-side TX test tone
    // through onTxAudioReady so a recording gets a deterministic "phone" segment
    // (verifies SSB<->CW switching while recording). The actual transmit still
    // requires a separately-gated key/MOX.
    QJsonObject doTestTone(const QString& action, const QString& value);
    QJsonObject doConnectWait(int timeoutMs, QLocalSocket* sock);
    struct ConnectWait;
    void finishConnectWait(const std::shared_ptr<ConnectWait>& wait, bool timedOut);
    // TX test-signal control (two-tone) and ATU control. Both gated by
    // AETHER_AUTOMATION_ALLOW_TX where they key the transmitter.
    QJsonObject doTxTest(const QString& action);
    QJsonObject doAtu(const QString& action);

    void forceUnkey(const char* reason);  // emergency all-stop (tune/mox/two-tone)

    // Slice lifecycle (add/remove/select/tx) and VFO tuning — RX/config, no keying.
    QJsonObject doSlice(const QString& action, const QString& arg);
    QJsonObject doTune(const QString& value);
    // Semantic transmitter keying (#3646 fidelity): `key ptt on|off` / `key mox`
    // route to RadioModel::setTransmit — the exact calls the space-bar PTT filter
    // and the mox_toggle shortcut make, but reachable headlessly. Keying is gated
    // by AETHER_AUTOMATION_ALLOW_TX (the same rail as txtest/atu); unkey is not.
    QJsonObject doKey(const QString& name, const QString& arg);
    // Drive the CWX keyer (send a CW string / set WPM / abort). `send` keys the
    // transmitter so it sits on the AETHER_AUTOMATION_ALLOW_TX rail and arms the
    // force-unkey watchdog; speed/stop do not key. CW's rapid TX→RX edges are the
    // easy repro for post-TX FFT-floor recovery (#3804).
    QJsonObject doCwx(const QString& action, const QString& arg);
    // Per-GUI-client station identity (#3646 fidelity). Sets `client station
    // <name>` so other MultiFlex clients see the agent's name; NEVER the radio
    // callsign. Auto-applied on connect, restored on stop. No keying.
    QJsonObject doStation(const QString& name);
    void applyAgentStation(const QString& name);  // capture prior + send
    void restoreStation();                        // re-send the user's real name
    // QRZ callsign lookup (status | cached <call> | lookup <call> |
    // spottext <text>).  spottext feeds the CW callsign spotter the given
    // text as if the decoder produced it — end-to-end card-pop proof with
    // no radio or live CW required. No keying.
    QJsonObject doQrz(const QString& action, const QString& value);
    // Resize a top-level window so the panadapter x_pixels (== SpectrumWidget
    // width) propagates to a realistic value for headless render-size fidelity.
    QJsonObject doResize(const QString& value, const QString& target) const;
    // window <maximize|restore|minimize|fullscreen> [target]: drive a top-level
    // window's state (resize only ever set explicit geometry, so an un-maximize
    // was unverifiable). dumpTree now also carries `windowState`. (#3918)
    QJsonObject doWindow(const QString& action, const QString& target) const;
    // Fire a ShortcutManager action by id — the MIDI-controller dispatch path —
    // for actions with no key sequence and no menu entry (Band Zoom, Segment
    // Zoom, …). TX-keying ids stay behind AETHER_AUTOMATION_ALLOW_TX. (#4057)
    QJsonObject doShortcut(const QString& id) const;
    // Resolve the top-level window a window-scoped verb (resize/window) acts on:
    // the target's window() if given, else the QMainWindow (or first visible real
    // top-level). Shared by doResize and doWindow.
    static QWidget* topLevelWindowForTarget(const QString& target);
    // Menu-bar discovery/popup (#3646 fidelity): `menu list` enumerates the
    // menu-bar tree; `menu open <name>` pops a top-level menu for grab/dumpTree.
    QJsonObject doMenu(const QString& action, const QString& arg) const;
    // Identity of THIS bridge instance — pid/socket/label — for multi-instance
    // drivers that enumerate the per-pid discovery directory.
    QJsonObject doWhoami() const;
    // Observability suite (#3646): runtime log-category control, ring-buffer
    // tail, push subscription, and timeline markers. All diagnostic, no keying.
    QJsonObject doLog(const QString& action, const QString& arg, QLocalSocket* sock);
    QJsonObject doMark(const QString& text);
    struct LogEvent;
    static QJsonObject logEventToJson(const LogEvent& e);  // redacts on egress

    // Resolve a target string to a widget, including pan-index scoped targets:
    // exact objectName first, then class name (with or without namespace) or
    // accessibleName. Within each match class, visible/enabled widgets win over
    // hidden duplicates.
    static QWidget* resolveWidget(const QString& target);

    QLocalServer* m_server{nullptr};
    QString       m_serverName;
    QString       m_discoveryFile;    // legacy single-instance pointer (back-compat)
    QString       m_discoveryDir;     // <temp>/aethersdr-automation/ (per-pid entries)
    QString       m_discoveryEntry;   // this instance's <pid>.json in m_discoveryDir
    QString       m_label;            // AETHER_AUTOMATION_LABEL (human instance tag)
    QHash<QLocalSocket*, QByteArray> m_buffers;  // per-client read buffer
    QPointer<RadioModel> m_radioModel;           // for get(); may be null
    QPointer<AudioEngine> m_audioEngine;          // for get audio; may be null
    QPointer<QsoRecorder> m_qsoRecorder;          // for record(); may be null
    IConnectionAutomation* m_connection = nullptr;  // connect/disconnect verbs
    QPointer<QObject> m_connectionGuard;            // auto-nulls when the impl is destroyed
    // Returns the connection hook only while its implementor is alive, so every
    // synchronous use fails closed ("unavailable") after the panel is gone —
    // exactly as the former QPointer<ConnectionPanel> member did.
    IConnectionAutomation* connection() const
    {
        return m_connectionGuard ? m_connection : nullptr;
    }
    QPointer<QObject> m_connectionDialogHost;    // MainWindow show/hide invokables
    std::function<QJsonObject(const QString&)> m_sliceReceiveSourceHandler;
    std::function<QJsonObject()> m_receiveSyncSnapshotHandler;
    std::function<QJsonObject()> m_kiwiSdrSnapshotHandler;

    // Agent station identity (#3646). The bridge sets the per-GUI-client station
    // name to the agent's name on connect and restores the user's real name on
    // stop, so other MultiFlex clients can see an agent is driving.
    QString m_agentStation;          // applied name (AETHER_AUTOMATION_STATION)
    QString m_priorStationName;      // user's real station name, captured to restore
    bool    m_stationApplied{false};

#ifdef HAVE_WEBSOCKETS
    // In-process TCI client simulator (`tci start|status|stop`, #3305/#4009).
    // Connects to the app's own TCI server over loopback exactly like WSJT-X
    // (init burst → ready → audio_samplerate + audio_start) so agents can
    // exercise the TCI/DAX lifecycle — including abrupt-disconnect reaping —
    // without an external WebSocket client.
    QWebSocket* m_tciSim{nullptr};
    bool    m_tciSimReady{false};
    bool    m_tciSimAudioStarted{false};
    qint64  m_tciSimBinaryFrames{0};
    qint64  m_tciSimBinaryBytes{0};
    qint64  m_tciSimTextMsgs{0};
    qint64  m_tciSimLastFrameMs{-1};
    QString m_tciSimCloseReason;
    QElapsedTimer m_tciSimTimer;
#endif

    // TX safety rails (active only when AETHER_AUTOMATION_ALLOW_TX is set).
    QTimer* m_txWatchdog{nullptr};
    qint64  m_txKeyedSinceMs{0};   // when continuous key-down started (0 = idle)
    int     m_txMaxKeyMs{20000};   // max continuous key time before force-unkey
    int     m_txMaxPower{-1};      // power-ceiling clamp for invoke (-1 = off)
    bool    m_txAllowed{false};    // AETHER_AUTOMATION_ALLOW_TX at start()
    // Log/event channel (#3646 observability suite). The tap fills m_logRing
    // from arbitrary logging threads; the main thread reads it for tail/drain.
    struct LogEvent {
        quint64 seq{0};
        qint64  monoUs{0};   // process-monotonic microseconds (jitter-grade)
        QString wall;        // HH:mm:ss.zzz, to line up with the log file
        int     type{0};     // QtMsgType
        QString cat;
        QString msg;         // raw; PII-redacted only on egress
    };
    int            m_logTapId{-1};
    QElapsedTimer  m_monoClock;            // started in start()
    mutable QMutex m_logMutex;             // guards m_logRing / m_logSeq
    std::deque<LogEvent> m_logRing;
    quint64        m_logSeq{0};
    QHash<QLocalSocket*, quint64> m_logSubscribers;  // sock -> last seq sent
    QTimer*        m_logDrain{nullptr};
    static constexpr int kLogRingMax = 8000;

    struct ConnectWait {
        QPointer<QLocalSocket> socket;
        QTimer* timer{nullptr};
        QMetaObject::Connection connection;
        QElapsedTimer elapsed;
        int timeoutMs{0};
        bool complete{false};
    };
    std::vector<std::shared_ptr<ConnectWait>> m_connectWaits;
};

} // namespace AetherSDR
