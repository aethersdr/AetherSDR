#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QFile>
#include <QTimer>

namespace MasterSDR {

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
    // Download (radio → client)
    void onDownloadPortReceived(int code, const QString& body);
    void onNewConnection();
    void onReadyRead();
    void onDownloadFinished();
    void onDownloadError();

    // Upload (client → radio)
    void onUploadPortReceived(int code, const QString& body);
    void onUploadConnected();
    void onUploadBytesWritten(qint64 bytes);
    void onUploadError();
    void sendNextChunk();

    void cleanup(bool removeFile);

    enum Direction { None, Download, Upload };

    RadioModel*  m_model{nullptr};
    QTcpServer*  m_server{nullptr};    // download: we listen
    QTcpSocket*  m_client{nullptr};    // download: accepted socket / upload: our socket
    QFile*       m_file{nullptr};      // download: output file
    QTimer*      m_timeout{nullptr};
    int          m_slotId{-1};
    QString      m_filePath;           // download: save path / upload: source path
    qint64       m_bytesReceived{0};
    QByteArray   m_uploadData;
    qint64       m_bytesSent{0};
    Direction    m_direction{None};
    bool         m_transferring{false};
    bool         m_cancelled{false};

    static constexpr qint64 MAX_FILE_SIZE = 5'000'000;  // 5MB per FlexLib
    static constexpr int CONNECT_TIMEOUT_MS = 10'000;
    static constexpr int UPLOAD_CHUNK_SIZE = 65536;      // 64KB chunks
};

} // namespace MasterSDR
