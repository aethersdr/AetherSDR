#include "DvkWavTransfer.h"
#include "DvkWavConverter.h"
#include "../models/DvkModel.h"
#include "../models/RadioModel.h"
#include "core/backends/flex/RadioConnection.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QSaveFile>

namespace AetherSDR {

DvkWavTransfer::DvkWavTransfer(RadioModel* model, QObject* parent)
    : QObject(parent), m_model(model), m_dvk(model ? &model->dvkModel() : nullptr)
{
    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this]() {
        if (!isCurrent(m_connectTimeoutGeneration)) return;
        if (m_direction == Download && !m_client) {
            finish(false, "Timed out waiting for radio connection", true);
        } else if (m_direction == Upload && m_client &&
                   m_client->state() != QAbstractSocket::ConnectedState) {
            if (retryUploadOnDefaultPort(m_connectTimeoutGeneration)) {
                return;
            }
            finish(false, "Timed out connecting to radio upload port", false);
        }
    });
}

DvkWavTransfer::~DvkWavTransfer()
{
    if (m_transferring) {
        cleanup(m_direction == Download);
    }
}

bool DvkWavTransfer::canSendCommands() const
{
    return m_commandSender || m_model;
}

bool DvkWavTransfer::isBusy() const
{
    return m_transferring
        || (m_sinceUpload.isValid() && m_sinceUpload.elapsed() < FILE_SERVER_SETTLE_MS);
}

bool DvkWavTransfer::isRadioPeer(const QHostAddress& peer, const QHostAddress& radio)
{
    return radio.isNull() || peer.isEqual(radio, QHostAddress::ConvertV4MappedToIPv4);
}

void DvkWavTransfer::reportRefusal(int code)
{
    if (m_dvk) {
        m_dvk->noteRefusal(static_cast<uint>(code));
    }
}

void DvkWavTransfer::sendCommand(const QString& command, ReplyCallback callback)
{
    if (m_commandSender) {
        m_commandSender(command, std::move(callback));
    } else if (m_model) {
        m_model->sendCmdPublic(command, std::move(callback));
    }
}

// ── Transfer identity ──────────────────────────────────────────────────────

bool DvkWavTransfer::isCurrent(quint64 generation) const
{
    return m_transferring && !m_cancelled && !m_finished
        && m_operationGeneration == generation;
}

bool DvkWavTransfer::isCurrentSocket(quint64 generation, const QTcpSocket* expectedSocket) const
{
    return isCurrent(generation) && m_client == expectedSocket;
}

bool DvkWavTransfer::isCurrentServer(quint64 generation, const QTcpServer* expectedServer) const
{
    return isCurrent(generation) && m_server == expectedServer;
}

quint64 DvkWavTransfer::nextAsyncId()
{
    ++m_nextAsyncId;
    if (m_nextAsyncId == 0) {
        ++m_nextAsyncId;
    }
    return m_nextAsyncId;
}

void DvkWavTransfer::invalidateOperation()
{
    ++m_operationGeneration;
    if (m_operationGeneration == 0) {
        ++m_operationGeneration;
    }
    m_portRequestId = 0;
    m_connectTimeoutGeneration = 0;
}

quint64 DvkWavTransfer::begin(Direction direction, int slotId)
{
    invalidateOperation();
    m_slotId = slotId;
    m_bytesReceived = 0;
    m_bytesSent = 0;
    m_bytesAccepted = 0;
    m_direction = direction;
    m_transferring = true;
    m_cancelled = false;
    m_finished = false;
    m_uploadPort = 0;
    m_uploadFallbackTried = false;
    m_uploadConnected = false;
    if (m_dvk) {
        m_dvk->setTransferActive(true);
    }
    return m_operationGeneration;
}

void DvkWavTransfer::startConnectTimeout(quint64 generation)
{
    m_connectTimeoutGeneration = generation;
    m_timeout->start(CONNECT_TIMEOUT_MS);
}

void DvkWavTransfer::stopConnectTimeout()
{
    m_timeout->stop();
    m_connectTimeoutGeneration = 0;
}

// ── Download (radio → client) ──────────────────────────────────────────────

void DvkWavTransfer::download(int slotId, const QString& savePath)
{
    if (m_transferring) {
        emit finished(false, "Transfer already in progress");
        return;
    }
    if (isBusy()) {
        emit finished(false, "The radio's file server is still busy; try again in a moment");
        return;
    }

    if (!canSendCommands()) {
        emit finished(false, "The radio connection was lost before the transfer could start.");
        return;
    }

    const quint64 generation = begin(Download, slotId);
    m_filePath = savePath;

    emit statusChanged(QString("Requesting export of slot %1…").arg(slotId));
    if (!isCurrent(generation)) {
        return;
    }

    const quint64 requestId = nextAsyncId();
    m_portRequestId = requestId;
    sendCommand(QString("dvk download id=%1").arg(slotId),
                makeDownloadPortCallback(generation, requestId));
}

std::function<void(int, const QString&)> DvkWavTransfer::makeDownloadPortCallback(
    quint64 generation, quint64 requestId)
{
    // RadioModel owns this callback and can outlive us: the reply for a
    // cancelled request still arrives and is still dispatched.
    QPointer<DvkWavTransfer> transfer(this);
    return [transfer, generation, requestId](int code, const QString& body) {
        if (!transfer) {
            return;
        }
        transfer->handleDownloadPortReceived(generation, requestId, code, body);
    };
}

void DvkWavTransfer::handleDownloadPortReceived(quint64 generation, quint64 requestId,
                                                int code, const QString& body)
{
    if (!isCurrent(generation) || requestId == 0 || m_portRequestId != requestId) return;
    m_portRequestId = 0;

    if (code != 0) {
        reportRefusal(code);
        finish(false, QString("Radio rejected download — %1")
                   .arg(DvkModel::dvkErrorString(static_cast<uint>(code))),
               false);
        return;
    }

    bool ok = false;
    int port = body.trimmed().toInt(&ok);
    if (!ok || port <= 0 || port > 65535) {
        finish(false, QString("Invalid port in response: %1").arg(body.trimmed()), false);
        return;
    }

    if (!openDownloadFile()) {
        return;
    }

    m_server = new QTcpServer(this);
    QPointer<DvkWavTransfer> transfer(this);
    QPointer<QTcpServer> server(m_server);
    connect(m_server, &QTcpServer::newConnection, this, [transfer, generation, server]() {
        if (transfer && server) {
            transfer->handleNewConnection(generation, server);
        }
    });

    if (!m_server->listen(QHostAddress::Any, static_cast<quint16>(port))) {
        finish(false, QString("Cannot listen on port %1: %2")
                   .arg(port).arg(m_server->errorString()), true);
        return;
    }

    qDebug() << "DvkWavTransfer: listening on port" << port << "for slot" << m_slotId;
    emit statusChanged(QString("Waiting for radio on port %1…").arg(port));
    if (!isCurrent(generation)) {
        return;
    }
    startConnectTimeout(generation);
}

bool DvkWavTransfer::openDownloadFile()
{
    QDir().mkpath(QFileInfo(m_filePath).absolutePath());

    // Keep the previous export visible until the radio stream is complete.
    // Direct-write fallback would truncate the destination if its directory
    // cannot host a temporary file, defeating that guarantee.
    m_file = new QSaveFile(m_filePath, this);
    m_file->setDirectWriteFallback(false);
    if (!m_file->open(QIODevice::WriteOnly)) {
        finish(false, "Cannot create file: " + m_file->errorString(), true);
        return false;
    }

    return true;
}

void DvkWavTransfer::handleNewConnection(quint64 generation, QTcpServer* server)
{
    if (!isCurrentServer(generation, server) || m_client) return;

    QTcpSocket* pending = m_server->nextPendingConnection();
    if (!pending) return;
    // Anyone on the LAN can reach the listening port; only the radio may
    // write the operator's file. Others are dropped and the wait continues.
    if (m_model && !isRadioPeer(pending->peerAddress(), m_model->radioAddress())) {
        const QString peer = pending->peerAddress().toString();
        qWarning() << "DvkWavTransfer: rejected export connection from" << peer
                   << "expected" << m_model->radioAddress().toString();
        emit statusChanged(QString("Ignored a connection from %1; waiting for the radio at %2…")
                               .arg(peer, m_model->radioAddress().toString()));
        pending->abort();
        pending->deleteLater();
        return;
    }
    stopConnectTimeout();
    m_client = pending;

    m_server->close();

    QPointer<DvkWavTransfer> transfer(this);
    QPointer<QTcpSocket> socket(m_client);
    connect(m_client, &QTcpSocket::readyRead, this, [transfer, generation, socket]() {
        if (transfer && socket) {
            transfer->handleReadyRead(generation, socket);
        }
    });
    connect(m_client, &QTcpSocket::disconnected, this, [transfer, generation, socket]() {
        if (transfer && socket) {
            transfer->handleDownloadFinished(generation, socket);
        }
    });
    connect(m_client, &QTcpSocket::errorOccurred, this,
            [transfer, generation, socket](QAbstractSocket::SocketError) {
        if (transfer && socket) {
            transfer->handleDownloadError(generation, socket);
        }
    });

    qDebug() << "DvkWavTransfer: radio connected, receiving WAV data";
    emit statusChanged(QString("Exporting slot %1…").arg(m_slotId));
}

void DvkWavTransfer::handleReadyRead(quint64 generation, QTcpSocket* socket)
{
    if (!isCurrentSocket(generation, socket) || !m_file) return;

    receiveDownloadBytes(m_client->readAll());
}

void DvkWavTransfer::receiveDownloadBytes(const QByteArray& data)
{
    if (m_cancelled || m_finished || !m_file || data.isEmpty()) {
        return;
    }

    if (data.size() > MAX_FILE_SIZE - m_bytesReceived) {
        qWarning() << "DvkWavTransfer: file exceeds" << MAX_FILE_SIZE << "bytes";
        finish(false, QString("Export exceeds the %1 KB limit")
                   .arg(MAX_FILE_SIZE / 1024), true);
        return;
    }

    const qint64 written = m_file->write(data);
    if (written != data.size()) {
        finish(false, "Cannot write export file: " + m_file->errorString(), true);
        return;
    }

    m_bytesReceived += written;
}

void DvkWavTransfer::handleDownloadFinished(quint64 generation, QTcpSocket* socket)
{
    if (!isCurrentSocket(generation, socket)) return;

    finalizeDownload();
}

void DvkWavTransfer::finalizeDownload()
{
    if (m_cancelled || m_finished) {
        return;
    }

    if (m_bytesReceived == 0) {
        finish(false, "Radio sent no data", true);
        return;
    }

    if (!m_file || !m_file->commit()) {
        const QString error = m_file ? m_file->errorString() : QString("Output file is unavailable");
        finish(false, "Cannot finalize export file: " + error, true);
        return;
    }
    m_file->deleteLater();
    m_file = nullptr;

    qDebug() << "DvkWavTransfer: export complete," << m_bytesReceived << "bytes";
    finish(true, QString("Exported slot %1 (%2 KB)")
               .arg(m_slotId).arg(m_bytesReceived / 1024), false);
}

void DvkWavTransfer::handleDownloadError(quint64 generation, QTcpSocket* socket)
{
    if (!isCurrentSocket(generation, socket)) return;

    // A clean close after we've already received data is a successful end of
    // transfer, not an error. The radio fires errorOccurred(RemoteHostClosed)
    // and disconnected() together; route both through the same idempotent path.
    if (m_client && m_client->error() == QAbstractSocket::RemoteHostClosedError && m_bytesReceived > 0) {
        handleDownloadFinished(generation, socket);
        return;
    }

    const QString err = m_client ? m_client->errorString() : "Unknown error";
    finish(false, "Transfer error: " + err, true);
}

// ── Upload (client → radio) ────────────────────────────────────────────────

void DvkWavTransfer::upload(int slotId, const QString& filePath)
{
    if (m_transferring) {
        emit finished(false, "Transfer already in progress");
        return;
    }
    if (isBusy()) {
        emit finished(false, "The radio's file server is still busy; try again in a moment");
        return;
    }

    if (!canSendCommands()) {
        emit finished(false, "The radio connection was lost before the transfer could start.");
        return;
    }
    // Checked here, not only when the menu opened: the file dialog is modal,
    // and the DVK may have started recording or playing while it was up.
    if (m_dvk && !m_dvk->canStartOperation()) {
        emit finished(false, "The voice keyer is busy; stop it before importing");
        return;
    }

    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        emit finished(false, "Cannot open file: " + f.errorString());
        return;
    }
    if (f.size() > MAX_IMPORT_FILE_SIZE) {
        emit finished(false, QString("File too large (%1 KB, max %2 KB)")
                                 .arg(f.size() / 1024).arg(MAX_IMPORT_FILE_SIZE / 1024));
        return;
    }
    const QByteArray source = f.readAll();
    f.close();

    QString error;
    QByteArray converted = DvkWavConverter::convertForRadio(source, error);
    if (converted.isEmpty()) {
        emit finished(false, error);
        return;
    }

    const quint64 generation = begin(Upload, slotId);
    m_filePath = filePath;
    m_uploadData = std::move(converted);

    emit statusChanged(QString("Requesting upload to slot %1…").arg(slotId));
    if (!isCurrent(generation)) {
        return;
    }

    const quint64 requestId = nextAsyncId();
    m_portRequestId = requestId;
    sendCommand(QString("dvk upload id=%1").arg(slotId),
                makeUploadSlotCallback(generation, requestId));
}

std::function<void(int, const QString&)> DvkWavTransfer::makeUploadSlotCallback(
    quint64 generation, quint64 requestId)
{
    QPointer<DvkWavTransfer> transfer(this);
    return [transfer, generation, requestId](int code, const QString&) {
        if (!transfer) {
            return;
        }
        transfer->handleUploadSlotAccepted(generation, requestId, code);
    };
}

void DvkWavTransfer::handleUploadSlotAccepted(quint64 generation, quint64 requestId, int code)
{
    if (!isCurrent(generation) || requestId == 0 || m_portRequestId != requestId) return;
    m_portRequestId = 0;

    if (code != 0) {
        reportRefusal(code);
        finish(false, QString("Radio rejected upload — %1")
                   .arg(DvkModel::dvkErrorString(static_cast<uint>(code))),
               false);
        return;
    }

    // `dvk upload` only names the slot; the file itself rides the radio's
    // generic file-upload server under the dvk_recording type.
    const quint64 portRequestId = nextAsyncId();
    m_portRequestId = portRequestId;
    ReplyCallback onPort = makeUploadPortCallback(generation, portRequestId);
    if (m_uploadPortRequester) {
        m_uploadPortRequester(m_uploadData.size(), std::move(onPort));
    } else if (m_model) {
        m_model->requestFileUploadPort(m_uploadData.size(), QStringLiteral("dvk_recording"),
                                       std::move(onPort));
    }
}

std::function<void(int, const QString&)> DvkWavTransfer::makeUploadPortCallback(
    quint64 generation, quint64 requestId)
{
    QPointer<DvkWavTransfer> transfer(this);
    return [transfer, generation, requestId](int code, const QString& body) {
        if (!transfer) {
            return;
        }
        transfer->handleUploadPortReceived(generation, requestId, code, body);
    };
}

void DvkWavTransfer::handleUploadPortReceived(quint64 generation, quint64 requestId,
                                              int code, const QString& body)
{
    if (!isCurrent(generation) || requestId == 0 || m_portRequestId != requestId) return;
    m_portRequestId = 0;

    if (code != 0) {
        reportRefusal(code);
        finish(false, QString("Radio rejected upload — %1")
                   .arg(DvkModel::dvkErrorString(static_cast<uint>(code))),
               false);
        return;
    }

    // The file server answers with its port, 42607 on fw 4.2.20; fall back to
    // that when the body carries none, as FirmwareUploader does.
    bool ok = false;
    int port = body.trimmed().toInt(&ok);
    if (!ok || port <= 0 || port > 65535) {
        qWarning() << "DvkWavTransfer: no usable port in file upload reply" << body.trimmed()
                   << "- using" << DEFAULT_UPLOAD_PORT;
        port = DEFAULT_UPLOAD_PORT;
    }

    openUploadSocket(generation, port);
}

void DvkWavTransfer::openUploadSocket(quint64 generation, int port)
{
    qDebug() << "DvkWavTransfer: connecting to upload port" << port << "for slot" << m_slotId;
    emit statusChanged(QString("Connecting to port %1…").arg(port));
    if (!isCurrent(generation)) {
        return;
    }

    m_uploadPort = port;
    m_client = new QTcpSocket(this);
    QPointer<DvkWavTransfer> transfer(this);
    QPointer<QTcpSocket> socket(m_client);
    connect(m_client, &QTcpSocket::connected, this, [transfer, generation, socket]() {
        if (transfer && socket) {
            transfer->handleUploadConnected(generation, socket);
        }
    });
    connect(m_client, &QTcpSocket::bytesWritten, this,
            [transfer, generation, socket](qint64 bytes) {
        if (transfer && socket) {
            transfer->handleUploadBytesWritten(generation, socket, bytes);
        }
    });
    connect(m_client, &QTcpSocket::errorOccurred, this,
            [transfer, generation, socket](QAbstractSocket::SocketError) {
        if (transfer && socket) {
            transfer->handleUploadError(generation, socket);
        }
    });

    // Small delay to let the radio set up its server (matches FirmwareUploader).
    // Bound to the socket it was queued for: without that, a cancel-then-restart
    // inside the window either connects the replacement's socket to this port or
    // -- when the replacement is a download -- dereferences a null m_client.
    QTimer::singleShot(200, this, makeUploadConnectCallback(
        generation, socket, [transfer, socket, port]() {
            if (transfer && socket && transfer->m_model) {
                socket->connectToHost(transfer->m_model->radioAddress(),
                                      static_cast<quint16>(port));
            }
        }));

    startConnectTimeout(generation);
}

std::function<void()> DvkWavTransfer::makeUploadConnectCallback(
    quint64 generation, QTcpSocket* socket, std::function<void()> connectAction)
{
    QPointer<DvkWavTransfer> transfer(this);
    QPointer<QTcpSocket> socketGuard(socket);
    return [transfer, generation, socketGuard, connectAction = std::move(connectAction)]() {
        if (!transfer || !socketGuard
            || !transfer->isCurrentSocket(generation, socketGuard)) {
            return;
        }
        connectAction();
    };
}

void DvkWavTransfer::handleUploadConnected(quint64 generation, QTcpSocket* socket)
{
    if (!isCurrentSocket(generation, socket)) return;
    m_uploadConnected = true;
    stopConnectTimeout();

    qDebug() << "DvkWavTransfer: connected, sending" << m_uploadData.size() << "bytes";
    emit statusChanged(QString("Uploading to slot %1…").arg(m_slotId));
    if (!isCurrentSocket(generation, socket)) {
        return;
    }

    sendNextChunk(generation, socket);
}

void DvkWavTransfer::sendNextChunk(quint64 generation, QTcpSocket* socket)
{
    if (!isCurrentSocket(generation, socket)) {
        return;
    }

    // Keep one accepted span in flight. bytesWritten() reports drained bytes,
    // while write() can have already accepted more data into QTcpSocket's
    // buffer. Advancing from m_bytesSent here would requeue that accepted tail
    // after a partial bytesWritten() notification.
    if (m_bytesSent != m_bytesAccepted) {
        return;
    }

    const qint64 remaining = m_uploadData.size() - m_bytesAccepted;
    if (remaining <= 0) {
        return;
    }

    const qint64 toSend = qMin(static_cast<qint64>(UPLOAD_CHUNK_SIZE), remaining);
    const qint64 accepted = socket->write(m_uploadData.constData() + m_bytesAccepted, toSend);
    // A synchronous socket error can finish the transfer inside write().
    if (!isCurrentSocket(generation, socket)) {
        return;
    }
    if (accepted < 0) {
        finish(false, "Upload write error: " + socket->errorString(), false);
        return;
    }
    if (accepted == 0) {
        finish(false, "Upload write made no progress", false);
        return;
    }

    m_bytesAccepted += accepted;
}

void DvkWavTransfer::handleUploadBytesWritten(quint64 generation, QTcpSocket* socket,
                                              qint64 bytes)
{
    if (!isCurrentSocket(generation, socket)) return;

    const qint64 pending = m_bytesAccepted - m_bytesSent;
    if (bytes <= 0 || bytes > pending) {
        finish(false, "Upload write acknowledgement invalid", false);
        return;
    }

    m_bytesSent += bytes;
    const int percent = static_cast<int>(m_bytesSent * 100 / m_uploadData.size());
    emit statusChanged(QString("Uploading to slot %1… %2%").arg(m_slotId).arg(percent));
    if (!isCurrentSocket(generation, socket)) {
        return;
    }

    if (m_bytesSent >= m_uploadData.size()) {
        qDebug() << "DvkWavTransfer: upload complete," << m_bytesSent << "bytes";
        socket->flush();
        socket->disconnectFromHost();
        finish(true, QString("Uploaded to slot %1 (%2 KB)")
                   .arg(m_slotId).arg(m_bytesSent / 1024), false);
        return;
    }

    sendNextChunk(generation, socket);
}

bool DvkWavTransfer::retryUploadOnDefaultPort(quint64 generation)
{
    if (!isCurrent(generation) || m_uploadFallbackTried || m_uploadConnected
        || m_uploadPort == 0 || m_uploadPort == DEFAULT_UPLOAD_PORT) {
        return false;
    }
    m_uploadFallbackTried = true;
    if (m_client) {
        m_client->disconnect(this);
        m_client->abort();
        m_client->deleteLater();
        m_client = nullptr;
    }
    stopConnectTimeout();
    openUploadSocket(generation, DEFAULT_UPLOAD_PORT);
    return true;
}

void DvkWavTransfer::handleUploadError(quint64 generation, QTcpSocket* socket)
{
    if (!isCurrentSocket(generation, socket)) return;

    // As FirmwareUploader: a port the radio named but would not connect gets
    // one retry on the file server's usual 42607. An error after connecting is
    // a failed upload, not a wrong port.
    if (retryUploadOnDefaultPort(generation)) {
        return;
    }

    const QString err = m_client ? m_client->errorString() : "Unknown error";
    finish(false, "Upload error: " + err, false);
}

// ── Shared cleanup ─────────────────────────────────────────────────────────

void DvkWavTransfer::cancel()
{
    if (!m_transferring || m_finished) {
        return;
    }
    m_cancelled = true;
    finish(false, "Transfer cancelled", m_direction == Download);
}

void DvkWavTransfer::finish(bool success, const QString& message, bool discardDownload)
{
    // Idempotent: the radio fires both errorOccurred() and disconnected() on a
    // clean close, and abort()/disconnectFromHost() during teardown can emit
    // further signals. Only the first call through here does anything.
    if (m_finished) {
        return;
    }
    m_finished = true;

    // Enter the terminal state and tear down BEFORE emitting, the same
    // ordering ProfileTransfer uses (#5617). Emitting first ran cleanup()
    // after the handler, so a replacement transfer started from finished()
    // was silently torn down by the operation that had just ended -- and
    // isTransferring() still read true inside the handler.
    cleanup(discardDownload);
    emit finished(success, message);
}

void DvkWavTransfer::cleanup(bool discardDownload)
{
    // Re-entrancy guard: abort()/deleteLater() below can synchronously deliver
    // queued socket signals (disconnected/errorOccurred) that route back here.
    if (m_cleaningUp) {
        return;
    }
    m_cleaningUp = true;

    if (m_timeout) {
        m_timeout->stop();
    }
    // Retire the operation identity before tearing down: a reply or timer
    // already queued for this transfer must not match a replacement started
    // from the finished() handler below.
    invalidateOperation();
    // Only an upload that reached the file server leaves it busy; a refused
    // `dvk upload` or a port that never connected must not mask the next try.
    if (m_direction == Upload && m_uploadConnected) {
        m_sinceUpload.start();
    }
    m_transferring = false;
    m_direction = None;
    if (m_dvk) {
        m_dvk->setTransferActive(false);
    }

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
        if (discardDownload) {
            m_file->cancelWriting();
        }
        m_file->deleteLater();
        m_file = nullptr;
    }

    m_uploadData.clear();
    m_bytesReceived = 0;
    m_bytesSent = 0;
    m_bytesAccepted = 0;

    m_cleaningUp = false;
}

} // namespace AetherSDR
