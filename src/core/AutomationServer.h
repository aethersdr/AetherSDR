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
    QJsonObject doGrab(const QString& target, const QString& path) const;
    QJsonObject doInvoke(const QString& target, const QString& action,
                         const QString& value) const;
    QJsonObject doGet(const QString& model, const QString& selector,
                      const QString& property) const;
    // TX test-signal control (two-tone) and ATU control. Both gated by
    // AETHER_AUTOMATION_ALLOW_TX where they key the transmitter.
    QJsonObject doTxTest(const QString& action);
    QJsonObject doAtu(const QString& action);

    void forceUnkey(const char* reason);  // emergency all-stop (tune/mox/two-tone)

    // Slice lifecycle (add/remove/select) and VFO tuning — RX/config, no keying.
    QJsonObject doSlice(const QString& action, const QString& arg);
    QJsonObject doTune(const QString& value);
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
    QString       m_discoveryFile;
    QHash<QLocalSocket*, QByteArray> m_buffers;  // per-client read buffer
    QPointer<RadioModel> m_radioModel;           // for get(); may be null

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
