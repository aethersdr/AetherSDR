#pragma once

#include <QObject>
#include <QHash>
#include <QPointer>
#include <QByteArray>
#include <QElapsedTimer>
#include <QMutex>
#include <QString>

#include <deque>

class QLocalServer;
class QLocalSocket;
class QWidget;
class QJsonObject;
class QTimer;

namespace AetherSDR {

class RadioModel;

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
//                                     setText / setCurrentText / setCurrentIndex.
//                                     SAFETY: refuses any control marked as
//                                     transmit-keying (markTxKeying() / the
//                                     "aetherTxKeying" property — MOX/PTT, TUNE,
//                                     ATU, CWX send, packet/APRS send) unless
//                                     AETHER_AUTOMATION_ALLOW_TX is set, so the
//                                     bridge can never key a live radio by
//                                     accident. A button-scoped name heuristic
//                                     is a logged fallback; setpoint
//                                     sliders/combos are never blocked.
//   get <model> [selector] [prop]  -> live JSON snapshot of a model:
//                                     radio | transmit | slice <id|active|tx> |
//                                     slices | pan <panId|active> | pans. With a
//                                     trailing property name, returns just that
//                                     field. Assert on state without screenshots.
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
//   menu list | menu open <name>   -> enumerate the menu bar / pop a menu for a
//                                     follow-up grab/dumpTree.
//   whoami                         -> {pid, socket, label, station} — identify
//                                     THIS instance among concurrent bridges.
//
// Phase 2b verbs (observability + reach, this batch — #3646):
//
//   grab pan <index> [path]        -> capture a SPECIFIC pan's spectrum surface
//                                     (by SpectrumWidget::panIndex) in a multi-
//                                     pan layout; plain `grab SpectrumWidget`
//                                     only ever returns the first one.
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
//   pan add                        -> create a new panadapter (panafall); the
//                                     only UI path is an unaddressable QLabel.
//   pan close <id|index|active|all>-> tear down a panadapter regardless of how it
//                                     was opened (sends display pan remove AND
//                                     display panafall remove).
//   dumpTree (extended)            -> nodes now carry toolTip, and QComboBox
//                                     nodes carry items[]/currentIndex and pans
//                                     carry panIndex, all assertable without
//                                     stepping a control.
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
    // grab pan <index> [path]: capture the SpectrumWidget for a specific pan
    // (by SpectrumWidget::panIndex) in a multi-pan layout — plain `grab
    // SpectrumWidget` only ever resolves the first one (#3646).
    QJsonObject doGrabPan(const QString& indexStr, const QString& path) const;
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
    // showMenu <target>: pop a QToolButton/QPushButton drop-down menu, posted
    // onto the GUI event loop with the owning window raised — showing the native
    // popup from inside the socket-read callback re-enters Cocoa and segfaults on
    // a backgrounded macOS instance. (#3646 fidelity)
    QJsonObject doShowMenu(const QString& target) const;
    // pan close <panId|index|active|all>: tear down a panadapter regardless of
    // how it was opened. Sends `display pan remove` AND `display panafall remove`
    // (the FlexLib-correct pair) so a panafall-created pan closes too — the plain
    // `display pan close` in removePanadapter leaves the waterfall behind. (#3646)
    QJsonObject doPan(const QString& action, const QString& arg);
    QJsonObject doGet(const QString& model, const QString& selector,
                      const QString& property) const;
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
    // Resize a top-level window so the panadapter x_pixels (== SpectrumWidget
    // width) propagates to a realistic value for headless render-size fidelity.
    QJsonObject doResize(const QString& value, const QString& target) const;
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

    // Resolve a target string to a widget: exact objectName first, then
    // class name (with or without namespace) or accessibleName.
    static QWidget* resolveWidget(const QString& target);

    QLocalServer* m_server{nullptr};
    QString       m_serverName;
    QString       m_discoveryFile;    // legacy single-instance pointer (back-compat)
    QString       m_discoveryDir;     // <temp>/aethersdr-automation/ (per-pid entries)
    QString       m_discoveryEntry;   // this instance's <pid>.json in m_discoveryDir
    QString       m_label;            // AETHER_AUTOMATION_LABEL (human instance tag)
    QHash<QLocalSocket*, QByteArray> m_buffers;  // per-client read buffer
    QPointer<RadioModel> m_radioModel;           // for get(); may be null

    // Agent station identity (#3646). The bridge sets the per-GUI-client station
    // name to the agent's name on connect and restores the user's real name on
    // stop, so other MultiFlex clients can see an agent is driving.
    QString m_agentStation;          // applied name (AETHER_AUTOMATION_STATION)
    QString m_priorStationName;      // user's real station name, captured to restore
    bool    m_stationApplied{false};

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
};

} // namespace AetherSDR
