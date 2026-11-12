#pragma once

// One held session with a microHAM TRIO: page request for the cookie, upgrade,
// then the web socket kept open. Frames go to MicrohamTrio::classify(); what to
// do next comes from MicrohamTrio::decideSession(), so this class keeps the
// state that takes and holds no policy of its own.
// Read only: nothing here sends a selection. Main-thread only. Bytes and time
// are injected; nothing here opens a socket.

#include "core/MicrohamTrioProtocol.h"
#include "core/MicrohamTrioWebSocket.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QtGlobal>

#include <functional>
#include <optional>

class QTimer;

namespace AetherSDR {

// One TCP connection at a time, as bytes. The session opens it twice per
// connect: once for the page, once for the web socket.
class MicrohamTrioTransport : public QObject {
    Q_OBJECT

public:
    explicit MicrohamTrioTransport(QObject* parent = nullptr) : QObject(parent) {}

    // Drop any connection and connect. Answers with opened() or failed().
    virtual void open(const QString& host, quint16 port) = 0;
    virtual void send(const QByteArray& bytes) = 0;
    // Drop the connection. No signal follows.
    virtual void close() = 0;

signals:
    void opened();
    void received(const QByteArray& bytes);
    void closedByPeer();
    void failed(const QString& reason);
};

// Monotonic milliseconds.
class MicrohamTrioClock {
public:
    virtual ~MicrohamTrioClock() = default;
    virtual qint64 nowMs() const = 0;
};

class MicrohamTrioSession : public QObject {
    Q_OBJECT

public:
    struct Config {
        QString host;
        quint16 port{80};
        MicrohamTrio::Policy policy;
        qint64 connectTimeoutMs{6000};     // page request through the 101
        qint64 firstStateTimeoutMs{6000};  // the 101 through the first state document
        // An error page in the first frame, or no state at all, clears by
        // itself: tried this many times, this far apart, before it counts as
        // a failure.
        int    transientAttempts{3};
        qint64 transientRetryMs{1500};
    };

    // Unpredictable values: random32 feeds the frame masks and the handshake nonce.
    struct Entropy {
        std::function<quint32()> random32;
    };
    static Entropy systemEntropy();

    enum class Status {
        Idle,         // not started
        Connecting,
        Held,
        Evicted,      // the peer closed the held session; not retaken
        Failed,       // retried after a backoff
        Released,     // left to the web page
    };

    enum class Failure {
        None,
        HostUnusable,
        ConnectFailed,
        ConnectTimeout,
        ClosedEarly,         // the peer closed before the upgrade was answered
        CookieMissing,
        CookieRefused,       // the upgrade was answered 400
        UpgradeRedirected,
        UpgradeRefused,
        HandshakeInvalid,    // a 101 that is not a WebSocket acceptance of our key
        ErrorPage,           // the first frame was not JSON
        NoStateDocument,
        UnreadableState,
        ProtocolError,       // a frame the decoder refuses
        ConnectionLost,      // the held connection broke without a close
        SessionSilent,       // held, but no frame or keep-alive reply in time
    };

    enum class SnapshotSource {
        OnConnect,   // the first state of a session
        Push,        // a change made elsewhere: front panel, band data, another client
    };

    // `transport` and `clock` are not owned and must outlive the session.
    MicrohamTrioSession(MicrohamTrioTransport* transport, const MicrohamTrioClock* clock,
                        Entropy entropy, QObject* parent = nullptr);
    ~MicrohamTrioSession() override;

    // Take the session and keep it until stop(). Nothing is sent before start().
    void start(const Config& config);
    // A held session is closed with a WebSocket close frame first.
    void stop();
    // Give the session to the web page, or take it back.
    void setReleasedForBrowser(bool released);

    bool    isRunning() const { return m_running; }
    Status  status() const { return m_status; }
    Failure lastFailure() const { return m_failure; }
    // The last state received since start(), and whether it arrived on the
    // session held now.
    const std::optional<MicrohamTrio::Snapshot>& snapshot() const { return m_snapshot; }
    bool snapshotCurrent() const
    {
        return m_link == MicrohamTrio::Link::Held && m_stateOnThisSession;
    }

    MicrohamTrio::State state() const;
    // Clock time at which the session's timer next runs tick().
    qint64 nextTickAtMs() const { return m_nextTickAtMs; }

public slots:
    // Run the session decision. The session's own timer calls it; a test
    // calls it after moving its clock.
    void tick();

signals:
    // Every state document received, changed or not.
    void snapshotChanged(const AetherSDR::MicrohamTrio::Snapshot& snapshot,
                         AetherSDR::MicrohamTrioSession::SnapshotSource source);
    void sessionStateChanged(AetherSDR::MicrohamTrioSession::Status status);

private slots:
    void onOpened();
    void onReceived(const QByteArray& bytes);
    void onClosedByPeer();
    void onTransportFailed(const QString& reason);

private:
    enum class Phase {
        Idle,
        CookieConnecting,
        CookieReading,
        UpgradeConnecting,
        UpgradeReading,
        Open,
    };

    MicrohamTrio::State buildState(qint64 nowMs) const;
    void evaluate();
    void startAttempt(qint64 nowMs);
    void finishCookie();
    void enterHeld(qint64 nowMs);
    void feedFrames(const QByteArray& bytes);
    void handleFrame(const MicrohamTrio::Ws::ServerFrame& frame);
    void takeState(const MicrohamTrio::Frame& frame);
    void dropConnection();
    void becomeEvicted();
    void fail(Failure failure);
    void failTransient(Failure failure, qint64 nowMs);
    void sendText(const QByteArray& payload);
    void setStatus(Status status);
    void schedule(qint64 waitMs);

    MicrohamTrioTransport*   m_transport{nullptr};
    const MicrohamTrioClock* m_clock{nullptr};
    Entropy                  m_entropy;
    QTimer*                  m_timer{nullptr};
    qint64                   m_nextTickAtMs{0};

    Config m_config;
    bool   m_running{false};
    bool   m_released{false};

    Phase   m_phase{Phase::Idle};
    Status  m_status{Status::Idle};
    Failure m_failure{Failure::None};
    MicrohamTrio::Link m_link{MicrohamTrio::Link::NeverConnected};

    // Handshake: the response head so far, the cookie, our key.
    QByteArray m_head;
    bool       m_cookieHeadDone{false};
    qint64     m_cookieBodyBytes{0};
    std::optional<qint64> m_cookieBodyLength;
    QByteArray m_wid;
    QByteArray m_key;
    MicrohamTrio::Ws::FrameDecoder m_decoder;

    std::optional<qint64> m_lastConnectAtMs;   // when the last connect began
    std::optional<qint64> m_heldSinceMs;
    std::optional<qint64> m_lastMessageAtMs;
    std::optional<qint64> m_lastKeepAliveAtMs;
    std::optional<qint64> m_unansweredKeepAliveAtMs;
    std::optional<qint64> m_transientRetryAtMs;
    int  m_consecutiveFailures{0};
    int  m_transientFailures{0};
    bool m_stateOnThisSession{false};

    std::optional<MicrohamTrio::Snapshot> m_snapshot;
};

} // namespace AetherSDR
