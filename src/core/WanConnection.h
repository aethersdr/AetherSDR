#pragma once

#include "CommandParser.h"

#include <QObject>
#include <QSslSocket>
#include <QByteArray>
#include <QElapsedTimer>
#include <QString>
#include <QHostAddress>
#include <QTimer>
#include <QVector>
#include <functional>
#include <atomic>

namespace AetherSDR {

// Pinned-cert cache management for the Pinned Certificates settings UI
// (#2951 / GHSA-wfx7-w6p8-4jr2). Backs the same store WanConnection
// uses for TOFU pin enforcement; defined in WanConnection.cpp so the
// JSON shape stays in one place.
struct PinnedCertInfo {
    QString host;
    QString fingerprintHex;
    QString pinnedAtIso;   // empty for legacy Phase 1 entries
};

namespace WanCertCache {
    QVector<PinnedCertInfo> listPinnedCerts();
    void forgetPinnedCert(const QString& host);
    void forgetAllPinnedCerts();
}

// Manages a TLS connection to a FlexRadio over the internet (SmartLink).
// Speaks the same V/H/R/S/M protocol as RadioConnection but over TLS,
// with an initial "wan validate handle=<h>" handshake.
//
// Emits the same signals as RadioConnection so RadioModel can use either.
class WanConnection : public QObject {
    Q_OBJECT

public:
    explicit WanConnection(QObject* parent = nullptr);
    ~WanConnection() override;

    bool isConnected() const    { return m_connected; }
    bool isSocketIdle() const   { return m_socket.state() == QAbstractSocket::UnconnectedState; }
    quint32 clientHandle() const { return m_handle; }
    QHostAddress radioAddress() const { return m_socket.peerAddress(); }
    quint16 localTcpPort() const { return m_socket.localPort(); }

    // Connect via TLS to radio's public IP with WAN handle auth
    void connectToRadio(const QString& host, quint16 tlsPort,
                        const QString& wanHandle);
    void disconnectFromRadio();

    // Same command interface as RadioConnection
    using ResponseCallback = std::function<void(int resultCode, const QString& body)>;
    quint32 sendCommand(const QString& command,
                        ResponseCallback callback = nullptr);

    // Phase 2 of GHSA-wfx7-w6p8-4jr2 (#2951): operator decides whether
    // to accept a new cert presented during a mismatch.  Connection is
    // paused — wan validate has NOT been sent — until one of these
    // methods is called by the UI in response to certFingerprintMismatch.
    //
    //   accept → overwrite the stored pin with the presented fingerprint
    //            and resume the WAN handshake.
    //   reject → tear the TLS session down and surface an error to the
    //            caller; the radio never sees an authenticated session.
    void acceptPresentedCert();
    void rejectPresentedCert();

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString& message);
    void messageReceived(const ParsedMessage& msg);
    void statusReceived(const QString& object, const QMap<QString, QString>& kvs);
    void versionReceived(const QString& version);
    void pingRttMeasured(int ms);

    // Phase 2 of GHSA-wfx7-w6p8-4jr2 (#2951). Fired when onTlsConnected
    // detects a fingerprint mismatch against the stored pin. The UI is
    // expected to prompt the operator and respond with accept/reject.
    // While waiting for the response no wan validate is sent — an
    // attacker on a hostile path never sees an authenticated session.
    void certFingerprintMismatch(const QString& host,
                                 const QString& expectedHex,
                                 const QString& presentedHex);

private slots:
    void onTlsConnected();
    void onTlsDisconnected();
    void onSslErrors(const QList<QSslError>& errors);
    void onSocketError(QAbstractSocket::SocketError error);
    void onReadyRead();
    void onHeartbeat();

private:
    void processLine(const QString& line);

    QSslSocket m_socket;
    QByteArray m_readBuffer;
    QTimer     m_heartbeat;
    QString    m_wanHandle;

    bool    m_connected{false};
    bool    m_validated{false};  // wan validate sent
    quint32 m_handle{0};
    std::atomic<quint32> m_seqCounter{1};
    quint32 m_lastPingSeq{0};
    QElapsedTimer m_pingStopwatch;

    // TOFU cert pin (GHSA-wfx7-w6p8-4jr2). Captured on first connect
    // to a given host; subsequent connects enforce on mismatch via the
    // certFingerprintMismatch signal + accept/reject methods (phase 2).
    QString m_host;
    QString m_expectedFingerprintHex;
    QString m_presentedFingerprintHex;  // pending on mismatch, awaiting UI decision
    bool    m_awaitingCertDecision{false};
    // Send wan validate once the pin decision lands (and at the end of
    // the normal first-use / match path too). Factored out so the
    // mismatch-accept path lands at exactly the same handshake step
    // the no-prompt paths use.
    void sendWanValidate();

    QMap<quint32, ResponseCallback> m_pendingCallbacks;
};

} // namespace AetherSDR
