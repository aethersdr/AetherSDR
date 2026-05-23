#pragma once

#include <QObject>
#include <QTcpSocket>
#include <QByteArray>

namespace MasterSDR {

class RadioModel;

// Handles firmware file upload to the radio via TCP.
//
// Protocol:
//   1. Send "file filename <name>" on command channel
//   2. Send "file upload <size> update" — radio responds with TCP port
//   3. Open TCP to radio IP:port, stream the file
//   4. Radio validates, applies, reboots

class FirmwareUploader : public QObject {
    Q_OBJECT
public:
    explicit FirmwareUploader(RadioModel* model, QObject* parent = nullptr);

    // Start the upload process for the given .ssdr file
    void upload(const QString& filePath);

    // Cancel an in-progress upload
    void cancel();

    bool isUploading() const { return m_uploading; }

signals:
    void progressChanged(int percent, const QString& status);
    void finished(bool success, const QString& message);

private:
    void onUploadPortReceived(int code, const QString& body);
    void onConnected();
    void onBytesWritten(qint64 bytes);
    void onError();

    RadioModel*  m_model{nullptr};
    QTcpSocket   m_socket;
    QByteArray   m_fileData;
    QString      m_fileName;
    qint64       m_bytesSent{0};
    int          m_uploadPort{-1};
    bool         m_uploading{false};
    bool         m_cancelled{false};

    static constexpr int CHUNK_SIZE = 65536;  // 64KB chunks
    static constexpr int DEFAULT_PORT = 4995;
    static constexpr int FALLBACK_PORT = 42607;
};

} // namespace MasterSDR
