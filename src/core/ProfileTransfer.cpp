#include "ProfileTransfer.h"

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
    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, &ProfileTransfer::handleTimeout);

    m_idleTimer = new QTimer(this);
    m_idleTimer->setSingleShot(true);
    connect(m_idleTimer, &QTimer::timeout, this, [this] {
        if (!m_busy)
            return;
        fail(QStringLiteral("Transfer timed out while waiting for data."));
    });

    m_overallTimer = new QTimer(this);
    m_overallTimer->setSingleShot(true);
    connect(m_overallTimer, &QTimer::timeout, this, [this] {
        if (!m_busy)
            return;
        fail(QStringLiteral("Profile database transfer timed out."));
    });

    if (m_model) {
        connect(m_model, &RadioModel::profileDatabaseImportingChanged, this, [this](bool importing) {
            if (m_phase == Phase::WaitingForImport && !importing)
                scheduleImportCompletion();
        });
    }
}

ProfileTransfer::~ProfileTransfer()
{
    if (m_busy)
        cleanup();
}

void ProfileTransfer::exportDatabase(const ExportSelection& selection, const QString& destinationPath)
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

    begin(Operation::ExportDatabase, Phase::UploadMetaSubset);
    m_path = destinationPath;

    qCInfo(lcProtocol).noquote()
        << "ProfileTransfer: export requested"
        << QStringLiteral("meta_subset_bytes=%1").arg(metaSubset.size());
    emit progress(QStringLiteral("Sending export selection to radio..."), 0, metaSubset.size());
    requestUploadPort(metaSubset, QStringLiteral("db_meta_subset"));
}

void ProfileTransfer::importDatabase(const QString& ssdrCfgPath)
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
    const QByteArray payload = file.readAll();
    if (payload.size() != file.size()) {
        emit failed(Operation::ImportDatabase,
                    QStringLiteral("Could not read the complete .ssdr_cfg package."));
        return;
    }
    bool repackaged = false;
    const QByteArray importPayload = prepareDatabaseImportPayload(payload, &repackaged, &error);
    if (importPayload.isEmpty()) {
        emit failed(Operation::ImportDatabase, error);
        return;
    }

    begin(Operation::ImportDatabase, Phase::UploadImport);
    m_path = ssdrCfgPath;

    qCInfo(lcProtocol).noquote()
        << "ProfileTransfer: import requested"
        << QStringLiteral("package_bytes=%1").arg(payload.size())
        << QStringLiteral("upload_bytes=%1").arg(importPayload.size())
        << QStringLiteral("repackaged=%1").arg(repackaged ? 1 : 0);
    emit progress(QStringLiteral("Requesting database import upload port..."), 0, importPayload.size());
    requestUploadPort(importPayload, QStringLiteral("db_import"));
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

void ProfileTransfer::begin(Operation operation, Phase phase)
{
    m_operation = operation;
    m_phase = phase;
    m_busy = true;
    m_cancelled = false;
    m_usedFallbackPort = false;
    m_downloadFinalized = false;
    m_importCompletionScheduled = false;
    m_bytesDone = 0;
    m_bytesQueued = 0;
    m_bytesTotal = 0;
    m_uploadPort = 0;
    m_overallTimer->start(kOverallTimeoutMs);
    emit started(operation);
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
    m_timeout->stop();
    m_idleTimer->stop();
    m_overallTimer->stop();

    if (m_socket) {
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
    if (m_saveFile) {
        m_saveFile->cancelWriting();
        m_saveFile->deleteLater();
        m_saveFile = nullptr;
    }

    m_uploadPayload.clear();
    m_path.clear();
    m_bytesDone = 0;
    m_bytesQueued = 0;
    m_bytesTotal = 0;
    m_uploadPort = 0;
    m_busy = false;
    m_cancelled = false;
    m_usedFallbackPort = false;
    m_downloadFinalized = false;
    m_importCompletionScheduled = false;
    m_phase = Phase::Idle;
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
    if (m_busy) {
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

void ProfileTransfer::requestUploadPort(const QByteArray& payload, const QString& uploadKind)
{
    m_uploadPayload = payload;
    m_bytesDone = 0;
    m_bytesQueued = 0;
    m_bytesTotal = payload.size();
    m_usedFallbackPort = false;

    const QString command = QStringLiteral("file upload %1 %2").arg(payload.size()).arg(uploadKind);
    qCInfo(lcProtocol).noquote()
        << "ProfileTransfer: command"
        << command;
    m_timeout->start(kCommandTimeoutMs);
    m_model->requestFileUploadPort(payload.size(), uploadKind,
        [this](int code, const QString& body) {
            onUploadPortReceived(code, body);
        });
}

void ProfileTransfer::onUploadPortReceived(int code, const QString& body)
{
    if (m_cancelled || !m_busy)
        return;

    m_timeout->stop();
    if (code != 0) {
        fail(QStringLiteral("Radio rejected the file upload request (error 0x%1).")
                 .arg(code, 0, 16));
        return;
    }

    const auto parsedPort = parseTransferPort(body);
    const quint16 port = parsedPort.value_or(kDefaultUploadPort);
    connectUploadSocket(port);
}

void ProfileTransfer::connectUploadSocket(quint16 port)
{
    if (m_socket) {
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }

    m_uploadPort = port;
    m_socket = new QTcpSocket(this);
    connect(m_socket, &QTcpSocket::connected, this, &ProfileTransfer::onUploadConnected);
    connect(m_socket, &QTcpSocket::bytesWritten, this, &ProfileTransfer::onUploadBytesWritten);
    connect(m_socket, &QTcpSocket::disconnected, this, &ProfileTransfer::onUploadDisconnected);
    connect(m_socket, &QTcpSocket::errorOccurred, this, [this] { onUploadError(); });

    emit progress(QStringLiteral("Connecting to radio upload port %1...").arg(port),
                  m_bytesDone, m_bytesTotal);
    qCInfo(lcProtocol) << "ProfileTransfer: connecting to upload port" << port;

    QTimer::singleShot(200, this, [this, port] {
        if (!m_busy || m_cancelled || !m_socket)
            return;
        m_socket->connectToHost(m_model->radioAddress(), port);
    });
    m_timeout->start(kConnectTimeoutMs);
}

void ProfileTransfer::tryFallbackUploadPort()
{
    if (m_usedFallbackPort) {
        fail(QStringLiteral("Cannot connect to the radio transfer port."));
        return;
    }
    m_usedFallbackPort = true;
    emit progress(QStringLiteral("Trying fallback transfer port %1...").arg(kFallbackTransferPort),
                  m_bytesDone, m_bytesTotal);
    connectUploadSocket(kFallbackTransferPort);
}

void ProfileTransfer::onUploadConnected()
{
    if (m_cancelled || !m_busy)
        return;

    m_timeout->stop();
    resetIdleTimer();
    emit progress(m_phase == Phase::UploadMetaSubset
                      ? QStringLiteral("Uploading export selection...")
                      : QStringLiteral("Uploading SmartSDR database package..."),
                  m_bytesDone, m_bytesTotal);
    sendNextUploadChunk();
}

void ProfileTransfer::sendNextUploadChunk()
{
    if (!m_socket || m_cancelled)
        return;

    const qint64 remaining = m_uploadPayload.size() - m_bytesQueued;
    if (remaining <= 0)
        return;

    const qint64 toSend = qMin<qint64>(kUploadChunkSize, remaining);
    const qint64 written = m_socket->write(m_uploadPayload.constData() + m_bytesQueued, toSend);
    if (written < 0)
        onUploadError();
    else
        m_bytesQueued += written;
}

void ProfileTransfer::onUploadBytesWritten(qint64 bytes)
{
    if (m_cancelled || !m_busy)
        return;

    resetIdleTimer();
    m_bytesDone += bytes;
    emit progress(m_phase == Phase::UploadMetaSubset
                      ? QStringLiteral("Uploading export selection...")
                      : QStringLiteral("Uploading SmartSDR database package..."),
                  m_bytesDone, m_bytesTotal);

    if (m_bytesDone >= m_bytesTotal) {
        qCInfo(lcProtocol) << "ProfileTransfer: upload complete" << m_bytesDone << "bytes";
        m_idleTimer->stop();
        m_socket->flush();
        m_socket->disconnectFromHost();
        return;
    }

    if (m_bytesQueued < m_bytesTotal)
        sendNextUploadChunk();
}

void ProfileTransfer::onUploadDisconnected()
{
    if (m_cancelled || !m_busy)
        return;

    if (m_bytesDone < m_bytesTotal) {
        fail(QStringLiteral("Radio closed the upload connection before the transfer completed."));
        return;
    }

    if (m_socket) {
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_uploadPayload.clear();

    if (m_phase == Phase::UploadMetaSubset) {
        emit progress(QStringLiteral("Waiting for the radio to prepare the database package..."), 0, 0);
        QTimer::singleShot(kMetaSubsetSettleMs, this, [this] {
            if (!m_busy || m_cancelled)
                return;
            requestPackageDownload();
        });
        return;
    }

    if (m_phase == Phase::UploadImport) {
        waitForImportCompletion();
        return;
    }
}

void ProfileTransfer::onUploadError()
{
    if (m_cancelled || !m_busy)
        return;

    if (m_bytesDone == 0 && m_uploadPort != kFallbackTransferPort) {
        tryFallbackUploadPort();
        return;
    }

    const QString err = m_socket ? m_socket->errorString() : QStringLiteral("Unknown socket error");
    fail(QStringLiteral("Upload failed: %1").arg(err));
}

void ProfileTransfer::requestPackageDownload()
{
    if (!m_busy)
        return;

    m_phase = Phase::DownloadPackage;
    m_bytesDone = 0;
    m_bytesTotal = 0;
    emit progress(QStringLiteral("Requesting SmartSDR database package..."), 0, 0);
    qCInfo(lcProtocol) << "ProfileTransfer: command file download db_package";

    m_timeout->start(kCommandTimeoutMs);
    m_model->requestFileDownloadPort(QStringLiteral("db_package"),
        [this](int code, const QString& body) {
            onDownloadPortReceived(code, body);
        });
}

void ProfileTransfer::onDownloadPortReceived(int code, const QString& body)
{
    if (m_cancelled || !m_busy)
        return;

    m_timeout->stop();
    if (code != 0) {
        fail(QStringLiteral("Radio rejected the database download request (error 0x%1).")
                 .arg(code, 0, 16));
        return;
    }

    const auto parsedPort = parseTransferPort(body);
    const quint16 port = parsedPort.value_or(kFallbackTransferPort);
    startDownloadServer(port);
}

void ProfileTransfer::startDownloadServer(quint16 port)
{
    QString error;
    if (!validateExportDestination(m_path, &error)) {
        fail(error);
        return;
    }

    m_saveFile = new QSaveFile(m_path, this);
    if (!m_saveFile->open(QIODevice::WriteOnly)) {
        fail(QStringLiteral("Cannot create export file: %1").arg(m_saveFile->errorString()));
        return;
    }

    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &ProfileTransfer::onDownloadConnection);
    if (!m_server->listen(QHostAddress::Any, port)) {
        fail(QStringLiteral("Cannot listen for the radio database download on port %1: %2")
                 .arg(port)
                 .arg(m_server->errorString()));
        return;
    }

    qCInfo(lcProtocol) << "ProfileTransfer: listening for database package on port" << port;
    emit progress(QStringLiteral("Waiting for radio database download on port %1...").arg(port), 0, 0);
    m_timeout->start(kConnectTimeoutMs);
}

void ProfileTransfer::onDownloadConnection()
{
    if (m_cancelled || !m_busy)
        return;

    m_timeout->stop();
    m_socket = m_server->nextPendingConnection();
    if (!m_socket)
        return;
    m_server->close();

    connect(m_socket, &QTcpSocket::readyRead, this, &ProfileTransfer::onDownloadReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &ProfileTransfer::onDownloadDisconnected);
    connect(m_socket, &QTcpSocket::errorOccurred, this, [this] { onDownloadError(); });

    resetIdleTimer();
    emit progress(QStringLiteral("Receiving SmartSDR database package..."), 0, 0);
}

void ProfileTransfer::onDownloadReadyRead()
{
    if (m_cancelled || !m_saveFile || !m_socket)
        return;

    resetIdleTimer();
    const QByteArray chunk = m_socket->readAll();
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

void ProfileTransfer::onDownloadDisconnected()
{
    if (m_cancelled || !m_busy || m_downloadFinalized)
        return;

    m_downloadFinalized = true;
    m_idleTimer->stop();
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
    const QString finishedPath = m_path;
    finish(finishedPath);
}

void ProfileTransfer::onDownloadError()
{
    if (m_cancelled || !m_busy)
        return;

    if (m_socket && m_socket->error() == QAbstractSocket::RemoteHostClosedError && m_bytesDone > 0) {
        onDownloadDisconnected();
        return;
    }

    const QString err = m_socket ? m_socket->errorString() : QStringLiteral("Unknown socket error");
    fail(QStringLiteral("Database download failed: %1").arg(err));
}

void ProfileTransfer::waitForImportCompletion()
{
    m_phase = Phase::WaitingForImport;
    m_idleTimer->stop();
    emit progress(QStringLiteral("Import sent; waiting for radio to apply the database..."),
                  m_bytesDone, m_bytesTotal);

    if (m_model && m_model->profileDatabaseImporting()) {
        m_timeout->start(kOverallTimeoutMs / 2);
        return;
    }

    scheduleImportCompletion();
}

void ProfileTransfer::scheduleImportCompletion()
{
    if (!m_busy || m_importCompletionScheduled)
        return;

    m_importCompletionScheduled = true;
    m_timeout->stop();
    QTimer::singleShot(kImportSettleMs, this, [this] {
        if (!m_busy || m_cancelled)
            return;
        completeImport();
    });
}

void ProfileTransfer::completeImport()
{
    if (!m_busy)
        return;

    m_timeout->stop();
    if (m_model)
        m_model->refreshProfiles();
    emit progress(QStringLiteral("Import complete. Refreshing profile lists..."),
                  m_bytesTotal, m_bytesTotal);
    finish(m_path);
}

void ProfileTransfer::resetIdleTimer()
{
    if (m_idleTimer)
        m_idleTimer->start(kIdleTimeoutMs);
}

void ProfileTransfer::handleTimeout()
{
    if (!m_busy)
        return;

    switch (m_phase) {
    case Phase::UploadMetaSubset:
    case Phase::UploadImport:
        if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
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
    if (!m_saveFile) {
        *error = QStringLiteral("Export file is not open.");
        return false;
    }

    if (!m_saveFile->commit()) {
        *error = QStringLiteral("Could not finalize export package: %1").arg(m_saveFile->errorString());
        return false;
    }
    m_saveFile->deleteLater();
    m_saveFile = nullptr;

    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Could not verify exported package.");
        return false;
    }
    const QByteArray magic = file.read(4);
    if (magic.size() >= 2 && magic.left(2) != "PK") {
        qCWarning(lcProtocol) << "ProfileTransfer: exported package does not start with ZIP magic";
    }
    return true;
}

} // namespace AetherSDR
