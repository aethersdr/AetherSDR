#pragma once

#include <QByteArray>
#include <QMap>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
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

signals:
    void progressChanged(int percent, const QString& status);
    void finished(bool success, const QString& message);

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
    void onOverallTimeout(Generation generation, quint64 timeoutToken);
    void finishOperation(Generation generation, bool success, const QString& message);
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
    bool m_radioProgressSeen{false};
    int m_radioProgressPercent{0};

    static constexpr qint64 kChunkSize = 64LL * 1024LL;
    static constexpr qint64 kMaxFileBytes = 500LL * 1024LL * 1024LL;
    static constexpr quint16 kDefaultPort = 4995;
    static constexpr quint16 kFallbackPort = 42607;
    static constexpr int kConnectDelayMs = 200;
    static constexpr int kUploadPortTimeoutMs = 10000;
    static constexpr int kConnectTimeoutMs = 10000;
    static constexpr int kUploadInactivityTimeoutMs = 30000;
    static constexpr int kConfirmationTimeoutMs = 120000;
    static constexpr int kOverallUploadTimeoutMs = 10 * 60 * 1000;
};

} // namespace AetherSDR
