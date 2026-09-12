#pragma once

#include <QByteArray>
#include <QMap>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QElapsedTimer>
#include <QTimer>

#include <functional>
#include <utility>

QT_BEGIN_NAMESPACE
class QTcpSocket;
QT_END_NAMESPACE

namespace AetherSDR {

class RadioModel;
class FirmwareUploaderTestAccess;

// Handles firmware file upload to a Flex radio via its file-upload TCP socket.
// A fully drained local socket only proves the bytes left the local write
// buffer. Radio receipt, firmware installation, and restart remain unconfirmed.
class FirmwareUploader : public QObject {
    Q_OBJECT
public:
    explicit FirmwareUploader(RadioModel* model, QObject* parent = nullptr);

    void upload(const QString& filePath);
    void cancel();

    bool isUploading() const { return m_uploading; }

    // What a finished operation actually established. A drained local socket,
    // `transfer=1.00`, and a command-channel disconnect are all consistent with
    // a successful install and with a silent failure, so none of them may be
    // reported as either (#5572). Only the radio's own `file update failed=`
    // settles it: FlexLib's ParseUpdateStatus treats any parseable `failed`
    // value as the terminal word on the update and tears down the command
    // channel on it (reference/FlexLib_API_v4.1.5.39794/FlexLib/Radio.cs
    // :12612-12631), so failed=0 is the radio saying "accepted, rebooting".
    enum class Outcome {
        Succeeded,    // the radio confirmed the image (failed=0)
        Unconfirmed,  // bytes left the host; the radio never said either way
        Failed,       // the radio rejected it, or the transfer did not complete
    };
    Q_ENUM(Outcome)

signals:
    void progressChanged(int percent, const QString& status);
    void finished(Outcome outcome, const QString& message);

private:
    friend class FirmwareUploaderTestAccess;

    using Generation = quint64;
    using WriteFunction = std::function<qint64(const char*, qint64)>;

    bool beginOperation(const QByteArray& fileData, const QString& fileName);
    void requestUploadPort(Generation generation);
    void onUploadPortReceived(Generation generation, int code, const QString& body);
    void connectUploadSocket(Generation generation, quint16 port);
    void tryFallbackPort(Generation generation);
    void onConnected(Generation generation, QTcpSocket* socket);
    void startSending(Generation generation, WriteFunction writer);
    void queueNextChunk(Generation generation);
    void onBytesWritten(Generation generation, QTcpSocket* socket, qint64 bytes);
    void acknowledgeBytes(Generation generation, qint64 bytes);
    void onDisconnected(Generation generation, QTcpSocket* socket);
    void handleDisconnected(Generation generation);
    void onError(Generation generation, QTcpSocket* socket);
    void onRadioStatus(Generation generation,
                       const QString& object,
                       const QMap<QString, QString>& kvs);
    void handleModelDisconnected(Generation generation);
    void handleConnectionStateChanged(bool connected);
    void markUploadDispatched();
    void armTimeout(Generation generation, int timeoutMs, const QString& message);
    void onTimeout(Generation generation, quint64 timeoutToken, const QString& message);
    void armOverallTimeout(Generation generation);
    int overallTimeoutMsFor(qint64 fileBytes) const;
    void onOverallTimeout(Generation generation, quint64 timeoutToken);
    void finishOperation(Generation generation, Outcome outcome, const QString& message);
    bool retryBarrierActive() const;
    void destroySocket(bool abortConnection = true);
    void disconnectStatusRelay();
    bool isCurrent(Generation generation) const;
    Generation nextGeneration();

    QPointer<RadioModel> m_model;
    QPointer<QTcpSocket> m_socket;
    QByteArray m_fileData;
    QString m_fileName;
    WriteFunction m_writer;
    QMetaObject::Connection m_statusConnection;
    QTimer m_timeoutTimer;
    QTimer m_overallTimeoutTimer;
    QString m_timeoutMessage;
    qint64 m_bytesQueued{0};
    qint64 m_bytesAcknowledged{0};
    qint64 m_pendingBytes{0};
    quint16 m_uploadPort{0};
    Generation m_generation{0};
    Generation m_timeoutGeneration{0};
    Generation m_overallTimeoutGeneration{0};
    quint64 m_timeoutToken{0};
    quint64 m_overallTimeoutToken{0};
    bool m_uploading{false};
    bool m_requiresFreshConnection{false};
    bool m_disconnectObserved{false};
    bool m_waitingForConfirmation{false};
    bool m_outcomeSettledByRadio{false};
    bool m_radioProgressSeen{false};
    int m_radioProgressPercent{0};
    QElapsedTimer m_radioProgressAge;
    // Constant in production; the test lowers it so the stale branch is
    // reachable without a five-second wall-clock wait.
    qint64 m_radioProgressStaleMs{kRadioProgressStaleMs};

    static constexpr qint64 kChunkSize = 64LL * 1024LL;
    static constexpr qint64 kMaxFileBytes = 500LL * 1024LL * 1024LL;
    static constexpr quint16 kDefaultPort = 4995;
    static constexpr quint16 kFallbackPort = 42607;
    static constexpr int kConnectDelayMs = 200;
    static constexpr int kUploadPortTimeoutMs = 10000;
    static constexpr int kConnectTimeoutMs = 10000;
    static constexpr int kUploadInactivityTimeoutMs = 30000;
    static constexpr int kConfirmationTimeoutMs = 120000;
    // Hard ceiling on one upload. Fixed at ten minutes this was not
    // reachable-bandwidth-aware: #5572's own image is 386,282,416 bytes, which
    // needs a sustained ~644 KB/s to finish inside it — fine on the reporter's
    // LAN (~57 s), but a SmartLink/WAN update on a modest uplink would be
    // killed mid-image and then refused a retry by the barrier above. The real
    // stall guard is kUploadInactivityTimeoutMs; this only bounds a transfer
    // that is progressing pathologically slowly, so size it off the image at a
    // deliberately pessimistic floor rate and keep ten minutes as the minimum.
    static constexpr int kOverallUploadBaseMs = 10 * 60 * 1000;
    static constexpr qint64 kMinExpectedBytesPerSec = 64LL * 1024LL;
    static constexpr int kOverallUploadCeilingMs = 2 * 60 * 60 * 1000;
    // Radio `transfer=` supersedes the local byte counter, but only while it is
    // actually arriving: if the status stream stalls while TCP keeps draining,
    // fall back to local progress rather than freezing the bar (#5572 review).
    static constexpr qint64 kRadioProgressStaleMs = 5000;
};

} // namespace AetherSDR
