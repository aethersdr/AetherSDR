#include "DvkWavTransfer.h"
#include "../models/DvkModel.h"
#include "../models/RadioModel.h"
#include "../core/RadioConnection.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QtEndian>

#include <limits>

namespace AetherSDR {

DvkWavTransfer::DvkWavTransfer(RadioModel* model, QObject* parent)
    : QObject(parent), m_model(model)
{
    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
}

DvkWavTransfer::~DvkWavTransfer()
{
    if (m_transferring) {
        cleanup(m_direction == Direction::Download);
    }
}

// ── Download (radio → client) ──────────────────────────────────────────────

void DvkWavTransfer::download(int slotId, const QString& savePath)
{
    if (m_transferring || m_cleaningUp) {
        emit finished(false, "Transfer already in progress");
        return;
    }

    m_slotId = slotId;
    m_filePath = savePath;
    m_bytesReceived = 0;
    const Generation generation = beginOperation(Direction::Download);
    const QPointer<DvkWavTransfer> self(this);

    emit statusChanged(QString("Requesting export of slot %1…").arg(slotId));
    if (!self || !self->isCurrent(generation, Phase::WaitingForDownloadPort)) {
        return;
    }

    self->m_model->sendCmdPublic(
        QString("dvk download id=%1").arg(slotId),
        self->makePortResponseCallback(generation, Direction::Download));
}

void DvkWavTransfer::onDownloadPortReceived(Generation generation, int code, const QString& body)
{
    if (!isCurrent(generation, Phase::WaitingForDownloadPort)) {
        return;
    }

    if (code != 0) {
        finish(generation, false, QString("Radio rejected download — %1")
                   .arg(DvkModel::dvkErrorString(static_cast<uint>(code))),
               false);
        return;
    }

    bool ok = false;
    int port = body.trimmed().toInt(&ok);
    if (!ok || port <= 0 || port > 65535) {
        finish(generation, false, QString("Invalid port in response: %1").arg(body.trimmed()), false);
        return;
    }

    QDir().mkpath(QFileInfo(m_filePath).absolutePath());

    m_file = new QFile(m_filePath, this);
    if (!m_file->open(QIODevice::WriteOnly)) {
        finish(generation, false, "Cannot create file: " + m_file->errorString(), false);
        return;
    }

    m_server = new QTcpServer(this);
    const QPointer<DvkWavTransfer> self(this);
    const QPointer<QTcpServer> server = m_server;
    connect(m_server, &QTcpServer::newConnection, this, [self, generation, server]() {
        if (self && server) {
            self->onNewConnection(generation, server);
        }
    });

    if (!m_server->listen(QHostAddress::Any, static_cast<quint16>(port))) {
        finish(generation, false, QString("Cannot listen on port %1: %2")
                   .arg(port).arg(m_server->errorString()), true);
        return;
    }

    m_phase = Phase::WaitingForDownloadConnection;
    qDebug() << "DvkWavTransfer: listening on port" << port << "for slot" << m_slotId;
    armTimeout(generation);
    emit statusChanged(QString("Waiting for radio on port %1…").arg(port));
}

DvkWavTransfer::PortResponseCallback DvkWavTransfer::makePortResponseCallback(
    Generation generation, Direction direction)
{
    const QPointer<DvkWavTransfer> self(this);
    return [self, generation, direction](int code, const QString& body) {
        if (!self) {
            return;
        }
        if (direction == Direction::Download) {
            self->onDownloadPortReceived(generation, code, body);
        } else {
            self->onUploadPortReceived(generation, code, body);
        }
    };
}

void DvkWavTransfer::onNewConnection(Generation generation, QTcpServer* server)
{
    if (!server || !isCurrent(generation, Phase::WaitingForDownloadConnection)
        || server != m_server) {
        return;
    }
    clearTimeout();

    m_client = server->nextPendingConnection();
    if (!m_client) return;

    server->close();
    m_phase = Phase::ReceivingDownload;

    const QPointer<DvkWavTransfer> self(this);
    const QPointer<QTcpSocket> client = m_client;
    connect(m_client, &QTcpSocket::readyRead, this, [self, generation, client]() {
        if (self && client) {
            self->onReadyRead(generation, client);
        }
    });
    connect(m_client, &QTcpSocket::disconnected, this, [self, generation, client]() {
        if (self && client) {
            self->onDownloadFinished(generation, client);
        }
    });
    connect(m_client, &QTcpSocket::errorOccurred, this,
            [self, generation, client](QAbstractSocket::SocketError) {
                if (self && client) {
                    self->onDownloadError(generation, client);
                }
            });

    qDebug() << "DvkWavTransfer: radio connected, receiving WAV data";
    emit statusChanged(QString("Exporting slot %1…").arg(m_slotId));
}

void DvkWavTransfer::onReadyRead(Generation generation, QTcpSocket* socket)
{
    if (!socket || !isCurrent(generation, Phase::ReceivingDownload) || socket != m_client
        || !m_file) {
        return;
    }

    const QByteArray data = socket->readAll();
    m_bytesReceived += data.size();

    if (m_bytesReceived > MAX_FILE_SIZE) {
        qWarning() << "DvkWavTransfer: file exceeds" << MAX_FILE_SIZE << "bytes, truncating";
        m_file->write(data.constData(), data.size() - (m_bytesReceived - MAX_FILE_SIZE));
        finish(generation, true, QString("Export complete (truncated at %1 KB)")
                   .arg(MAX_FILE_SIZE / 1024), false);
        return;
    }

    m_file->write(data);
}

void DvkWavTransfer::onDownloadFinished(Generation generation, QTcpSocket* socket)
{
    if (!socket || !isCurrent(generation, Phase::ReceivingDownload) || socket != m_client) {
        return;
    }

    if (m_bytesReceived == 0) {
        finish(generation, false, "Radio sent no data", true);
        return;
    }

    qDebug() << "DvkWavTransfer: export complete," << m_bytesReceived << "bytes";
    finish(generation, true, QString("Exported slot %1 (%2 KB)")
               .arg(m_slotId).arg(m_bytesReceived / 1024), false);
}

void DvkWavTransfer::onDownloadError(Generation generation, QTcpSocket* socket)
{
    if (!socket || !isCurrent(generation, Phase::ReceivingDownload) || socket != m_client) {
        return;
    }

    // A clean close after we've already received data is a successful end of
    // transfer, not an error. The radio fires errorOccurred(RemoteHostClosed)
    // and disconnected() together; route both through the same idempotent path.
    if (socket->error() == QAbstractSocket::RemoteHostClosedError && m_bytesReceived > 0) {
        onDownloadFinished(generation, socket);
        return;
    }

    finish(generation, false, "Transfer error: " + socket->errorString(), true);
}

// ── Upload (client → radio) ────────────────────────────────────────────────

void DvkWavTransfer::upload(int slotId, const QString& filePath)
{
    if (m_transferring || m_cleaningUp) {
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
    const Generation generation = beginOperation(Direction::Upload);
    const QPointer<DvkWavTransfer> self(this);

    emit statusChanged(QString("Requesting upload to slot %1…").arg(slotId));
    if (!self || !self->isCurrent(generation, Phase::WaitingForUploadPort)) {
        return;
    }

    self->m_model->sendCmdPublic(
        QString("dvk upload id=%1").arg(slotId),
        self->makePortResponseCallback(generation, Direction::Upload));
}

void DvkWavTransfer::onUploadPortReceived(Generation generation, int code, const QString& body)
{
    if (!isCurrent(generation, Phase::WaitingForUploadPort)) {
        return;
    }

    if (code != 0) {
        finish(generation, false, QString("Radio rejected upload — %1")
                   .arg(DvkModel::dvkErrorString(static_cast<uint>(code))),
               false);
        return;
    }

    bool ok = false;
    int port = body.trimmed().toInt(&ok);
    if (!ok || port <= 0 || port > 65535) {
        finish(generation, false, QString("Invalid port in response: %1").arg(body.trimmed()), false);
        return;
    }

    qDebug() << "DvkWavTransfer: connecting to upload port" << port << "for slot" << m_slotId;
    m_client = new QTcpSocket(this);
    m_phase = Phase::WaitingForUploadConnection;
    const QPointer<DvkWavTransfer> self(this);
    const QPointer<QTcpSocket> client = m_client;
    connect(m_client, &QTcpSocket::connected, this, [self, generation, client]() {
        if (self && client) {
            self->onUploadConnected(generation, client);
        }
    });
    connect(m_client, &QTcpSocket::bytesWritten, this, [self, generation, client](qint64 bytes) {
        if (self && client) {
            self->onUploadBytesWritten(generation, client, bytes);
        }
    });
    connect(m_client, &QTcpSocket::errorOccurred, this,
            [self, generation, client](QAbstractSocket::SocketError) {
                if (self && client) {
                    self->onUploadError(generation, client);
                }
    });

    // Small delay to let the radio set up its server (matches FirmwareUploader)
    const QPointer<RadioModel> model = m_model;
    QTimer::singleShot(200, this,
                       makeUploadConnectTimerCallback(
                           generation, client, static_cast<quint16>(port),
                           [model](QTcpSocket* socket, quint16 connectPort) {
                               if (model) {
                                   socket->connectToHost(model->radioAddress(), connectPort);
                               }
                           }));

    armTimeout(generation);
    emit statusChanged(QString("Connecting to port %1…").arg(port));
}

DvkWavTransfer::DeferredConnectCallback DvkWavTransfer::makeUploadConnectTimerCallback(
    Generation generation, QPointer<QTcpSocket> socket, quint16 port, UploadConnectFunction connect)
{
    const QPointer<DvkWavTransfer> self(this);
    return [self, generation, socket, port, connect = std::move(connect)]() {
        if (self && socket) {
            self->runUploadConnect(generation, socket, port, connect);
        }
    };
}

bool DvkWavTransfer::runUploadConnect(Generation generation, QTcpSocket* socket, quint16 port,
                                       const UploadConnectFunction& connect)
{
    if (!socket || !isCurrent(generation, Phase::WaitingForUploadConnection)
        || socket != m_client) {
        return false;
    }
    const QPointer<DvkWavTransfer> self(this);
    connect(socket, port);
    return self && self->isCurrent(generation, Phase::WaitingForUploadConnection)
        && socket == self->m_client;
}

void DvkWavTransfer::onUploadConnected(Generation generation, QTcpSocket* socket)
{
    if (!socket || !isCurrent(generation, Phase::WaitingForUploadConnection) || socket != m_client) {
        return;
    }
    clearTimeout();
    m_phase = Phase::SendingUpload;

    qDebug() << "DvkWavTransfer: connected, sending" << m_uploadData.size() << "bytes";
    const QPointer<DvkWavTransfer> self(this);
    emit statusChanged(QString("Uploading to slot %1…").arg(m_slotId));
    if (!self || !self->isCurrent(generation, Phase::SendingUpload) || socket != self->m_client) {
        return;
    }

    sendNextChunk(generation, socket);
}

void DvkWavTransfer::sendNextChunk(Generation generation, QTcpSocket* socket)
{
    if (!socket || !isCurrent(generation, Phase::SendingUpload) || socket != m_client) {
        return;
    }

    const qint64 remaining = m_uploadData.size() - m_bytesSent;
    if (remaining <= 0) {
        return;
    }

    const qint64 toSend = qMin(static_cast<qint64>(UPLOAD_CHUNK_SIZE), remaining);
    socket->write(m_uploadData.constData() + m_bytesSent, toSend);
}

void DvkWavTransfer::onUploadBytesWritten(Generation generation, QTcpSocket* socket, qint64 bytes)
{
    if (!socket || !isCurrent(generation, Phase::SendingUpload) || socket != m_client) {
        return;
    }

    m_bytesSent += bytes;
    const int percent = static_cast<int>(m_bytesSent * 100 / m_uploadData.size());
    const QPointer<DvkWavTransfer> self(this);
    emit statusChanged(QString("Uploading to slot %1… %2%").arg(m_slotId).arg(percent));
    if (!self || !self->isCurrent(generation, Phase::SendingUpload) || socket != self->m_client) {
        return;
    }

    if (m_bytesSent >= m_uploadData.size()) {
        qDebug() << "DvkWavTransfer: upload complete," << m_bytesSent << "bytes";
        socket->flush();
        socket->disconnectFromHost();
        if (!self || !self->isCurrent(generation, Phase::SendingUpload) || socket != self->m_client) {
            return;
        }
        finish(generation, true, QString("Uploaded to slot %1 (%2 KB)")
                   .arg(m_slotId).arg(m_bytesSent / 1024), false);
        return;
    }

    sendNextChunk(generation, socket);
}

void DvkWavTransfer::onUploadError(Generation generation, QTcpSocket* socket)
{
    if (!socket || (!isCurrent(generation, Phase::WaitingForUploadConnection)
         && !isCurrent(generation, Phase::SendingUpload))
        || socket != m_client) {
        return;
    }

    finish(generation, false, "Upload error: " + socket->errorString(), false);
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
    if (!m_transferring) {
        return;
    }
    finish(m_generation, false, "Transfer cancelled", m_direction == Direction::Download);
}

void DvkWavTransfer::finish(Generation generation, bool success, const QString& message,
                            bool removeFile)
{
    if (!m_transferring || generation != m_generation) {
        return;
    }

    // Invalidate every retained callback before socket teardown. This also
    // permits a finished() handler to begin a replacement operation without an
    // old callback being accepted into the new one.
    m_transferring = false;
    m_direction = Direction::None;
    m_phase = Phase::Idle;
    m_generation = nextGeneration();
    cleanup(removeFile);
    emit finished(success, message);
}

void DvkWavTransfer::cleanup(bool removeFile)
{
    // Re-entrancy guard: abort()/deleteLater() below can synchronously deliver
    // queued socket signals (disconnected/errorOccurred) that route back here.
    if (m_cleaningUp) {
        return;
    }
    m_cleaningUp = true;

    clearTimeout();

    // Disconnect every socket/server signal BEFORE tearing down so abort() and
    // deleteLater() cannot re-enter our slots and touch freed objects.
    if (m_client) {
        m_client->disconnect(this);
        m_client->abort();
        m_client->deleteLater();
        m_client = nullptr;
    }

    if (m_server) {
        m_server->disconnect(this);
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }

    if (m_file) {
        m_file->close();
        if (removeFile) {
            m_file->remove();
        }
        m_file->deleteLater();
        m_file = nullptr;
    }

    m_uploadData.clear();
    m_bytesReceived = 0;
    m_bytesSent = 0;

    m_cleaningUp = false;
}

void DvkWavTransfer::onTimeout(Generation generation)
{
    if (isCurrent(generation, Phase::WaitingForDownloadConnection) && !m_client) {
        finish(generation, false, "Timed out waiting for radio connection", true);
        return;
    }
    if (isCurrent(generation, Phase::WaitingForUploadConnection) && m_client
        && m_client->state() != QAbstractSocket::ConnectedState) {
        finish(generation, false, "Timed out connecting to radio upload port", false);
    }
}

void DvkWavTransfer::armTimeout(Generation generation)
{
    clearTimeout();
    const QPointer<DvkWavTransfer> self(this);
    m_timeoutConnection = connect(m_timeout, &QTimer::timeout, this, [self, generation] {
        if (self) {
            self->onTimeout(generation);
        }
    });
    m_timeout->start(CONNECT_TIMEOUT_MS);
}

void DvkWavTransfer::clearTimeout()
{
    if (m_timeout) {
        m_timeout->stop();
    }
    if (m_timeoutConnection) {
        disconnect(m_timeoutConnection);
        m_timeoutConnection = {};
    }
}

DvkWavTransfer::Generation DvkWavTransfer::beginOperation(Direction direction)
{
    m_generation = nextGeneration();
    m_direction = direction;
    m_phase = direction == Direction::Download ? Phase::WaitingForDownloadPort
                                                : Phase::WaitingForUploadPort;
    m_transferring = true;
    return m_generation;
}

bool DvkWavTransfer::isCurrent(Generation generation, Phase phase) const
{
    return m_transferring && m_generation == generation && m_phase == phase;
}

DvkWavTransfer::Generation DvkWavTransfer::nextGeneration()
{
    return m_generation == std::numeric_limits<Generation>::max() ? 1 : m_generation + 1;
}

} // namespace AetherSDR
