#pragma once

#include <QObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QFile>
#include <QTimer>

class QSaveFile;

#include <functional>
#include <utility>

namespace AetherSDR {

class RadioModel;

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

class DvkWavTransferTestAccess;

class DvkWavTransfer : public QObject {
    Q_OBJECT
    friend class DvkWavTransferTestAccess;
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
    enum Direction { None, Download, Upload };

    // Every deferred continuation below is bound to the transfer generation
    // that queued it, and every socket callback to the socket that raised it,
    // so a cancelled transfer's late reply cannot act on its replacement
    // (#5634 — same failure shape ProfileTransfer carried).
    bool isCurrent(quint64 generation) const;
    bool isCurrentSocket(quint64 generation, const QTcpSocket* expectedSocket) const;
    bool isCurrentServer(quint64 generation, const QTcpServer* expectedServer) const;
    quint64 nextAsyncId();
    void invalidateOperation();
    quint64 begin(Direction direction, int slotId);

    // Download (radio → client)
    std::function<void(int, const QString&)> makeDownloadPortCallback(quint64 generation,
                                                                      quint64 requestId);
    void handleDownloadPortReceived(quint64 generation, quint64 requestId,
                                    int code, const QString& body);
    void handleNewConnection(quint64 generation, QTcpServer* server);
    void handleReadyRead(quint64 generation, QTcpSocket* socket);
    void handleDownloadFinished(quint64 generation, QTcpSocket* socket);
    void handleDownloadError(quint64 generation, QTcpSocket* socket);
    bool openDownloadFile();
    void receiveDownloadBytes(const QByteArray& data);
    void finalizeDownload();

    // Upload (client → radio)
    std::function<void(int, const QString&)> makeUploadPortCallback(quint64 generation,
                                                                    quint64 requestId);
    void handleUploadPortReceived(quint64 generation, quint64 requestId,
                                  int code, const QString& body);
    std::function<void()> makeUploadConnectCallback(quint64 generation, QTcpSocket* socket,
                                                    std::function<void()> connectAction);
    void handleUploadConnected(quint64 generation, QTcpSocket* socket);
    void handleUploadBytesWritten(quint64 generation, QTcpSocket* socket, qint64 bytes);
    void handleUploadError(quint64 generation, QTcpSocket* socket);
    void sendNextChunk(quint64 generation, QTcpSocket* socket);

    void startConnectTimeout(quint64 generation);
    void stopConnectTimeout();

    void cleanup(bool discardDownload);

    // Single idempotent funnel: tears down, then emits finished() once.
    // Re-entrant calls (e.g. a second socket signal during teardown) are no-ops.
    void finish(bool success, const QString& message, bool discardDownload);

    QPointer<RadioModel> m_model;
    QTcpServer*  m_server{nullptr};    // download: we listen
    QTcpSocket*  m_client{nullptr};    // download: accepted socket / upload: our socket
    QSaveFile*   m_file{nullptr};      // download: staged output file
    QTimer*      m_timeout{nullptr};
    int          m_slotId{-1};
    QString      m_filePath;           // download: save path / upload: source path
    qint64       m_bytesReceived{0};
    QByteArray   m_uploadData;
    qint64       m_bytesSent{0};      // bytes confirmed drained by QTcpSocket
    qint64       m_bytesAccepted{0};  // bytes accepted by QTcpSocket::write()
    Direction    m_direction{None};
    bool         m_transferring{false};
    bool         m_cancelled{false};
    bool         m_finished{false};   // guards against re-entrant finish/cleanup
    bool         m_cleaningUp{false}; // guards against re-entrant cleanup()
    quint64      m_operationGeneration{0};
    quint64      m_nextAsyncId{0};
    quint64      m_portRequestId{0};
    quint64      m_connectTimeoutGeneration{0};

    static constexpr qint64 MAX_FILE_SIZE = 5'000'000;  // 5MB per FlexLib
    static constexpr int CONNECT_TIMEOUT_MS = 10'000;
    static constexpr int UPLOAD_CHUNK_SIZE = 65536;      // 64KB chunks
};

} // namespace AetherSDR
