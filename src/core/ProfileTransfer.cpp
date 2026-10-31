#include "ProfileTransfer.h"
#include "core/backends/FileTransferOperation.h"

#include "LogManager.h"
#include "ZipArchive.h"
#include "../models/MemoryEntry.h"
#include "../models/RadioModel.h"
#include "../models/TransmitModel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QMap>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QSet>

namespace AetherSDR {

namespace {

QByteArray prepareDatabaseImportPayload(const QByteArray& ssdrCfg, bool* repackaged,
                                        QString* error)
{
    *repackaged = false;
    const QMap<QString, QByteArray> entries = readZipEntries(ssdrCfg, error);
    if (entries.isEmpty())
        return {};

    const QByteArray flexPayload = entries.value(QStringLiteral("flex_payload"));
    const QByteArray metaData = entries.value(QStringLiteral("meta_data"));
    const QByteArray metaSubset = entries.value(QStringLiteral("meta_subset"));

    if (flexPayload.isEmpty()) {
        *error = QStringLiteral("The .ssdr_cfg package does not contain flex_payload.");
        return {};
    }
    if (!metaSubset.isEmpty())
        return ssdrCfg;
    if (metaData.isEmpty()) {
        *error = QStringLiteral("The .ssdr_cfg package does not contain meta_data.");
        return {};
    }

    *repackaged = true;
    return writeStoredZip({
        {QStringLiteral("meta_subset"), metaData},
        {QStringLiteral("flex_payload"), flexPayload},
    });
}

} // namespace

ProfileTransfer::ProfileTransfer(RadioModel* model, QObject* parent)
    : QObject(parent), m_model(model)
{
    if (model) {
        connect(model, &RadioModel::connectionStateChanged, this, [this] {
            if (m_busy && !m_model->fileTransferCurrent(m_transfer)) {
                fail(QStringLiteral("Original radio session ended during profile transfer."));
            }
        });
    }
    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this] {
        if (isCurrent(m_commandTimeoutGeneration, m_commandTimeoutPhase)) {
            handleTimeout();
        }
    });

    m_idleTimer = new QTimer(this);
    m_idleTimer->setSingleShot(true);
    connect(m_idleTimer, &QTimer::timeout, this, [this] {
        if (isCurrent(m_idleTimeoutGeneration, m_idleTimeoutPhase)) {
            fail(QStringLiteral("Transfer timed out while waiting for data."));
        }
    });

    m_overallTimer = new QTimer(this);
    m_overallTimer->setSingleShot(true);
    connect(m_overallTimer, &QTimer::timeout, this, [this] {
        // The overall budget belongs to the operation begin() armed it for, so
        // it is generation-bound like every other deferred path in this class.
        if (m_busy && m_operationGeneration == m_overallTimeoutGeneration) {
            fail(QStringLiteral("Profile database transfer timed out."));
        }
    });
}

ProfileTransfer::~ProfileTransfer()
{
    if (m_busy)
        cleanup();
}

void ProfileTransfer::exportDatabase(const ExportSelection& selection, const QString& destinationPath,
                                     std::optional<RadioControlSession> session)
{
    QString error;
    if (!validateCommonPreconditions(Operation::ExportDatabase, &error)) {
        emit failed(Operation::ExportDatabase, error);
        return;
    }
    if (!validateExportDestination(destinationPath, &error)) {
        emit failed(Operation::ExportDatabase, error);
        return;
    }

    ExportSelection expanded = expandSelection(selection);
    if (!validateExportSelection(expanded, &error)) {
        emit failed(Operation::ExportDatabase, error);
        return;
    }

    const QByteArray metaSubset = buildMetaSubset(expanded);
    if (metaSubset.isEmpty()) {
        emit failed(Operation::ExportDatabase,
                    QStringLiteral("Profile export selection produced an empty SmartSDR meta_subset file."));
        return;
    }

    m_transfer = m_model->beginFileTransfer(FileTransferPurpose::ProfileMetadata, this,
        session.value_or(m_model->controlSession()), kOverallTimeoutMs);
    if (!m_transfer) {
        emit failed(Operation::ExportDatabase, QStringLiteral("Transfer unavailable, busy, or original session ended."));
        return;
    }
    const QPointer<ProfileTransfer> alive(this);
    const quint64 generation = begin(Operation::ExportDatabase, Phase::UploadMetaSubset);
    if (!alive || !isCurrent(generation, Phase::UploadMetaSubset)) {
        return;
    }
    m_path = destinationPath;

    qCInfo(lcProtocol).noquote()
        << "ProfileTransfer: export requested"
        << QStringLiteral("meta_subset_bytes=%1").arg(metaSubset.size());
    emit progress(QStringLiteral("Sending export selection to radio..."), 0, metaSubset.size());
    if (!alive) { return; }
    if (!isCurrent(generation, Phase::UploadMetaSubset)) {
        return;
    }
    requestUploadPort(metaSubset, FileTransferPurpose::ProfileMetadata);
}

void ProfileTransfer::importDatabase(const QString& ssdrCfgPath, std::optional<RadioControlSession> session)
{
    QString error;
    if (!validateCommonPreconditions(Operation::ImportDatabase, &error)) {
        emit failed(Operation::ImportDatabase, error);
        return;
    }
    if (!validateImportFile(ssdrCfgPath, &error)) {
        emit failed(Operation::ImportDatabase, error);
        return;
    }

    QFile file(ssdrCfgPath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit failed(Operation::ImportDatabase,
                    QStringLiteral("Cannot open import package: %1").arg(file.errorString()));
        return;
    }
    const QByteArray payload = file.read(kMaxImportSize + 1);
    if (payload.size() != file.size()) {
        emit failed(Operation::ImportDatabase,
                    QStringLiteral("Could not read the complete .ssdr_cfg package."));
        return;
    }
    bool repackaged = false;
    const QByteArray importPayload = prepareDatabaseImportPayload(payload, &repackaged, &error);
    if (importPayload.isEmpty() || importPayload.size() > kMaxImportSize) {
        emit failed(Operation::ImportDatabase, error.isEmpty()
            ? QStringLiteral("Prepared import exceeds the 250 MB transfer limit.") : error);
        return;
    }

    m_transfer = m_model->beginFileTransfer(FileTransferPurpose::ProfileImport, this,
        session.value_or(m_model->controlSession()), kOverallTimeoutMs);
    if (!m_transfer) {
        emit failed(Operation::ImportDatabase, QStringLiteral("Transfer unavailable, busy, or original session ended."));
        return;
    }
    const QPointer<ProfileTransfer> alive(this);
    const quint64 generation = begin(Operation::ImportDatabase, Phase::UploadImport);
    if (!alive || !isCurrent(generation, Phase::UploadImport)) {
        return;
    }
    m_path = ssdrCfgPath;

    qCInfo(lcProtocol).noquote()
        << "ProfileTransfer: import requested"
        << QStringLiteral("package_bytes=%1").arg(payload.size())
        << QStringLiteral("upload_bytes=%1").arg(importPayload.size())
        << QStringLiteral("repackaged=%1").arg(repackaged ? 1 : 0);
    emit progress(QStringLiteral("Requesting database import upload port..."), 0, importPayload.size());
    if (!alive) { return; }
    if (!isCurrent(generation, Phase::UploadImport)) {
        return;
    }
    requestUploadPort(importPayload, FileTransferPurpose::ProfileImport);
}

void ProfileTransfer::cancel()
{
    if (!m_busy)
        return;

    m_cancelled = true;
    const Operation op = m_operation;
    qCInfo(lcProtocol) << "ProfileTransfer: cancelled";
    cleanup();
    emit failed(op, QStringLiteral("Profile transfer cancelled."));
}

bool ProfileTransfer::isCurrent(quint64 generation, Phase expectedPhase) const
{
    return m_busy && !m_cancelled && m_operationGeneration == generation && m_phase == expectedPhase;
}

bool ProfileTransfer::isCurrentSocket(quint64 generation, Phase expectedPhase,
                                      const QTcpSocket* expectedSocket) const
{
    return isCurrent(generation, expectedPhase) && m_socket == expectedSocket;
}

quint64 ProfileTransfer::nextAsyncId()
{
    ++m_nextAsyncId;
    if (m_nextAsyncId == 0) {
        ++m_nextAsyncId;
    }
    return m_nextAsyncId;
}

void ProfileTransfer::invalidateOperation()
{
    ++m_operationGeneration;
    if (m_operationGeneration == 0) {
        ++m_operationGeneration;
    }
    m_uploadPortRequestId = 0;
    m_downloadPortRequestId = 0;
    m_commandTimeoutGeneration = 0;
    m_idleTimeoutGeneration = 0;
    m_commandTimeoutPhase = Phase::Idle;
    m_idleTimeoutPhase = Phase::Idle;
}

void ProfileTransfer::startCommandTimeout(int timeoutMs, Phase expectedPhase)
{
    m_commandTimeoutGeneration = m_operationGeneration;
    m_commandTimeoutPhase = expectedPhase;
    m_timeout->start(timeoutMs);
}

void ProfileTransfer::stopCommandTimeout()
{
    m_timeout->stop();
    m_commandTimeoutGeneration = 0;
    m_commandTimeoutPhase = Phase::Idle;
}

void ProfileTransfer::startIdleTimeout(Phase expectedPhase)
{
    m_idleTimeoutGeneration = m_operationGeneration;
    m_idleTimeoutPhase = expectedPhase;
    m_idleTimer->start(kIdleTimeoutMs);
}

void ProfileTransfer::stopIdleTimeout()
{
    m_idleTimer->stop();
    m_idleTimeoutGeneration = 0;
    m_idleTimeoutPhase = Phase::Idle;
}

quint64 ProfileTransfer::begin(Operation operation, Phase phase)
{
    invalidateOperation();
    const quint64 generation = m_operationGeneration;
    m_operation = operation;
    m_phase = phase;
    m_busy = true;
    m_cancelled = false;
    m_usedFallbackPort = false;
    m_uploadConnected = false;
    m_downloadFinalized = false;
    m_importCompletionScheduled = false;
    m_bytesDone = 0;
    m_bytesQueued = 0;
    m_bytesTotal = 0;
    m_uploadPort = 0;
    m_overallTimeoutGeneration = generation;
    m_overallTimer->start(kOverallTimeoutMs);
    emit started(operation);
    return generation;
}

void ProfileTransfer::fail(const QString& error)
{
    if (!m_busy && !m_cancelled)
        return;

    const Operation op = m_operation;
    qCWarning(lcProtocol) << "ProfileTransfer failed:" << error;
    cleanup();
    emit failed(op, error);
}

void ProfileTransfer::finish(QString path)
{
    const Operation op = m_operation;
    cleanup();
    emit finished(op, path);
}

void ProfileTransfer::cleanup()
{
    if (m_cleaningUp) { return; }
    m_cleaningUp = true;
    if (m_model) { m_model->finishFileTransfer(m_transfer, m_uploadConnected ? 3000 : 0); }
    else if (m_transfer) { m_transfer->cancel(); }
    m_transfer.reset();
    // Enter the terminal state before tearing down QObjects. Their methods can
    // emit synchronously, and no callback may start a second terminal outcome.
    m_busy = false;
    m_cancelled = false;
    invalidateOperation();

    stopCommandTimeout();
    stopIdleTimeout();
    m_overallTimer->stop();
    m_overallTimeoutGeneration = 0;
    QObject::disconnect(m_importingChangedConnection);
    m_importingChangedConnection = {};

    destroySocket(true);
    destroyServer();
    if (m_saveFile) {
        m_saveFile->close();
        m_saveFile->deleteLater();
        m_saveFile = nullptr;
    }

    m_uploadPayload.clear();
    m_path.clear();
    m_bytesDone = 0;
    m_bytesQueued = 0;
    m_bytesTotal = 0;
    m_uploadPort = 0;
    m_usedFallbackPort = false;
    m_downloadFinalized = false;
    m_importCompletionScheduled = false;
    m_phase = Phase::Idle;
    m_cleaningUp = false;
}

void ProfileTransfer::destroySocket(bool abortConnection)
{
    if (!m_socket) {
        return;
    }

    QTcpSocket* socket = m_socket;
    m_socket = nullptr;
    QObject::disconnect(socket, nullptr, this, nullptr);
    if (abortConnection) {
        socket->abort();
    } else {
        socket->disconnectFromHost();
    }
    socket->deleteLater();
}

void ProfileTransfer::destroyServer()
{
    if (!m_server) {
        return;
    }

    // Same detach-then-act ordering as destroySocket(). close() does not emit
    // synchronously, but a newConnection already queued before cleanup would
    // otherwise reach handleDownloadConnection() with m_server null; its
    // m_server != server test stops it dereferencing that. Severing the handler
    // makes the teardown safe by construction rather than by guard ordering.
    QTcpServer* server = m_server;
    m_server = nullptr;
    QObject::disconnect(server, nullptr, this, nullptr);
    server->close();
    server->deleteLater();
}

ExportSelection ProfileTransfer::expandSelection(ExportSelection selection) const
{
    if (!m_model)
        return selection;

    if (selection.globalProfiles && selection.globalProfileNames.isEmpty())
        selection.globalProfileNames = m_model->globalProfiles();
    if (selection.txProfiles && selection.txProfileNames.isEmpty())
        selection.txProfileNames = m_model->transmitModel().profileList();
    if (selection.micProfiles && selection.micProfileNames.isEmpty())
        selection.micProfileNames = m_model->transmitModel().micProfileList();

    if (selection.memories && selection.memoryGroups.isEmpty()) {
        QSet<QString> seen;
        for (auto it = m_model->memories().cbegin(); it != m_model->memories().cend(); ++it) {
            const MemoryEntry& memory = it.value();
            const QString key = memory.owner + QChar(0x1f) + memory.group;
            if (seen.contains(key))
                continue;
            seen.insert(key);
            selection.memoryGroups.append({memory.owner, memory.group});
        }
    }

    return selection;
}

bool ProfileTransfer::validateCommonPreconditions(Operation operation, QString* error) const
{
    if (isBusy()) {
        *error = QStringLiteral("A profile transfer is already in progress.");
        return false;
    }
    if (!m_model || !m_model->isConnected()) {
        *error = QStringLiteral("Connect to a FlexRadio before importing or exporting profiles.");
        return false;
    }
    if (m_model->isWan()) {
        *error = operation == Operation::ExportDatabase
            ? QStringLiteral("SmartLink/WAN profile export is not supported because the radio must connect back to this client for the database download.")
            : QStringLiteral("SmartLink/WAN profile import is not enabled in this build; use a direct LAN connection for database transfers.");
        return false;
    }
    if (m_model->isProfileTransferBlocked()) {
        *error = QStringLiteral("Stop MOX, TUNE, PTT, or active transmit before importing or exporting radio profiles.");
        return false;
    }
    if (m_model->radioAddress().isNull()) {
        *error = QStringLiteral("The connected radio address is unavailable.");
        return false;
    }
    return true;
}

bool ProfileTransfer::validateExportSelection(const ExportSelection& selection, QString* error) const
{
    if (!selection.anySelected()) {
        *error = QStringLiteral("Select at least one radio database category to export.");
        return false;
    }
    return true;
}

bool ProfileTransfer::validateExportDestination(const QString& path, QString* error) const
{
    const QFileInfo info(path);
    if (path.trimmed().isEmpty()) {
        *error = QStringLiteral("Choose a destination .ssdr_cfg file.");
        return false;
    }
    if (info.suffix().compare(QStringLiteral("ssdr_cfg"), Qt::CaseInsensitive) != 0) {
        *error = QStringLiteral("SmartSDR profile backups must use the .ssdr_cfg extension.");
        return false;
    }
    const QDir dir = info.absoluteDir();
    if (!dir.exists() && !QDir().mkpath(dir.absolutePath())) {
        *error = QStringLiteral("Cannot create the export directory.");
        return false;
    }
    return true;
}

bool ProfileTransfer::validateImportFile(const QString& path, QString* error) const
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        *error = QStringLiteral("Choose a readable .ssdr_cfg package to import.");
        return false;
    }
    if (info.suffix().compare(QStringLiteral("ssdr_cfg"), Qt::CaseInsensitive) != 0) {
        *error = QStringLiteral("Only SmartSDR .ssdr_cfg packages can be imported.");
        return false;
    }
    if (info.size() <= 0) {
        *error = QStringLiteral("The selected .ssdr_cfg package is empty.");
        return false;
    }
    if (info.size() > kMaxImportSize) {
        *error = QStringLiteral("The selected .ssdr_cfg package is larger than the safety limit.");
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot read the import package: %1").arg(file.errorString());
        return false;
    }
    return true;
}

void ProfileTransfer::requestUploadPort(const QByteArray& payload, FileTransferPurpose purpose)
{
    if (!m_model) {
        // The QPointer nulled between begin() and here. Report it: begin() has
        // already armed the 180 s overall budget, and a silent return would
        // spend all of it before showing a generic timeout.
        fail(QStringLiteral("The radio connection was lost before the transfer could start."));
        return;
    }
    // Callers re-check their own generation before calling in, so this is the
    // plain still-running test it looks like -- not a generation comparison.
    if (!m_busy || m_cancelled) {
        return;
    }
    m_uploadPayload = payload;
    m_bytesDone = 0;
    m_bytesQueued = 0;
    m_bytesTotal = payload.size();
    m_usedFallbackPort = false;

    const quint64 generation = m_operationGeneration;
    const Phase expectedPhase = m_phase;
    const quint64 requestId = nextAsyncId();
    m_uploadPortRequestId = requestId;
    startCommandTimeout(kCommandTimeoutMs, expectedPhase);
    m_model->requestFileTransfer(m_transfer, {purpose, payload.size(), {}, 0},
        makeUploadPortCallback(generation, expectedPhase, requestId));
}

FileTransferCompletion ProfileTransfer::makeUploadPortCallback(
    quint64 generation, Phase expectedPhase, quint64 requestId)
{
    QPointer<ProfileTransfer> transfer(this);
    return [transfer, generation, expectedPhase, requestId](const FileTransferReply& reply) {
        if (!transfer) {
            return;
        }
        transfer->handleUploadPortReceived(generation, expectedPhase, requestId, reply);
    };
}

void ProfileTransfer::handleUploadPortReceived(quint64 generation, Phase expectedPhase,
                                               quint64 requestId, const FileTransferReply& reply)
{
    if (!isCurrent(generation, expectedPhase) || requestId == 0
        || m_uploadPortRequestId != requestId) {
        return;
    }

    m_uploadPortRequestId = 0;
    stopCommandTimeout();
    if (reply.result.kind != RadioControlReply::Kind::Accepted) {
        fail(reply.result.code == -1 ? reply.result.message :
             QStringLiteral("Radio rejected the file upload request (error 0x%1).")
                 .arg(reply.result.code, 0, 16));
        return;
    }

    const quint16 port = reply.port;
    connectUploadSocket(port);
}

void ProfileTransfer::connectUploadSocket(quint16 port)
{
    const QPointer<ProfileTransfer> alive(this);
    destroySocket(true);
    // A fallback has superseded the command reply that selected the old port.
    // The same operation can still receive that reply after the replacement.
    m_uploadPortRequestId = 0;

    m_uploadPort = port;
    m_socket = new QTcpSocket(this);
    QPointer<QTcpSocket> socket(m_socket);
    const quint64 generation = m_operationGeneration;
    const Phase expectedPhase = m_phase;
    QPointer<ProfileTransfer> transfer(this);
    connect(m_socket, &QTcpSocket::connected, this, [transfer, generation, expectedPhase, socket] {
        if (transfer && socket) {
            transfer->handleUploadConnected(generation, expectedPhase, socket);
        }
    });
    connect(m_socket, &QTcpSocket::bytesWritten, this,
            [transfer, generation, expectedPhase, socket](qint64 bytes) {
        if (transfer && socket) {
            transfer->handleUploadBytesWritten(generation, expectedPhase, socket, bytes);
        }
    });
    connect(m_socket, &QTcpSocket::disconnected, this, [transfer, generation, expectedPhase, socket] {
        if (transfer && socket) {
            transfer->handleUploadDisconnected(generation, expectedPhase, socket);
        }
    });
    connect(m_socket, &QTcpSocket::errorOccurred, this,
            [transfer, generation, expectedPhase, socket](QAbstractSocket::SocketError) {
        if (transfer && socket) {
            transfer->handleUploadError(generation, expectedPhase, socket);
        }
    });

    emit progress(QStringLiteral("Connecting to radio upload port %1...").arg(port),
                  m_bytesDone, m_bytesTotal);
    if (!alive) { return; }
    if (!isCurrentSocket(generation, expectedPhase, socket)) {
        return;
    }
    qCInfo(lcProtocol) << "ProfileTransfer: connecting to upload port" << port;

    QTimer::singleShot(200, this, makeUploadConnectCallback(
        generation, expectedPhase, socket, [transfer, socket, port] {
            if (transfer && socket && transfer->m_model
                && transfer->m_model->fileTransferCurrent(transfer->m_transfer)) {
                socket->connectToHost(transfer->m_transfer->peer(), port);
            }
        }));
    startCommandTimeout(kConnectTimeoutMs, expectedPhase);
}

std::function<void()> ProfileTransfer::makeUploadConnectCallback(quint64 generation,
                                                                  Phase expectedPhase,
                                                                  QTcpSocket* socket,
                                                                  std::function<void()> connectAction)
{
    QPointer<ProfileTransfer> transfer(this);
    QPointer<QTcpSocket> socketGuard(socket);
    return [transfer, generation, expectedPhase, socketGuard, connectAction = std::move(connectAction)] {
        if (!transfer || !socketGuard
            || !transfer->isCurrentSocket(generation, expectedPhase, socketGuard)) {
            return;
        }
        connectAction();
    };
}

void ProfileTransfer::tryFallbackUploadPort()
{
    const QPointer<ProfileTransfer> alive(this);
    if (m_usedFallbackPort) {
        fail(QStringLiteral("Cannot connect to the radio transfer port."));
        return;
    }
    m_usedFallbackPort = true;
    const quint64 generation = m_operationGeneration;
    const Phase expectedPhase = m_phase;
    emit progress(QStringLiteral("Trying fallback transfer port %1...").arg(kFallbackTransferPort),
                  m_bytesDone, m_bytesTotal);
    if (!alive) { return; }
    if (!isCurrent(generation, expectedPhase)) {
        return;
    }
    connectUploadSocket(kFallbackTransferPort);
}

void ProfileTransfer::handleUploadConnected(quint64 generation, Phase expectedPhase,
                                            QTcpSocket* socket)
{
    const QPointer<ProfileTransfer> alive(this);
    if (!isCurrentSocket(generation, expectedPhase, socket)) {
        return;
    }

    m_uploadConnected = true;
    stopCommandTimeout();
    resetIdleTimer();
    emit progress(m_phase == Phase::UploadMetaSubset
                      ? QStringLiteral("Uploading export selection...")
                      : QStringLiteral("Uploading SmartSDR database package..."),
                  m_bytesDone, m_bytesTotal);
    if (!alive) { return; }
    if (!isCurrentSocket(generation, expectedPhase, socket)) {
        return;
    }
    sendNextUploadChunk(generation, expectedPhase, socket);
}

void ProfileTransfer::sendNextUploadChunk(quint64 generation, Phase expectedPhase,
                                          QTcpSocket* socket)
{
    if (!isCurrentSocket(generation, expectedPhase, socket))
        return;

    if ((m_transfer && !m_transfer->current())
        || (m_model && !m_model->fileTransferCurrent(m_transfer))) {
        fail(QStringLiteral("Original transfer ended."));
        return;
    }
    const qint64 remaining = m_uploadPayload.size() - m_bytesQueued;
    if (remaining <= 0)
        return;

    const qint64 toSend = qMin<qint64>(kUploadChunkSize, remaining);
    const QPointer<ProfileTransfer> alive(this);
    const qint64 written = socket->write(m_uploadPayload.constData() + m_bytesQueued, toSend);
    if (!alive || !isCurrentSocket(generation, expectedPhase, socket)) { return; }
    if (written <= 0 || written > toSend) {
        fail(QStringLiteral("Upload failed: invalid socket byte accounting."));
        return;
    }
    m_bytesQueued += written;
}

void ProfileTransfer::handleUploadBytesWritten(quint64 generation, Phase expectedPhase,
                                               QTcpSocket* socket, qint64 bytes)
{
    const QPointer<ProfileTransfer> alive(this);
    if (!isCurrentSocket(generation, expectedPhase, socket)) {
        return;
    }

    if (bytes <= 0 || bytes > m_bytesQueued - m_bytesDone) {
        fail(QStringLiteral("Upload failed: invalid socket byte accounting."));
        return;
    }
    resetIdleTimer();
    m_bytesDone += bytes;
    emit progress(m_phase == Phase::UploadMetaSubset
                      ? QStringLiteral("Uploading export selection...")
                      : QStringLiteral("Uploading SmartSDR database package..."),
                  m_bytesDone, m_bytesTotal);
    if (!alive) { return; }
    if (!isCurrentSocket(generation, expectedPhase, socket)) {
        return;
    }

    if (m_bytesDone >= m_bytesTotal) {
        qCInfo(lcProtocol) << "ProfileTransfer: upload complete" << m_bytesDone << "bytes";
        stopIdleTimeout();
        m_socket->flush();
        m_socket->disconnectFromHost();
        return;
    }

    if (m_bytesQueued < m_bytesTotal)
        sendNextUploadChunk(generation, expectedPhase, socket);
}

void ProfileTransfer::handleUploadDisconnected(quint64 generation, Phase expectedPhase,
                                                QTcpSocket* socket)
{
    const QPointer<ProfileTransfer> alive(this);
    if (!isCurrentSocket(generation, expectedPhase, socket)) {
        return;
    }

    if (m_bytesDone < m_bytesTotal) {
        fail(QStringLiteral("Radio closed the upload connection before the transfer completed."));
        return;
    }

    destroySocket(false);
    m_uploadPayload.clear();

    if (m_phase == Phase::UploadMetaSubset) {
        emit progress(QStringLiteral("Waiting for the radio to prepare the database package..."), 0, 0);
        if (!alive) { return; }
        if (!isCurrent(generation, expectedPhase)) { return; }
        QPointer<ProfileTransfer> transfer(this);
        QTimer::singleShot(kMetaSubsetSettleMs, this, makeMetadataSettleCallback(
            generation, expectedPhase, [transfer] {
                if (transfer) {
                    transfer->requestPackageDownload();
                }
            }));
        return;
    }

    if (m_phase == Phase::UploadImport) {
        waitForImportCompletion();
        return;
    }
}

std::function<void()> ProfileTransfer::makeMetadataSettleCallback(quint64 generation,
                                                                   Phase expectedPhase,
                                                                   std::function<void()> settleAction)
{
    QPointer<ProfileTransfer> transfer(this);
    return [transfer, generation, expectedPhase, settleAction = std::move(settleAction)] {
        if (!transfer || !transfer->isCurrent(generation, expectedPhase)) {
            return;
        }
        settleAction();
    };
}

void ProfileTransfer::handleUploadError(quint64 generation, Phase expectedPhase,
                                        QTcpSocket* socket)
{
    if (!isCurrentSocket(generation, expectedPhase, socket)) {
        return;
    }

    if (!m_uploadConnected && m_bytesQueued == 0 && m_uploadPort != kFallbackTransferPort) {
        tryFallbackUploadPort();
        return;
    }

    const QString err = m_socket ? m_socket->errorString() : QStringLiteral("Unknown socket error");
    fail(QStringLiteral("Upload failed: %1").arg(err));
}

void ProfileTransfer::requestPackageDownload()
{
    const QPointer<ProfileTransfer> alive(this);
    if (!m_busy)
        return;
    if (!m_model) {
        fail(QStringLiteral("The radio connection was lost before the database package could be requested."));
        return;
    }

    const quint64 generation = m_operationGeneration;
    m_phase = Phase::DownloadPackage;
    m_bytesDone = 0;
    m_bytesTotal = 0;
    startCommandTimeout(kCommandTimeoutMs, Phase::DownloadPackage);
    emit progress(QStringLiteral("Requesting SmartSDR database package..."), 0, 0);
    if (!alive) { return; }
    if (!isCurrent(generation, Phase::DownloadPackage)) {
        return;
    }

    const quint64 requestId = nextAsyncId();
    m_downloadPortRequestId = requestId;
    m_model->requestFileTransfer(m_transfer, {FileTransferPurpose::ProfileExport, 0, {}, 0},
        makeDownloadPortCallback(generation, Phase::DownloadPackage, requestId));
}

FileTransferCompletion ProfileTransfer::makeDownloadPortCallback(
    quint64 generation, Phase expectedPhase, quint64 requestId)
{
    QPointer<ProfileTransfer> transfer(this);
    return [transfer, generation, expectedPhase, requestId](const FileTransferReply& reply) {
        if (!transfer) {
            return;
        }
        transfer->handleDownloadPortReceived(generation, expectedPhase, requestId, reply);
    };
}

void ProfileTransfer::handleDownloadPortReceived(quint64 generation, Phase expectedPhase,
                                                 quint64 requestId, const FileTransferReply& reply)
{
    if (!isCurrent(generation, expectedPhase) || requestId == 0
        || m_downloadPortRequestId != requestId) {
        return;
    }

    m_downloadPortRequestId = 0;
    stopCommandTimeout();
    if (reply.result.kind != RadioControlReply::Kind::Accepted) {
        fail(reply.result.code == -1 ? reply.result.message :
            QStringLiteral("Radio rejected the database download request (error 0x%1).")
                .arg(reply.result.code, 0, 16));
        return;
    }

    const quint16 port = reply.port;
    startDownloadServer(port);
}

void ProfileTransfer::startDownloadServer(quint16 port)
{
    QString error;
    if (!validateExportDestination(m_path, &error)) {
        fail(error);
        return;
    }

    m_saveFile = new QTemporaryFile(QFileInfo(m_path).absolutePath()
        + QStringLiteral("/.aethersdr-profile-XXXXXX"), this);
    if (!m_saveFile->open()) {
        fail(QStringLiteral("Cannot create export file: %1").arg(m_saveFile->errorString()));
        return;
    }

    m_server = new QTcpServer(this);
    QPointer<QTcpServer> server(m_server);
    const quint64 generation = m_operationGeneration;
    QPointer<ProfileTransfer> transfer(this);
    connect(m_server, &QTcpServer::newConnection, this, [transfer, generation, server] {
        if (transfer && server) {
            transfer->handleDownloadConnection(generation, Phase::DownloadPackage, server);
        }
    });
    if (!m_server->listen(QHostAddress::Any, port)) {
        fail(QStringLiteral("Cannot listen for the radio database download on port %1: %2")
                 .arg(port)
                 .arg(m_server->errorString()));
        return;
    }

    qCInfo(lcProtocol) << "ProfileTransfer: listening for database package on port" << port;
    startCommandTimeout(kConnectTimeoutMs, Phase::DownloadPackage);
    emit progress(QStringLiteral("Waiting for radio database download on port %1...").arg(port), 0, 0);
}

void ProfileTransfer::handleDownloadConnection(quint64 generation, Phase expectedPhase,
                                               QTcpServer* server)
{
    if (!isCurrent(generation, expectedPhase) || m_server != server || m_socket) {
        return;
    }

    QTcpSocket* pending = m_server->nextPendingConnection();
    if (!pending) { return; }
    if (!m_transfer || !transferPeerMatches(pending->peerAddress(), m_transfer->peer())) {
        pending->abort();
        pending->deleteLater();
        return;
    }
    stopCommandTimeout();
    m_socket = pending;
    m_server->close();

    QPointer<QTcpSocket> socket(m_socket);
    QPointer<ProfileTransfer> transfer(this);
    connect(m_socket, &QTcpSocket::readyRead, this, [transfer, generation, expectedPhase, socket] {
        if (transfer && socket) {
            transfer->handleDownloadReadyRead(generation, expectedPhase, socket);
        }
    });
    connect(m_socket, &QTcpSocket::disconnected, this,
            [transfer, generation, expectedPhase, socket] {
        if (transfer && socket) {
            transfer->handleDownloadDisconnected(generation, expectedPhase, socket);
        }
    });
    connect(m_socket, &QTcpSocket::errorOccurred, this,
            [transfer, generation, expectedPhase, socket](QAbstractSocket::SocketError) {
        if (transfer && socket) {
            transfer->handleDownloadError(generation, expectedPhase, socket);
        }
    });

    resetIdleTimer();
    emit progress(QStringLiteral("Receiving SmartSDR database package..."), 0, 0);
}

void ProfileTransfer::handleDownloadReadyRead(quint64 generation, Phase expectedPhase,
                                              QTcpSocket* socket)
{
    if (!isCurrentSocket(generation, expectedPhase, socket) || !m_saveFile) {
        return;
    }
    if ((m_transfer && !m_transfer->current())
        || (m_model && !m_model->fileTransferCurrent(m_transfer))) {
        fail(QStringLiteral("Original transfer ended."));
        return;
    }

    resetIdleTimer();
    if (m_socket->bytesAvailable() > kMaxImportSize - m_bytesDone) {
        fail(QStringLiteral("Profile export exceeds the 250 MiB package limit."));
        return;
    }
    const QByteArray chunk = m_socket->read(kMaxImportSize - m_bytesDone + 1);
    if (chunk.isEmpty())
        return;

    const qint64 written = m_saveFile->write(chunk);
    if (written != chunk.size()) {
        fail(QStringLiteral("Could not write the complete export package: %1")
                 .arg(m_saveFile->errorString()));
        return;
    }
    m_bytesDone += written;
    emit progress(QStringLiteral("Receiving SmartSDR database package..."), m_bytesDone, 0);
}

void ProfileTransfer::handleDownloadDisconnected(quint64 generation, Phase expectedPhase,
                                                 QTcpSocket* socket)
{
    const QPointer<ProfileTransfer> alive(this);
    if (!isCurrentSocket(generation, expectedPhase, socket) || m_downloadFinalized) {
        return;
    }
    if ((m_transfer && !m_transfer->current())
        || (m_model && !m_model->fileTransferCurrent(m_transfer))) {
        fail(QStringLiteral("Original transfer ended."));
        return;
    }

    m_downloadFinalized = true;
    stopIdleTimeout();
    if (m_bytesDone <= 0) {
        fail(QStringLiteral("Radio sent an empty database package."));
        return;
    }

    QString error;
    if (!commitExportFile(&error)) {
        fail(error);
        return;
    }

    qCInfo(lcProtocol) << "ProfileTransfer: export complete" << m_bytesDone << "bytes";
    emit progress(QStringLiteral("Export complete."), m_bytesDone, m_bytesDone);
    if (!alive) { return; }
    if (!isCurrentSocket(generation, expectedPhase, socket)) {
        return;
    }
    const QString finishedPath = m_path;
    finish(finishedPath);
}

void ProfileTransfer::handleDownloadError(quint64 generation, Phase expectedPhase,
                                          QTcpSocket* socket)
{
    if (!isCurrentSocket(generation, expectedPhase, socket)) {
        return;
    }

    if (m_socket && m_socket->error() == QAbstractSocket::RemoteHostClosedError && m_bytesDone > 0) {
        handleDownloadDisconnected(generation, expectedPhase, socket);
        return;
    }

    const QString err = m_socket ? m_socket->errorString() : QStringLiteral("Unknown socket error");
    fail(QStringLiteral("Database download failed: %1").arg(err));
}

void ProfileTransfer::waitForImportCompletion()
{
    const QPointer<ProfileTransfer> alive(this);
    const quint64 generation = m_operationGeneration;
    m_phase = Phase::WaitingForImport;
    stopIdleTimeout();
    emit progress(QStringLiteral("Import sent; waiting for radio to apply the database..."),
                  m_bytesDone, m_bytesTotal);
    if (!alive) { return; }

    if (!isCurrent(generation, Phase::WaitingForImport)) {
        return;
    }

    if (m_model && m_model->profileDatabaseImporting()) {
        startCommandTimeout(kOverallTimeoutMs / 2, Phase::WaitingForImport);
        QObject::disconnect(m_importingChangedConnection);
        QPointer<ProfileTransfer> transfer(this);
        m_importingChangedConnection = connect(m_model, &RadioModel::profileDatabaseImportingChanged,
                                                this, [transfer, generation](bool importing) {
            if (!transfer || importing || !transfer->isCurrent(generation, Phase::WaitingForImport)) {
                return;
            }
            transfer->scheduleImportCompletion();
        });
        return;
    }

    scheduleImportCompletion();
}

void ProfileTransfer::scheduleImportCompletion()
{
    if (!m_busy || m_importCompletionScheduled)
        return;

    m_importCompletionScheduled = true;
    stopCommandTimeout();
    const quint64 generation = m_operationGeneration;
    QPointer<ProfileTransfer> transfer(this);
    QTimer::singleShot(kImportSettleMs, this, [transfer, generation] {
        if (!transfer || !transfer->isCurrent(generation, Phase::WaitingForImport)) {
            return;
        }
        transfer->completeImport();
    });
}

void ProfileTransfer::completeImport()
{
    const QPointer<ProfileTransfer> alive(this);
    const quint64 generation = m_operationGeneration;
    if (!isCurrent(generation, Phase::WaitingForImport)) {
        return;
    }

    stopCommandTimeout();
    if (m_model)
        m_model->refreshProfiles();
    if (!isCurrent(generation, Phase::WaitingForImport)) {
        return;
    }
    emit progress(QStringLiteral("Import complete. Refreshing profile lists..."),
                  m_bytesTotal, m_bytesTotal);
    if (!alive) { return; }
    if (!isCurrent(generation, Phase::WaitingForImport)) {
        return;
    }
    finish(m_path);
}

void ProfileTransfer::resetIdleTimer()
{
    startIdleTimeout(m_phase);
}

void ProfileTransfer::handleTimeout()
{
    if (!m_busy)
        return;

    switch (m_phase) {
    case Phase::UploadMetaSubset:
    case Phase::UploadImport:
        if (m_socket && !m_uploadConnected && m_bytesQueued == 0
            && m_socket->state() != QAbstractSocket::ConnectedState)
            tryFallbackUploadPort();
        else
            fail(QStringLiteral("Timed out while uploading to the radio."));
        break;
    case Phase::DownloadPackage:
        fail(QStringLiteral("Timed out waiting for the radio database download."));
        break;
    case Phase::WaitingForImport:
        scheduleImportCompletion();
        break;
    case Phase::Idle:
        break;
    }
}

bool ProfileTransfer::commitExportFile(QString* error)
{
    if (!m_saveFile || !m_saveFile->flush() || !m_saveFile->seek(0)
        || m_saveFile->size() <= 0 || m_saveFile->size() > kMaxImportSize) {
        *error = QStringLiteral("Export package is unavailable or exceeds the size limit.");
        return false;
    }
    const QByteArray payload = m_saveFile->read(kMaxImportSize + 1);
    if (payload.size() != m_saveFile->size() || readZipEntries(payload, error).isEmpty()) {
        if (error->isEmpty()) { *error = QStringLiteral("Invalid or incomplete export package."); }
        return false;
    }
    QSaveFile destination(m_path);
    destination.setDirectWriteFallback(false);
    if (!destination.open(QIODevice::WriteOnly)
        || destination.write(payload) != payload.size() || !destination.commit()) {
        *error = QStringLiteral("Could not finalize export package: %1").arg(destination.errorString());
        return false;
    }
    return true;
}

} // namespace AetherSDR
