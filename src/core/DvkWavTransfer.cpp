#include "DvkWavTransfer.h"
#include "../models/RadioModel.h"
#include "../core/RadioConnection.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QtEndian>

namespace MasterSDR {

DvkWavTransfer::DvkWavTransfer(RadioModel* model, QObject* parent)
    : QObject(parent), m_model(model)
{
    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this]() {
        if (!m_transferring) return;
        if (m_direction == Download && !m_client) {
            emit finished(false, "Timed out waiting for radio connection");
            cleanup(true);
        } else if (m_direction == Upload && m_client &&
                   m_client->state() != QAbstractSocket::ConnectedState) {
            emit finished(false, "Timed out connecting to radio upload port");
            cleanup(false);
        }
    });
}

DvkWavTransfer::~DvkWavTransfer()
{
    if (m_transferring) cleanup(m_direction == Download);
}

// ── Download (radio → client) ──────────────────────────────────────────────

void DvkWavTransfer::download(int slotId, const QString& savePath)
{
    if (m_transferring) {
        emit finished(false, "Transfer already in progress");
        return;
    }

    m_slotId = slotId;
    m_filePath = savePath;
    m_bytesReceived = 0;
    m_direction = Download;
    m_transferring = true;
    m_cancelled = false;

    emit statusChanged(QString("Requesting export of slot %1…").arg(slotId));

    m_model->sendCmdPublic(
        QString("dvk download id=%1").arg(slotId),
        [this](int code, const QString& body) {
            onDownloadPortReceived(code, body);
        });
}

void DvkWavTransfer::onDownloadPortReceived(int code, const QString& body)
{
    if (m_cancelled) return;

    if (code != 0) {
        emit finished(false, QString("Radio rejected download (error 0x%1)").arg(code, 0, 16));
        cleanup(false);
        return;
    }

    bool ok = false;
    int port = body.trimmed().toInt(&ok);
    if (!ok || port <= 0 || port > 65535) {
        emit finished(false, QString("Invalid port in response: %1").arg(body.trimmed()));
        cleanup(false);
        return;
    }

    QDir().mkpath(QFileInfo(m_filePath).absolutePath());

    m_file = new QFile(m_filePath, this);
    if (!m_file->open(QIODevice::WriteOnly)) {
        emit finished(false, "Cannot create file: " + m_file->errorString());
        cleanup(false);
        return;
    }

    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &DvkWavTransfer::onNewConnection);

    if (!m_server->listen(QHostAddress::Any, static_cast<quint16>(port))) {
        emit finished(false, QString("Cannot listen on port %1: %2")
                          .arg(port).arg(m_server->errorString()));
        cleanup(true);
        return;
    }

    qDebug() << "DvkWavTransfer: listening on port" << port << "for slot" << m_slotId;
    emit statusChanged(QString("Waiting for radio on port %1…").arg(port));
    m_timeout->start(CONNECT_TIMEOUT_MS);
}

void DvkWavTransfer::onNewConnection()
{
    m_timeout->stop();

    m_client = m_server->nextPendingConnection();
    if (!m_client) return;

    m_server->close();

    connect(m_client, &QTcpSocket::readyRead, this, &DvkWavTransfer::onReadyRead);
    connect(m_client, &QTcpSocket::disconnected, this, &DvkWavTransfer::onDownloadFinished);
    connect(m_client, &QTcpSocket::errorOccurred, this, [this]() { onDownloadError(); });

    qDebug() << "DvkWavTransfer: radio connected, receiving WAV data";
    emit statusChanged(QString("Exporting slot %1…").arg(m_slotId));
}

void DvkWavTransfer::onReadyRead()
{
    if (m_cancelled || !m_file) return;

    const QByteArray data = m_client->readAll();
    m_bytesReceived += data.size();

    if (m_bytesReceived > MAX_FILE_SIZE) {
        qWarning() << "DvkWavTransfer: file exceeds" << MAX_FILE_SIZE << "bytes, truncating";
        m_file->write(data.constData(), data.size() - (m_bytesReceived - MAX_FILE_SIZE));
        m_client->abort();
        emit finished(true, QString("Export complete (truncated at %1 KB)")
                          .arg(MAX_FILE_SIZE / 1024));
        cleanup(false);
        return;
    }

    m_file->write(data);
}

void DvkWavTransfer::onDownloadFinished()
{
    if (m_cancelled) return;

    if (m_bytesReceived == 0) {
        emit finished(false, "Radio sent no data");
        cleanup(true);
        return;
    }

    qDebug() << "DvkWavTransfer: export complete," << m_bytesReceived << "bytes";
    emit finished(true, QString("Exported slot %1 (%2 KB)")
                      .arg(m_slotId).arg(m_bytesReceived / 1024));
    cleanup(false);
}

void DvkWavTransfer::onDownloadError()
{
    if (m_cancelled) return;

    QString err = m_client ? m_client->errorString() : "Unknown error";
    if (m_client && m_client->error() == QAbstractSocket::RemoteHostClosedError && m_bytesReceived > 0) {
        onDownloadFinished();
        return;
    }

    emit finished(false, "Transfer error: " + err);
    cleanup(true);
}

// ── Upload (client → radio) ────────────────────────────────────────────────

void DvkWavTransfer::upload(int slotId, const QString& filePath)
{
    if (m_transferring) {
        emit finished(false, "Transfer already in progress");
        return;
    }

    // Validate WAV format
    QString error;
    if (!validateWavFile(filePath, error)) {
        emit finished(false, error);
        return;
    }

    // Read file into memory
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        emit finished(false, "Cannot open file: " + f.errorString());
        return;
    }
    m_uploadData = f.readAll();
    f.close();

    if (m_uploadData.isEmpty()) {
        emit finished(false, "File is empty");
        return;
    }

    m_slotId = slotId;
    m_filePath = filePath;
    m_bytesSent = 0;
    m_direction = Upload;
    m_transferring = true;
    m_cancelled = false;

    emit statusChanged(QString("Requesting upload to slot %1…").arg(slotId));

    m_model->sendCmdPublic(
        QString("dvk upload id=%1").arg(slotId),
        [this](int code, const QString& body) {
            onUploadPortReceived(code, body);
        });
}

void DvkWavTransfer::onUploadPortReceived(int code, const QString& body)
{
    if (m_cancelled) return;

    if (code != 0) {
        emit finished(false, QString("Radio rejected upload (error 0x%1)").arg(code, 0, 16));
        cleanup(false);
        return;
    }

    bool ok = false;
    int port = body.trimmed().toInt(&ok);
    if (!ok || port <= 0 || port > 65535) {
        emit finished(false, QString("Invalid port in response: %1").arg(body.trimmed()));
        cleanup(false);
        return;
    }

    qDebug() << "DvkWavTransfer: connecting to upload port" << port << "for slot" << m_slotId;
    emit statusChanged(QString("Connecting to port %1…").arg(port));

    m_client = new QTcpSocket(this);
    connect(m_client, &QTcpSocket::connected, this, &DvkWavTransfer::onUploadConnected);
    connect(m_client, &QTcpSocket::bytesWritten, this, &DvkWavTransfer::onUploadBytesWritten);
    connect(m_client, &QTcpSocket::errorOccurred, this, [this]() { onUploadError(); });

    // Small delay to let the radio set up its server (matches FirmwareUploader)
    QTimer::singleShot(200, this, [this, port]() {
        if (m_cancelled) return;
        m_client->connectToHost(m_model->radioAddress(), static_cast<quint16>(port));
    });

    m_timeout->start(CONNECT_TIMEOUT_MS);
}

void DvkWavTransfer::onUploadConnected()
{
    if (m_cancelled) return;
    m_timeout->stop();

    qDebug() << "DvkWavTransfer: connected, sending" << m_uploadData.size() << "bytes";
    emit statusChanged(QString("Uploading to slot %1…").arg(m_slotId));

    sendNextChunk();
}

void DvkWavTransfer::sendNextChunk()
{
    if (m_cancelled || !m_client) return;

    const qint64 remaining = m_uploadData.size() - m_bytesSent;
    if (remaining <= 0) return;

    const qint64 toSend = qMin(static_cast<qint64>(UPLOAD_CHUNK_SIZE), remaining);
    m_client->write(m_uploadData.constData() + m_bytesSent, toSend);
}

void DvkWavTransfer::onUploadBytesWritten(qint64 bytes)
{
    if (m_cancelled) return;

    m_bytesSent += bytes;
    const int percent = static_cast<int>(m_bytesSent * 100 / m_uploadData.size());
    emit statusChanged(QString("Uploading to slot %1… %2%").arg(m_slotId).arg(percent));

    if (m_bytesSent >= m_uploadData.size()) {
        qDebug() << "DvkWavTransfer: upload complete," << m_bytesSent << "bytes";
        m_client->flush();
        m_client->disconnectFromHost();
        emit finished(true, QString("Uploaded to slot %1 (%2 KB)")
                          .arg(m_slotId).arg(m_bytesSent / 1024));
        cleanup(false);
        return;
    }

    sendNextChunk();
}

void DvkWavTransfer::onUploadError()
{
    if (m_cancelled) return;

    QString err = m_client ? m_client->errorString() : "Unknown error";
    emit finished(false, "Upload error: " + err);
    cleanup(false);
}

// ── WAV validation ─────────────────────────────────────────────────────────

bool DvkWavTransfer::validateWavFile(const QString& filePath, QString& error)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        error = "Cannot open file";
        return false;
    }

    if (f.size() > MAX_FILE_SIZE) {
        error = QString("File too large (%1 KB, max %2 KB)")
                    .arg(f.size() / 1024).arg(MAX_FILE_SIZE / 1024);
        return false;
    }

    QByteArray header = f.read(44);
    f.close();

    if (header.size() < 44) {
        error = "File too small for WAV header";
        return false;
    }

    // RIFF / WAVE check
    if (header.mid(0, 4) != "RIFF" || header.mid(8, 4) != "WAVE") {
        error = "Not a valid WAV file";
        return false;
    }

    // fmt chunk fields (standard 44-byte header layout)
    quint16 audioFormat   = qFromLittleEndian<quint16>(header.constData() + 20);
    quint16 numChannels   = qFromLittleEndian<quint16>(header.constData() + 22);
    quint32 sampleRate    = qFromLittleEndian<quint32>(header.constData() + 24);
    quint16 bitsPerSample = qFromLittleEndian<quint16>(header.constData() + 34);

    if (audioFormat != 3) {  // 3 = IEEE float
        error = QString("Requires 32-bit float format (got %1-bit %2)")
                    .arg(bitsPerSample)
                    .arg(audioFormat == 1 ? "PCM" : "unknown");
        return false;
    }
    if (numChannels != 2) {
        error = QString("Requires stereo (got %1 channel%2)")
                    .arg(numChannels).arg(numChannels == 1 ? "" : "s");
        return false;
    }
    if (sampleRate != 48000) {
        error = QString("Requires 48 kHz sample rate (got %1 Hz)").arg(sampleRate);
        return false;
    }
    if (bitsPerSample != 32) {
        error = QString("Requires 32-bit samples (got %1-bit)").arg(bitsPerSample);
        return false;
    }

    return true;
}

// ── Shared cleanup ─────────────────────────────────────────────────────────

void DvkWavTransfer::cancel()
{
    if (!m_transferring) return;
    m_cancelled = true;
    emit finished(false, "Transfer cancelled");
    cleanup(m_direction == Download);
}

void DvkWavTransfer::cleanup(bool removeFile)
{
    m_timeout->stop();
    m_transferring = false;
    m_direction = None;

    if (m_client) {
        m_client->abort();
        m_client->deleteLater();
        m_client = nullptr;
    }

    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }

    if (m_file) {
        m_file->close();
        if (removeFile)
            m_file->remove();
        m_file->deleteLater();
        m_file = nullptr;
    }

    m_uploadData.clear();
    m_bytesReceived = 0;
    m_bytesSent = 0;
}

} // namespace MasterSDR
