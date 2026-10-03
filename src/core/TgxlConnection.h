#pragma once

#include <functional>
#include <QObject>
#include <QTcpSocket>
#include <QElapsedTimer>
#include <QTimer>
#include <QMap>
#include <QString>

namespace AetherSDR {

// Direct TCP to a 4O3A Tuner Genius XL on port 9010 for manual relay control
// (C1/L/C2), independent of the Flex on 4992. Reverse-engineered from the TGXL
// management app (#469):
//   C<seq>|<command>\n          — client command
//   R<seq>|<code>|<body>\n      — TGXL response
//   S0|state key=val ...\n      — unsolicited state push
//   M|<text>\n                  — alert text; empty body clears it
//   V<version>\n                — version line on connect
class TgxlConnection : public QObject {
    Q_OBJECT

public:
    explicit TgxlConnection(QObject* parent = nullptr);

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

    void connectToTgxl(const QString& host, quint16 port = 9010);
    // A connect the app makes on its own (discovery, presence, startup) rather
    // than one the operator asked for. Only these attempts report unreachable().
    void autoConnectToTgxl(const QString& host, quint16 port = 9010);
    // A one-off try at another address for the same device. Reconnects keep
    // the previous target, and a missing saved code for this address fails
    // the try without blocking them.
    void tryAlternateTgxl(const QString& host, quint16 port = 9010);
    void disconnect();

    void setAutoReconnect(bool on) { m_autoReconnect = on; }
    void setAuthCode(const QString& code);
    void setAuthCodeForAttempt(quint64 attempt, const QString& code,
                               bool credentialStoreUnavailable = false);

    // Manual relay adjustment: relay 0=C1, 1=L, 2=C2; direction +1 or -1
    void adjustRelay(int relay, int direction);

    // Native autotune over the direct port-9010 channel. The TGXL drives
    // radio PTT via its hardware interlock cable, so no client-side keying
    // is required. Bypasses the firmware's `tgxl autotune` command path
    // (broken in firmware 4.2 — see issue tracker for "TUNE button on TGXL").
    void requestAutotune();

    // Send an arbitrary command to the TGXL (e.g. "activate ant=2")
    quint32 sendCommand(const QString& cmd);

    // Poll fast only while keyed. Measured on TGXL 1.2.17: 7 ms median round
    // trip, value changes every ~17 ms (~59 Hz), so ~60 Hz while TX; 4 Hz on RX.
    // Driven by the device's own ptt status fields (noticed up to 250 ms late);
    // setTransmitting() lets a caller that knows switch immediately.
    void setTransmitting(bool tx);
    bool isTransmitting() const { return m_transmitting; }
    // For tests: the interval currently in force.
    int  pollIntervalMs() const { return m_pollTimer.interval(); }

    static constexpr int kPollTxMs = 16;    // ~60 Hz
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
    void stateUpdated(const QMap<QString, QString>& kvs);
    void statusUpdated(const QMap<QString, QString>& kvs);
    // Operator-facing alert from the tuner ("LOW RF POWER" when a tune is
    // asked for with too little drive to measure). Empty text means the
    // tuner has cleared it, which it does on its own a few seconds later.
    // Broadcast to every connected client, not just the one that acted.
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

    QTcpSocket m_socket;
    QTimer     m_pollTimer;       // interval follows m_transmitting
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
    quint16    m_lastPort{9010};
};

} // namespace AetherSDR
