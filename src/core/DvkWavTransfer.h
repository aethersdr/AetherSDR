#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QFile>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>

#include <functional>

namespace AetherSDR {

class RadioModel;
class DvkWavTransferTestAccess;

// Transfers DVK recordings between the radio and local WAV files.
//
// Download (radio → client):
//   1. Send "dvk download id=N" → radio responds with TCP port
//   2. Client opens QTcpServer on that port
//   3. Radio connects and streams the WAV file
//
// Upload (client → radio):
//   1. Send "dvk upload id=N" → radio responds with TCP port
//   2. Client connects QTcpSocket to radio:<port>
//   3. Client streams the WAV file to the radio
//
// WAV format: 2-channel, 32-bit float, 48 kHz, max 5 MB.

class DvkWavTransfer : public QObject {
    Q_OBJECT
public:
    explicit DvkWavTransfer(RadioModel* model, QObject* parent = nullptr);
    ~DvkWavTransfer() override;

    void download(int slotId, const QString& savePath);
    void upload(int slotId, const QString& filePath);
    void cancel();
    bool isTransferring() const { return m_transferring; }

    // Validate WAV file format without starting a transfer.
    // Returns true if valid; on failure, sets error with details.
    static bool validateWavFile(const QString& filePath, QString& error);

signals:
    void statusChanged(const QString& message);
    void finished(bool success, const QString& message);

private:
    friend class DvkWavTransferTestAccess;

    using Generation = quint64;
    using UploadConnectFunction = std::function<void(QTcpSocket*, quint16)>;
    using PortResponseCallback = std::function<void(int, const QString&)>;
    using DeferredConnectCallback = std::function<void()>;

    enum class Direction { None, Download, Upload };
    enum class Phase {
        Idle,
        WaitingForDownloadPort,
        WaitingForDownloadConnection,
        ReceivingDownload,
        WaitingForUploadPort,
        WaitingForUploadConnection,
        SendingUpload,
    };

    // Download (radio → client)
    void onDownloadPortReceived(Generation generation, int code, const QString& body);
    void onNewConnection(Generation generation, QTcpServer* server);
    void onReadyRead(Generation generation, QTcpSocket* socket);
    void onDownloadFinished(Generation generation, QTcpSocket* socket);
    void onDownloadError(Generation generation, QTcpSocket* socket);

    // Upload (client → radio)
    void onUploadPortReceived(Generation generation, int code, const QString& body);
    DeferredConnectCallback makeUploadConnectTimerCallback(Generation generation,
                                                            QPointer<QTcpSocket> socket,
                                                            quint16 port,
                                                            UploadConnectFunction connect);
    bool runUploadConnect(Generation generation, QTcpSocket* socket, quint16 port,
                          const UploadConnectFunction& connect);
    void onUploadConnected(Generation generation, QTcpSocket* socket);
    void onUploadBytesWritten(Generation generation, QTcpSocket* socket, qint64 bytes);
    void onUploadError(Generation generation, QTcpSocket* socket);
    void sendNextChunk(Generation generation, QTcpSocket* socket);

    void cleanup(bool removeFile);
    void armTimeout(Generation generation);
    void clearTimeout();
    void onTimeout(Generation generation);
    Generation beginOperation(Direction direction);
    PortResponseCallback makePortResponseCallback(Generation generation, Direction direction);
    bool isCurrent(Generation generation, Phase phase) const;
    Generation nextGeneration();

    // Single idempotent funnel: emits finished() once and tears down.
    // Re-entrant calls (e.g. a second socket signal during teardown) are no-ops.
    void finish(Generation generation, bool success, const QString& message, bool removeFile);

    RadioModel*  m_model{nullptr};
    QPointer<QTcpServer> m_server;     // download: we listen
    QPointer<QTcpSocket> m_client;     // download: accepted socket / upload: our socket
    QFile*       m_file{nullptr};      // download: output file
    QTimer*      m_timeout{nullptr};
    QMetaObject::Connection m_timeoutConnection;
    int          m_slotId{-1};
    QString      m_filePath;           // download: save path / upload: source path
    qint64       m_bytesReceived{0};
    QByteArray   m_uploadData;
    qint64       m_bytesSent{0};
    Direction    m_direction{Direction::None};
    Phase        m_phase{Phase::Idle};
    Generation   m_generation{0};
    bool         m_transferring{false};
    bool         m_cleaningUp{false}; // guards against re-entrant cleanup()

    static constexpr qint64 MAX_FILE_SIZE = 5'000'000;  // 5MB per FlexLib
    static constexpr int CONNECT_TIMEOUT_MS = 10'000;
    static constexpr int UPLOAD_CHUNK_SIZE = 65536;      // 64KB chunks
};

} // namespace AetherSDR
