#include "WaveformInstaller.h"

#include "LegacyWaveformPackage.h"
#include "LogManager.h"
#include "../models/RadioModel.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QTcpSocket>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <utility>

namespace AetherSDR {

namespace {

bool isSafeUploadFileName(const QString& fileName)
{
    if (fileName.isEmpty() || fileName.size() > 255) {
        return false;
    }

    for (const QChar ch : fileName) {
        const ushort c = ch.unicode();
        const bool allowed =
            (c >= 'A' && c <= 'Z')
            || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9')
            || c == '.'
            || c == '_'
            || c == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

QString installerText(const char* source)
{
    return QCoreApplication::translate("WaveformInstaller", source);
}

} // namespace

WaveformInstaller::WaveformInstaller(RadioModel* model, QObject* parent)
    : QObject(parent), m_model(model)
{
}

void WaveformInstaller::install(const QString& filePath)
{
    installDockerWaveform(filePath);
}

void WaveformInstaller::installLegacyWaveform(const QString& filePath)
{
    beginInstall(filePath, PackageKind::Legacy);
}

void WaveformInstaller::installDockerWaveform(const QString& filePath)
{
    beginInstall(filePath, PackageKind::Docker);
}

WaveformInstaller::PackagePreparationResult WaveformInstaller::preparePackage(
    const QString& filePath,
    PackageKind kind,
    qint64 expectedSize)
{
    PackagePreparationResult result;
    const QFileInfo fileInfo(filePath);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        result.error = installerText("Waveform package no longer exists.");
        return result;
    }
    if (fileInfo.size() != expectedSize
        || expectedSize <= 0
        || expectedSize > kMaxFileBytes) {
        result.error = installerText("Waveform package changed while it was being prepared.");
        return result;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = installerText("Cannot open file: %1").arg(file.errorString());
        return result;
    }

    if (kind == PackageKind::Docker) {
        const QByteArray header = file.read(262);
        const bool isGzip = header.size() >= 2
            && static_cast<quint8>(header[0]) == 0x1fU
            && static_cast<quint8>(header[1]) == 0x8bU;
        const bool isTar = header.size() >= 262
            && header.mid(257, 5) == QByteArray("ustar", 5);
        if (!isGzip && !isTar) {
            result.error = installerText(
                "Not a valid Docker waveform image (expected a .tar.gz or tar archive)");
            return result;
        }
        if (!file.seek(0)) {
            result.error = installerText("Could not rewind the waveform package.");
            return result;
        }
    }

    QByteArray data = file.readAll();
    if (data.size() != expectedSize) {
        result.error = installerText("Could not read the complete waveform package");
        return result;
    }

    if (kind == PackageKind::Legacy) {
        const LegacyWaveformPackageInfo package = inspectLegacyWaveformPackage(data);
        if (!package.valid) {
            result.error = installerText("Not a valid legacy SmartSDR waveform package: %1")
                .arg(package.error);
            return result;
        }
    }

    result.data = std::move(data);
    return result;
}

void WaveformInstaller::beginInstall(const QString& filePath, PackageKind kind)
{
    if (isInstalling()) {
        emit finished(false, tr("Install already in progress"));
        return;
    }
    if (!m_model) {
        emit finished(false, tr("No radio is available for waveform installation"));
        return;
    }

    const QFileInfo fileInfo(filePath);
    const QString fileName = fileInfo.fileName();
    if (!isSafeUploadFileName(fileName)) {
        emit finished(false, tr("Waveform package filename contains characters "
                                "that cannot be sent safely to the radio. "
                                "Use letters, numbers, dots, dashes, and underscores."));
        return;
    }
    if (kind == PackageKind::Legacy
        && fileInfo.suffix().compare(QStringLiteral("ssdr_waveform"),
                                     Qt::CaseInsensitive) != 0) {
        emit finished(false, tr("Not a legacy SmartSDR waveform package "
                                "(expected .ssdr_waveform)"));
        return;
    }

    const qint64 fileSize = fileInfo.size();
    if (fileSize <= 0) {
        emit finished(false, tr("File is empty or unavailable"));
        return;
    }
    if (fileSize > kMaxFileBytes) {
        emit finished(false, tr("File too large (> 500 MB)"));
        return;
    }

    m_fileName = fileName;
    m_packageKind = kind;
    m_uploadPort = 0;
    m_activeGeneration = m_uploadState.begin(fileSize);
    const Generation generation = m_activeGeneration;

    emit progressChanged(0, tr("Reading and validating waveform package..."));

    auto* watcher = new QFutureWatcher<PackagePreparationResult>(this);
    connect(watcher, &QFutureWatcher<PackagePreparationResult>::finished,
            this, [this, watcher, generation] {
                const PackagePreparationResult result = watcher->result();
                watcher->deleteLater();
                if (!m_uploadState.isCurrent(generation)) {
                    return;
                }
                if (!result.error.isEmpty()) {
                    finishOperation(generation, false, result.error);
                    return;
                }
                m_fileData = result.data;
                requestUploadPort(generation);
            });
    watcher->setFuture(QtConcurrent::run(
        [path = fileInfo.absoluteFilePath(), kind, fileSize] {
            return preparePackage(path, kind, fileSize);
        }));
}

void WaveformInstaller::requestUploadPort(Generation generation)
{
    if (!m_uploadState.isCurrent(generation)) {
        return;
    }
    if (!m_model) {
        finishOperation(generation,
                        false,
                        tr("Radio disconnected during installation"));
        return;
    }

    emit progressChanged(0, tr("Requesting upload port..."));
    const QPointer<WaveformInstaller> self(this);
    if (m_packageKind == PackageKind::Legacy) {
        m_model->sendCmdPublic(
            QStringLiteral("file filename ") + m_fileName,
            [self, generation](int code, const QString&) {
                if (!self || !self->m_uploadState.isCurrent(generation)) {
                    return;
                }
                if (code != 0) {
                    self->finishOperation(
                        generation,
                        false,
                        self->tr("Radio rejected the waveform filename (error 0x%1)")
                            .arg(code, 0, 16));
                    return;
                }
                if (!self->m_model) {
                    self->finishOperation(generation,
                                          false,
                                          self->tr("Radio disconnected during installation"));
                    return;
                }
                self->m_model->sendCmdPublic(
                    QStringLiteral("file upload %1 new_waveform")
                        .arg(self->m_fileData.size()),
                    [self, generation](int uploadCode, const QString& body) {
                        if (self) {
                            self->onUploadPortReceived(generation, uploadCode, body);
                        }
                    });
            });
        return;
    }

    m_model->sendCmdPublic(
        QStringLiteral("file upload %1 waveform_docker_image %2")
            .arg(m_fileData.size())
            .arg(m_fileName),
        [self, generation](int code, const QString& body) {
            if (self) {
                self->onUploadPortReceived(generation, code, body);
            }
        });
}

void WaveformInstaller::cancel()
{
    if (!isInstalling()) {
        return;
    }

    destroySocket();
    m_uploadState.invalidate();
    m_fileData.clear();
    m_uploadPort = 0;
    emit finished(false, tr("Install cancelled"));
}

void WaveformInstaller::onUploadPortReceived(Generation generation,
                                             int code,
                                             const QString& body)
{
    if (!m_uploadState.isCurrent(generation)) {
        return;
    }
    if (code != 0) {
        finishOperation(generation,
                        false,
                        tr("Radio rejected upload (error 0x%1)").arg(code, 0, 16));
        return;
    }

    bool ok = false;
    const uint parsedPort = body.trimmed().toUInt(&ok);
    m_uploadPort = ok && parsedPort > 0U && parsedPort <= 65535U
        ? static_cast<quint16>(parsedPort)
        : kFallbackPort;
    emit progressChanged(0, tr("Connecting to port %1...").arg(m_uploadPort));

    QTimer::singleShot(kConnectDelayMs, this, [this, generation] {
        if (m_uploadState.isCurrent(generation)) {
            connectUploadSocket(generation, m_uploadPort);
        }
    });
}

void WaveformInstaller::connectUploadSocket(Generation generation, quint16 port)
{
    if (!m_uploadState.isCurrent(generation)) {
        return;
    }
    if (!m_model) {
        finishOperation(generation,
                        false,
                        tr("Radio disconnected during installation"));
        return;
    }

    destroySocket();
    m_uploadPort = port;
    auto* socket = new QTcpSocket(this);
    m_socket = socket;

    connect(socket, &QTcpSocket::connected, this,
            [this, generation, socket] { onConnected(generation, socket); });
    connect(socket, &QTcpSocket::bytesWritten, this,
            [this, generation, socket](qint64 bytes) {
                onBytesWritten(generation, socket, bytes);
            });
    connect(socket, &QTcpSocket::disconnected, this,
            [this, generation, socket] { onDisconnected(generation, socket); });
    connect(socket, &QTcpSocket::errorOccurred, this,
            [this, generation, socket](QAbstractSocket::SocketError) {
                onError(generation, socket);
            });

    qCDebug(lcWaveform) << "WaveformInstaller: connecting to upload port" << port;
    socket->connectToHost(m_model->radioAddress(), port);

    const QPointer<QTcpSocket> guardedSocket(socket);
    QTimer::singleShot(kConnectTimeoutMs, this,
                       [this, generation, guardedSocket, port] {
        if (!m_uploadState.isCurrent(generation)
            || !guardedSocket
            || guardedSocket != m_socket
            || guardedSocket->state() == QAbstractSocket::ConnectedState) {
            return;
        }
        if (port != kFallbackPort) {
            tryFallbackPort(generation);
        } else {
            finishOperation(generation, false, tr("Cannot connect to upload port"));
        }
    });
}

void WaveformInstaller::tryFallbackPort(Generation generation)
{
    if (!m_uploadState.isCurrent(generation)) {
        return;
    }
    if (m_uploadPort == kFallbackPort) {
        finishOperation(generation, false, tr("Cannot connect to upload port"));
        return;
    }

    emit progressChanged(0, tr("Trying fallback port %1...").arg(kFallbackPort));
    connectUploadSocket(generation, kFallbackPort);
}

void WaveformInstaller::onConnected(Generation generation, QTcpSocket* socket)
{
    if (!m_uploadState.isCurrent(generation) || socket != m_socket) {
        return;
    }

    qCDebug(lcWaveform) << "WaveformInstaller: connected, sending"
                        << m_fileData.size() << "bytes";
    emit progressChanged(0, m_packageKind == PackageKind::Legacy
                             ? tr("Uploading legacy waveform...")
                             : tr("Uploading Docker waveform image..."));
    queueNextChunk(generation, socket);
}

void WaveformInstaller::queueNextChunk(Generation generation, QTcpSocket* socket)
{
    if (!m_uploadState.isCurrent(generation) || socket != m_socket) {
        return;
    }

    const qint64 requested = m_uploadState.nextChunkSize(generation, kChunkSize);
    if (requested <= 0) {
        return;
    }
    const qint64 offset = m_uploadState.queuedBytes();
    const qint64 accepted = socket->write(m_fileData.constData() + offset, requested);
    if (!m_uploadState.recordQueued(generation, requested, accepted)) {
        const QString detail = accepted < 0
            ? socket->errorString()
            : tr("socket accepted no data");
        finishOperation(generation,
                        false,
                        tr("Upload failed: %1").arg(detail));
    }
}

void WaveformInstaller::onBytesWritten(Generation generation,
                                       QTcpSocket* socket,
                                       qint64 bytes)
{
    if (!m_uploadState.isCurrent(generation) || socket != m_socket) {
        return;
    }
    if (!m_uploadState.acknowledge(generation, bytes)) {
        finishOperation(generation,
                        false,
                        tr("Upload failed: invalid socket byte accounting"));
        return;
    }

    const qint64 acknowledged = m_uploadState.acknowledgedBytes();
    const qint64 total = m_uploadState.totalBytes();
    const int percent = static_cast<int>((acknowledged * 100LL) / total);
    emit progressChanged(percent,
                         tr("Uploading... %1 / %2 KB")
                             .arg(acknowledged / 1024)
                             .arg(total / 1024));

    if (m_uploadState.complete(generation)) {
        qCDebug(lcWaveform) << "WaveformInstaller: upload complete";
        emit progressChanged(100, tr("Upload complete - installing on radio..."));
        finishOperation(
            generation,
            true,
            m_packageKind == PackageKind::Legacy
                ? tr("Legacy waveform uploaded successfully. "
                     "The radio will install it momentarily.")
                : tr("Docker waveform image uploaded successfully. "
                     "The radio will install it momentarily."));
        return;
    }

    if (socket->bytesToWrite() < kChunkSize) {
        queueNextChunk(generation, socket);
    }
}

void WaveformInstaller::onDisconnected(Generation generation, QTcpSocket* socket)
{
    if (!m_uploadState.isCurrent(generation) || socket != m_socket) {
        return;
    }
    if (!m_uploadState.complete(generation)) {
        finishOperation(generation,
                        false,
                        tr("Radio closed the upload connection before the transfer completed"));
    }
}

void WaveformInstaller::onError(Generation generation, QTcpSocket* socket)
{
    if (!m_uploadState.isCurrent(generation) || socket != m_socket) {
        return;
    }

    if (m_uploadState.queuedBytes() == 0 && m_uploadPort != kFallbackPort) {
        tryFallbackPort(generation);
        return;
    }
    finishOperation(generation,
                    false,
                    tr("Upload failed: %1").arg(socket->errorString()));
}

void WaveformInstaller::finishOperation(Generation generation,
                                        bool success,
                                        const QString& message)
{
    if (!m_uploadState.isCurrent(generation)) {
        return;
    }

    destroySocket(!success);
    m_uploadState.invalidate();
    m_fileData.clear();
    m_uploadPort = 0;
    emit finished(success, message);
}

void WaveformInstaller::destroySocket(bool abortConnection)
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

} // namespace AetherSDR
