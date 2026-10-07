#pragma once

#include <QObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QTimer>

class QSaveFile;

#include <functional>
#include <utility>

namespace AetherSDR {

class DvkModel;
class RadioModel;

// Transfers DVK recordings between the radio and local WAV files (SmartSDR API
// wiki, TCPIP-dvk).
// Download: "dvk download id=N" -> radio replies with a TCP port; client listens
// on it and the radio connects and streams the WAV.
// Upload: "dvk upload id=N" -> 0 (names the slot), then
// "file upload <size> dvk_recording" -> port; client connects to radio:<port>
// and streams. Imports are converted to 24 kHz mono 16-bit (DvkWavConverter).

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
    // Transferring, or inside the radio file server's busy window after an
    // upload (fw 4.2.20 answers 50000053 for ~2.2 s after the upload closes).
    bool isBusy() const;

    // Export accepts a connection only from the radio. An unknown radio
    // address (not connected) accepts any peer.
    static bool isRadioPeer(const QHostAddress& peer, const QHostAddress& radio);

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
    std::function<void(int, const QString&)> makeUploadSlotCallback(quint64 generation,
                                                                    quint64 requestId);
    void handleUploadSlotAccepted(quint64 generation, quint64 requestId, int code);
    void openUploadSocket(quint64 generation, int port);
    // One retry on 42607 for a named port that refused or never answered,
    // before any connection was made. False when it does not apply.
    bool retryUploadOnDefaultPort(quint64 generation);
    // A refusal on any transfer leg also reaches the DVK license latch.
    void reportRefusal(int code);
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

    using ReplyCallback = std::function<void(int, const QString&)>;
    bool canSendCommands() const;
    // Every DVK transfer verb leaves through this one call.
    void sendCommand(const QString& command, ReplyCallback callback);

    QPointer<RadioModel> m_model;
    // Set only by tests, to observe the command sequence without a radio.
    std::function<void(const QString&, ReplyCallback)> m_commandSender;
    // The `file upload … dvk_recording` leg goes through RadioModel's
    // requestFileUploadPort(); tests replace it here.
    std::function<void(qint64, ReplyCallback)> m_uploadPortRequester;
    // Admission and the license latch live on the DVK model.
    QPointer<DvkModel> m_dvk;
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
    int          m_uploadPort{0};
    bool         m_uploadFallbackTried{false};
    bool         m_uploadConnected{false};
    QElapsedTimer m_sinceUpload;      // started when an upload ends
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
    static constexpr qint64 MAX_IMPORT_FILE_SIZE = 64'000'000;  // source WAV, pre-conversion
    static constexpr quint16 DEFAULT_UPLOAD_PORT = 42607;
    static constexpr qint64 FILE_SERVER_SETTLE_MS = 3000;
    static constexpr int CONNECT_TIMEOUT_MS = 10'000;
    static constexpr int UPLOAD_CHUNK_SIZE = 65536;      // 64KB chunks
};

} // namespace AetherSDR
