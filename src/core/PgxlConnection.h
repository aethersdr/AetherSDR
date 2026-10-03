#pragma once

#include <functional>
#include <QObject>
#include <QTcpSocket>
#include <QElapsedTimer>
#include <QTimer>
#include <QMap>
#include <QString>

#include <functional>

namespace AetherSDR {

// Direct TCP connection to a 4O3A Power Genius XL on port 9008.
// Same protocol as TgxlConnection (C/R/S/V/M message format).
// Provides PGXL status telemetry: state, power, SWR, current,
// temperature, mains voltage, band, bias mode, fan mode.
class PgxlConnection : public QObject {
    Q_OBJECT
    friend struct PgxlConnectionTestAccess;

public:
    explicit PgxlConnection(QObject* parent = nullptr);

    bool isConnected() const { return m_connected; }
    bool isConnecting() const { return !m_connected && m_socket.state() != QAbstractSocket::UnconnectedState; }
    bool isAuthBlocked() const { return m_authBlocked; }
    QString version() const { return m_version; }
    QString peerAddress() const { return m_socket.peerAddress().toString(); }
    // The host the operator (or discovery) asked for on the current attempt:
    // a name or a literal address. Saved codes key on it; see PeripheralAuthStore.
    QString attemptHost() const { return m_attemptHost; }
    // The host reconnects aim at; an alternate attempt leaves it unchanged.
    QString reconnectHost() const { return m_lastHost; }
    quint16 reconnectPort() const { return m_lastPort; }
    quint16 peerPort() const { return m_socket.peerPort(); }

    void connectToPgxl(const QString& host, quint16 port = 9008);
    // A connect the app makes on its own (discovery, presence, startup) rather
    // than one the operator asked for. Only these attempts report unreachable().
    void autoConnectToPgxl(const QString& host, quint16 port = 9008);
    // A one-off try at another address for the same device. Reconnects keep
    // the previous target, and a missing saved code for this address fails
    // the try without blocking them.
    void tryAlternatePgxl(const QString& host, quint16 port = 9008);
    void disconnect();

    void setAutoReconnect(bool on) { m_autoReconnect = on; }
    void setAuthCode(const QString& code);
    void setAuthCodeForAttempt(quint64 attempt, const QString& code,
                               bool credentialStoreUnavailable = false);

    quint32 sendCommand(const QString& cmd);

    // Poll fast only while keyed (as TgxlConnection). Measured: the PGXL meter yields
    // ~10 Hz of distinct values however fast it is polled (32.9 Hz of frames carried
    // 10.2 Hz of new readings across four sockets), so 10 Hz TX, 4 Hz RX. Driven by
    // the `state` field in status frames; setTransmitting() lets a caller raise the
    // rate early.
    void setTransmitting(bool tx);
    bool isTransmitting() const { return m_transmitting; }
    int  pollIntervalMs() const { return m_pollTimer.interval(); }

    static constexpr int kPollTxMs = 100;   // 10 Hz
    static constexpr int kPollRxMs = 250;   // 4 Hz

signals:
    void connected();
    void disconnected();
    void connectionFailed(const QString& errorString);
    // An automatic attempt never reached the device over TCP; carries the host
    // it asked for. Not sent after a deliberate disconnect or once auth has
    // blocked reconnects.
    void unreachable(const QString& attemptedHost);
    void authCodeRequired(quint64 attempt);
    void authCodeAccepted(const QString& code);
    void enteredAuthCodeDiscarded();
    void authBlockCleared();
    void statusUpdated(const QMap<QString, QString>& kvs);
    // The reply to `setup read` (nickname, ledintens, txdelay, inactivity-timeout,
    // authcode), an ordinary R frame matched by the request's sequence number. Needed
    // because `setup` writes take the whole group in one line (as the vendor utility
    // sends it), so changing one field means knowing the rest.
    void setupRead(const QMap<QString, QString>& kvs);
    // The amplifier refused a command: `R<seq>|<code>|` with a non-zero code
    // and an empty body. 50000013 is a bad parameter (a `setup` carrying a
    // value the amplifier will not take), 50000015 an unknown command.
    // Emitted so a refusal is visible rather than being read as an empty
    // status frame and dropped.
    void commandRefused(quint32 seq, const QString& code);
    // Operator-facing alert, empty text meaning the amplifier has cleared it.
    // Same `M|<text>` frame the tuner uses — the two devices share a protocol
    // and a vendor. Broadcast to every connected client, not only the one
    // that acted.
    //
    // No PGXL alert has actually been captured: the frame is handled because
    // the framing is shared and doing so costs nothing, not because one was
    // observed. See pgxl_direct_protocol_test.
    void alertChanged(const QString& text);

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onError(QAbstractSocket::SocketError error);
    void pollStatus();

private:
    friend struct PeripheralConnectionTestAccess;
    // Inject transport initiation in socket-free lifecycle tests.
    std::function<void(const QString&, quint16)> m_connectTransport;
    void applyPollRateFor(const QMap<QString, QString>& kvs);
    Q_INVOKABLE void processLine(const QString& line); // injected-frame test seam
    Q_INVOKABLE void processBytes(const QByteArray& bytes); // injected transport test seam
    Q_INVOKABLE void beginAttempt(); // same reset used before a real TCP connect
    Q_INVOKABLE void beginAttemptAt(const QString& host, quint16 port);
    Q_INVOKABLE void beginAutomaticAttemptAt(const QString& host, quint16 port);
    Q_INVOKABLE void beginAlternateAttemptAt(const QString& host, quint16 port);
    void openSocket(const QString& host, quint16 port, bool wasConnected);
    Q_INVOKABLE void onAuthTimeout();
    void finishHandshake();
    void sendAuthentication();
    void failAuthentication(const QString& reason, bool blockReconnect = true);

    // Test seam: when set, sendCommand() hands each framed line here instead
    // of writing to the socket.
    std::function<void(const QByteArray&)> m_commandWriter;
    QTcpSocket m_socket;
    QTimer     m_pollTimer;
    bool       m_transmitting{false};
    // One status poll in flight at a time. The transmit interval assumes the
    // round trip fits inside it; on a congested LAN it may not, and an
    // unconditional write would queue requests the device answers late and
    // we never asked for. Self-limiting instead: skip a tick while one is
    // outstanding, and give up on it after kPollStaleMs so a dropped reply
    // cannot wedge polling for good.
    bool          m_pollInFlight{false};
    QElapsedTimer m_pollSent;
    static constexpr int kPollStaleMs = 1000;
    QTimer     m_reconnectTimer;
    QByteArray m_readBuf;
    quint32    m_seq{0};
    // Sequence number of the outstanding `setup read`, 0 when none. See
    // setupRead().
    quint32    m_setupReadSeq{0};
    bool       m_connected{false};
    bool       m_gotVersion{false};
    bool       m_authPending{false};
    bool       m_waitingForAuthCode{false};
    bool       m_authBlocked{false};
    int        m_authFailures{0};
    quint64    m_authAttempt{0};
    QTimer     m_authTimer;
    QString    m_authCode;
    bool       m_userAuthCode{false};
    QString    m_userAuthEndpoint;
    QString    m_attemptHost;
    bool       m_authCloseReported{false};
    bool       m_autoReconnect{false};
    // Global "Reconnect automatically" and this device's "Connect automatically".
    bool reconnectAllowed() const;
    bool       m_deliberateDisconnect{false};
    bool       m_tcpReached{false};       // this attempt's socket connected
    bool       m_attemptAutomatic{false}; // see autoConnectTo…
    bool       m_lastAutomatic{false};    // reconnects repeat the target's origin
    bool       m_alternateAttempt{false}; // see tryAlternate…
    QString    m_version;
    QString    m_lastHost;
    quint16    m_lastPort{9008};
};

} // namespace AetherSDR
