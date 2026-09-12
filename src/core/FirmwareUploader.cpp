#include "FirmwareUploader.h"

#include "LogManager.h"
#include "../models/RadioModel.h"

#include <QFile>
#include <QFileInfo>
#include <QTcpSocket>
#include <QTimer>

#include <cmath>
#include <limits>

namespace AetherSDR {

FirmwareUploader::FirmwareUploader(RadioModel* model, QObject* parent)
    : QObject(parent), m_model(model)
{
    if (m_model) {
        connect(m_model, &RadioModel::connectionStateChanged,
                this, &FirmwareUploader::handleConnectionStateChanged);
    }
    m_timeoutTimer.setSingleShot(true);
    connect(&m_timeoutTimer, &QTimer::timeout, this, [this] {
        onTimeout(m_timeoutGeneration, m_timeoutToken, m_timeoutMessage);
    });
    m_overallTimeoutTimer.setSingleShot(true);
    connect(&m_overallTimeoutTimer, &QTimer::timeout, this, [this] {
        onOverallTimeout(m_overallTimeoutGeneration, m_overallTimeoutToken);
    });
}

void FirmwareUploader::upload(const QString& filePath)
{
    // Do not emit a terminal result for the operation which already owns this uploader.
    if (m_uploading) {
        return;
    }
    if (!m_model || !m_model->isConnected()) {
        emit finished(Outcome::Failed, tr("Connect to the radio before uploading firmware"));
        return;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit finished(Outcome::Failed, tr("Cannot open file: %1").arg(file.errorString()));
        return;
    }
    if (file.size() > kMaxFileBytes) {
        emit finished(Outcome::Failed, tr("File too large (> 500MB)"));
        return;
    }
    const QByteArray fileData = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        emit finished(Outcome::Failed, tr("Could not read firmware file: %1").arg(file.errorString()));
        return;
    }
    if (fileData.isEmpty()) {
        emit finished(Outcome::Failed, tr("File is empty"));
        return;
    }
    if (fileData.size() != file.size()) {
        emit finished(Outcome::Failed, tr("Firmware file changed while it was being read"));
        return;
    }

    if (!beginOperation(fileData, QFileInfo(filePath).fileName())) {
        return;
    }
    const Generation generation = m_generation;
    emit progressChanged(0, tr("Preparing upload..."));
    if (isCurrent(generation)) {
        requestUploadPort(generation);
    }
}

void FirmwareUploader::cancel()
{
    if (!m_uploading) {
        return;
    }
    finishOperation(m_generation, Outcome::Failed, tr("Firmware upload cancelled"));
}

bool FirmwareUploader::beginOperation(const QByteArray& fileData, const QString& fileName)
{
    // File-update status has no attempt identifier. A late failure from the
    // previous upload must not be attributed to a retry on the same session.
    if (retryBarrierActive()) {
        emit finished(Outcome::Failed, tr("Disconnect and reconnect to the radio before retrying firmware upload; "
                               "the previous update outcome may still be pending"));
        return false;
    }
    destroySocket();
    disconnectStatusRelay();
    m_timeoutTimer.stop();
    m_overallTimeoutTimer.stop();
    m_generation = nextGeneration();
    m_timeoutToken = 0;
    m_fileData = fileData;
    m_fileName = fileName;
    m_writer = {};
    m_bytesQueued = 0;
    m_bytesAcknowledged = 0;
    m_pendingBytes = 0;
    m_uploadPort = 0;
    m_uploading = true;
    m_waitingForConfirmation = false;
    m_radioProgressSeen = false;
    m_radioProgressPercent = 0;
    m_radioProgressAge.invalidate();
    m_outcomeSettledByRadio = false;
    armOverallTimeout(m_generation);

    if (!m_model) {
        return true;
    }
    const Generation generation = m_generation;
    const QPointer<FirmwareUploader> self(this);
    m_statusConnection = connect(
        m_model, &RadioModel::statusReceived, this,
        [self, generation](const QString& object, const QMap<QString, QString>& kvs) {
            if (self) {
                self->onRadioStatus(generation, object, kvs);
            }
        });
    return true;
}

void FirmwareUploader::requestUploadPort(Generation generation)
{
    if (!isCurrent(generation)) {
        return;
    }
    if (!m_model) {
        finishOperation(generation, Outcome::Failed, tr("Radio disconnected before upload started"));
        return;
    }

    emit progressChanged(0, tr("Requesting upload port..."));
    if (!isCurrent(generation)) {
        return;
    }
    armTimeout(generation,
               kUploadPortTimeoutMs,
               tr("Radio did not provide a firmware upload port"));
    if (!isCurrent(generation)) {
        return;
    }
    m_model->sendCommand(QStringLiteral("file filename ") + m_fileName);
    if (!isCurrent(generation) || !m_model) {
        return;
    }
    // The barrier is armed in onUploadPortReceived, once the radio has actually
    // accepted the command and opened its file server. Arming it here would
    // lock out a retry after a rejection that dispatched no firmware byte and
    // can leave no `file update` status pending (#5572 review).
    const QPointer<FirmwareUploader> self(this);
    m_model->sendCmdPublic(
        QStringLiteral("file upload %1 update").arg(m_fileData.size()),
        [self, generation](int code, const QString& body) {
            if (self) {
                self->onUploadPortReceived(generation, code, body);
            }
        });
}

void FirmwareUploader::onUploadPortReceived(Generation generation,
                                             int code,
                                             const QString& body)
{
    if (!isCurrent(generation)) {
        return;
    }
    if (code != 0) {
        finishOperation(generation,
                        Outcome::Failed,
                        tr("Radio rejected firmware upload (error 0x%1)").arg(code, 0, 16));
        return;
    }

    // The radio accepted the upload and is standing up its file server: from
    // here a `file update` outcome may arrive for this attempt, so the retry
    // barrier becomes real.
    markUploadDispatched();

    bool parsed = false;
    const uint port = body.trimmed().toUInt(&parsed);
    const quint16 uploadPort = parsed && port > 0U && port <= 65535U
        ? static_cast<quint16>(port)
        : kDefaultPort;
    emit progressChanged(0, tr("Connecting to port %1...").arg(uploadPort));
    if (!isCurrent(generation)) {
        return;
    }

    const QPointer<FirmwareUploader> self(this);
    QTimer::singleShot(kConnectDelayMs, this, [self, generation, uploadPort] {
        if (self && self->isCurrent(generation)) {
            self->connectUploadSocket(generation, uploadPort);
        }
    });
}

void FirmwareUploader::connectUploadSocket(Generation generation, quint16 port)
{
    if (!isCurrent(generation)) {
        return;
    }
    if (!m_model) {
        finishOperation(generation, Outcome::Failed, tr("Radio disconnected before upload started"));
        return;
    }

    destroySocket();
    m_uploadPort = port;
    auto* socket = new QTcpSocket(this);
    m_socket = socket;
    const QPointer<FirmwareUploader> self(this);
    connect(socket, &QTcpSocket::connected, this, [self, generation, socket] {
        if (self) {
            self->onConnected(generation, socket);
        }
    });
    connect(socket, &QTcpSocket::bytesWritten, this,
            [self, generation, socket](qint64 bytes) {
                if (self) {
                    self->onBytesWritten(generation, socket, bytes);
                }
            });
    connect(socket, &QTcpSocket::disconnected, this, [self, generation, socket] {
        if (self) {
            self->onDisconnected(generation, socket);
        }
    });
    connect(socket, &QTcpSocket::errorOccurred, this,
            [self, generation, socket](QAbstractSocket::SocketError) {
                if (self) {
                    self->onError(generation, socket);
                }
            });

    qCDebug(lcFirmware) << "FirmwareUploader: connecting to upload port" << port;
    socket->connectToHost(m_model->radioAddress(), port);
    armTimeout(generation, kConnectTimeoutMs, tr("Cannot connect to firmware upload port"));
}

void FirmwareUploader::tryFallbackPort(Generation generation)
{
    if (!isCurrent(generation)) {
        return;
    }
    if (m_uploadPort == kFallbackPort) {
        finishOperation(generation, Outcome::Failed, tr("Cannot connect to firmware upload port"));
        return;
    }
    emit progressChanged(0, tr("Trying fallback port %1...").arg(kFallbackPort));
    if (!isCurrent(generation)) {
        return;
    }
    connectUploadSocket(generation, kFallbackPort);
}

void FirmwareUploader::onConnected(Generation generation, QTcpSocket* socket)
{
    if (!isCurrent(generation) || socket != m_socket) {
        return;
    }
    qCDebug(lcFirmware) << "FirmwareUploader: connected, sending" << m_fileData.size() << "bytes";
    emit progressChanged(0, tr("Uploading firmware..."));
    if (!isCurrent(generation)) {
        return;
    }
    startSending(generation, [socket](const char* data, qint64 size) {
        return socket->write(data, size);
    });
}

void FirmwareUploader::startSending(Generation generation, WriteFunction writer)
{
    if (!isCurrent(generation) || !writer || m_waitingForConfirmation) {
        return;
    }
    m_writer = std::move(writer);
    armTimeout(generation, kUploadInactivityTimeoutMs, tr("Firmware upload timed out"));
    queueNextChunk(generation);
}

void FirmwareUploader::queueNextChunk(Generation generation)
{
    if (!isCurrent(generation) || m_waitingForConfirmation || m_pendingBytes != 0) {
        return;
    }
    const qint64 remaining = m_fileData.size() - m_bytesQueued;
    if (remaining <= 0) {
        return;
    }
    const qint64 requested = qMin(kChunkSize, remaining);
    const WriteFunction writer = m_writer;
    const qint64 accepted = writer(m_fileData.constData() + m_bytesQueued, requested);
    if (!isCurrent(generation)) {
        return;
    }
    if (accepted <= 0 || accepted > requested || accepted > remaining) {
        finishOperation(generation, Outcome::Failed, tr("Upload failed: socket accepted no data"));
        return;
    }
    // Advance only by bytes accepted by write(), never by a partial drain from
    // an earlier chunk. The next write therefore cannot duplicate source bytes.
    m_bytesQueued += accepted;
    m_pendingBytes = accepted;
}

void FirmwareUploader::onBytesWritten(Generation generation,
                                      QTcpSocket* socket,
                                      qint64 bytes)
{
    if (!isCurrent(generation) || socket != m_socket) {
        return;
    }
    acknowledgeBytes(generation, bytes);
}

void FirmwareUploader::acknowledgeBytes(Generation generation, qint64 bytes)
{
    if (!isCurrent(generation) || m_waitingForConfirmation) {
        return;
    }
    if (bytes <= 0 || bytes > m_pendingBytes) {
        finishOperation(generation, Outcome::Failed, tr("Upload failed: invalid socket byte accounting"));
        return;
    }
    m_pendingBytes -= bytes;
    m_bytesAcknowledged += bytes;
    // Radio-reported transfer supersedes the local counter, but only while it
    // is still arriving. Latching m_radioProgressSeen forever would freeze the
    // bar at the last reported percentage if the status stream stalled while
    // TCP kept draining — the inactivity timer is re-armed just below, so
    // nothing else would notice (#5572 review).
    const bool radioProgressFresh =
        m_radioProgressSeen && m_radioProgressAge.isValid()
        && m_radioProgressAge.elapsed() < m_radioProgressStaleMs;
    if (!radioProgressFresh) {
        const int percent = static_cast<int>((m_bytesAcknowledged * 100LL) / m_fileData.size());
        emit progressChanged(percent,
                             tr("Uploading... %1 / %2 KB")
                                 .arg(m_bytesAcknowledged / 1024)
                                 .arg(m_fileData.size() / 1024));
        if (!isCurrent(generation)) {
            return;
        }
    }
    armTimeout(generation, kUploadInactivityTimeoutMs, tr("Firmware upload timed out"));

    if (m_bytesAcknowledged == m_fileData.size()) {
        if (m_bytesQueued != m_fileData.size() || m_pendingBytes != 0) {
            finishOperation(generation, Outcome::Failed, tr("Upload failed: invalid socket byte accounting"));
            return;
        }
        qCDebug(lcFirmware) << "FirmwareUploader: firmware bytes drained";
        m_waitingForConfirmation = true;
        m_writer = {};
        emit progressChanged(m_radioProgressSeen ? m_radioProgressPercent : 100,
                             tr("Firmware bytes sent; waiting for radio confirmation..."));
        if (!isCurrent(generation)) {
            return;
        }
        // FlexLib closes the upload stream before its post-upload delay; do
        // likewise instead of holding a completed upload socket open.
        destroySocket(false);
        armTimeout(generation,
                   kConfirmationTimeoutMs,
                   tr("Firmware bytes were sent, but the radio did not confirm installation. "
                      "Reconnect to check the radio's firmware version."));
        return;
    }

    if (m_pendingBytes == 0) {
        queueNextChunk(generation);
    }
}

void FirmwareUploader::onDisconnected(Generation generation, QTcpSocket* socket)
{
    if (!isCurrent(generation) || socket != m_socket) {
        return;
    }
    handleDisconnected(generation);
}

void FirmwareUploader::handleDisconnected(Generation generation)
{
    if (!isCurrent(generation) || m_waitingForConfirmation) {
        return;
    }
    finishOperation(generation,
                    Outcome::Failed,
                    tr("Radio closed the firmware upload connection before the transfer completed"));
}

void FirmwareUploader::onError(Generation generation, QTcpSocket* socket)
{
    if (!isCurrent(generation) || socket != m_socket || m_waitingForConfirmation) {
        return;
    }
    if (m_bytesQueued == 0 && m_uploadPort != kFallbackPort) {
        tryFallbackPort(generation);
        return;
    }
    finishOperation(generation, Outcome::Failed, tr("Upload failed: %1").arg(socket->errorString()));
}

void FirmwareUploader::onRadioStatus(Generation generation,
                                      const QString& object,
                                      const QMap<QString, QString>& kvs)
{
    if (!isCurrent(generation) || object != QStringLiteral("file update")) {
        return;
    }

    if (kvs.contains(QStringLiteral("failed"))) {
        bool parsed = false;
        const int failed = kvs.value(QStringLiteral("failed")).toInt(&parsed);
        if (parsed && failed == 1) {
            m_outcomeSettledByRadio = true;
            const QString reason = kvs.value(QStringLiteral("reason")).trimmed();
            finishOperation(generation,
                            Outcome::Failed,
                            reason.isEmpty()
                                ? tr("Radio reported that firmware installation failed")
                                : tr("Radio reported that firmware installation failed: %1").arg(reason));
            return;
        }
        if (parsed && failed == 0) {
            // The radio's own word that it accepted the image. FlexLib treats
            // any parseable `failed` as terminal and drops the command channel
            // on it ("close main command channel too since the radio will
            // reboot", Radio.cs:12626-12630), so this — not a drained socket,
            // not transfer=1.00 — is the one signal that confirms an install.
            m_outcomeSettledByRadio = true;
            finishOperation(generation,
                            Outcome::Succeeded,
                            tr("Radio accepted the firmware image and is rebooting"));
            return;
        }
    }

    bool parsed = false;
    const double transfer = kvs.value(QStringLiteral("transfer")).toDouble(&parsed);
    if (!parsed || !std::isfinite(transfer) || transfer < 0.0 || transfer > 1.0) {
        return;
    }
    const int percent = static_cast<int>(transfer * 100.0);
    m_radioProgressSeen = true;
    m_radioProgressPercent = percent;
    m_radioProgressAge.restart();
    emit progressChanged(percent,
                         m_waitingForConfirmation
                             ? tr("Radio reported firmware transfer %1%; installation remains unconfirmed")
                                   .arg(percent)
                             : tr("Radio reports firmware transfer... %1%").arg(percent));
    if (!isCurrent(generation)) {
        return;
    }
    if (m_waitingForConfirmation) {
        return;
    }
    armTimeout(generation,
               kUploadInactivityTimeoutMs,
               tr("Firmware upload timed out"));
}

void FirmwareUploader::markUploadDispatched()
{
    m_requiresFreshConnection = true;
    m_disconnectObserved = false;
    // This object is parented to RadioSetupDialog, which carries
    // WA_DeleteOnClose: closing the window destroys the uploader and would take
    // the barrier with it, so reopening would offer the ambiguous same-session
    // retry the barrier exists to forbid. Record it on the model, which outlives
    // the dialog and clears it only on a genuine reconnect (#5572 review).
    if (m_model) {
        m_model->blockFirmwareRetryUntilReconnect();
    }
}

bool FirmwareUploader::retryBarrierActive() const
{
    // The model is authoritative whenever there is one; the local copy is the
    // fallback for a model-less uploader (tests).
    if (m_model) {
        return m_model->firmwareRetryBlocked();
    }
    return m_requiresFreshConnection;
}

void FirmwareUploader::handleConnectionStateChanged(bool connected)
{
    if (!connected) {
        m_disconnectObserved = true;
        handleModelDisconnected(m_generation);
    } else if (m_disconnectObserved) {
        m_requiresFreshConnection = false;
        m_disconnectObserved = false;
    }
}

void FirmwareUploader::handleModelDisconnected(Generation generation)
{
    if (!isCurrent(generation)) {
        return;
    }
    // A command-channel drop after the bytes landed is the signature of the
    // radio rebooting to apply the image — but it is equally the signature of
    // it falling over, so it confirms nothing either way. Report it as its own
    // outcome instead of collapsing it into success or failure (#5572 review).
    finishOperation(generation,
                    m_waitingForConfirmation ? Outcome::Unconfirmed : Outcome::Failed,
                    m_waitingForConfirmation
                        ? tr("Radio disconnected after the firmware bytes were sent — it is most likely "
                             "rebooting to apply the image. Reconnect to confirm the firmware version.")
                        : tr("Radio disconnected before the firmware transfer completed"));
}

void FirmwareUploader::armTimeout(Generation generation, int timeoutMs, const QString& message)
{
    if (!isCurrent(generation)) {
        return;
    }
    if (m_timeoutToken == std::numeric_limits<quint64>::max()) {
        m_timeoutToken = 1;
    } else {
        ++m_timeoutToken;
    }
    m_timeoutGeneration = generation;
    m_timeoutMessage = message;
    m_timeoutTimer.start(timeoutMs);
}

void FirmwareUploader::onTimeout(Generation generation,
                                  quint64 timeoutToken,
                                  const QString& message)
{
    if (!isCurrent(generation) || timeoutToken != m_timeoutToken) {
        return;
    }
    if (m_socket
        && m_socket->state() != QAbstractSocket::ConnectedState
        && m_bytesQueued == 0
        && m_uploadPort != kFallbackPort) {
        tryFallbackPort(generation);
        return;
    }
    // Only the post-drain wait is ambiguous; every earlier timeout means the
    // transfer demonstrably did not complete.
    finishOperation(generation,
                    m_waitingForConfirmation ? Outcome::Unconfirmed : Outcome::Failed,
                    message);
}

void FirmwareUploader::armOverallTimeout(Generation generation)
{
    if (!isCurrent(generation)) {
        return;
    }
    if (m_overallTimeoutToken == std::numeric_limits<quint64>::max()) {
        m_overallTimeoutToken = 1;
    } else {
        ++m_overallTimeoutToken;
    }
    m_overallTimeoutGeneration = generation;
    m_overallTimeoutTimer.start(overallTimeoutMsFor(m_fileData.size()));
}

int FirmwareUploader::overallTimeoutMsFor(qint64 fileBytes) const
{
    // Ten minutes is the floor, not the value: a 386 MB image (the #5572 case)
    // needs ~644 KB/s to fit in ten minutes, which no SmartLink/WAN uplink owes
    // us. Allow the image's size at a pessimistic floor rate, then cap it so a
    // wedged transfer still cannot run forever.
    const qint64 sizeAllowanceMs =
        (fileBytes * 1000LL) / kMinExpectedBytesPerSec + kConfirmationTimeoutMs;
    const qint64 budget = qMax<qint64>(kOverallUploadBaseMs, sizeAllowanceMs);
    return static_cast<int>(qMin<qint64>(budget, kOverallUploadCeilingMs));
}

void FirmwareUploader::onOverallTimeout(Generation generation, quint64 timeoutToken)
{
    if (!isCurrent(generation) || timeoutToken != m_overallTimeoutToken) {
        return;
    }
    finishOperation(generation,
                    m_waitingForConfirmation ? Outcome::Unconfirmed : Outcome::Failed,
                    tr("Firmware upload exceeded its %1-minute operation limit")
                        .arg(overallTimeoutMsFor(m_fileData.size()) / 60000));
}

void FirmwareUploader::finishOperation(Generation generation,
                                       Outcome outcome,
                                       const QString& message)
{
    if (!isCurrent(generation)) {
        return;
    }
    m_timeoutTimer.stop();
    m_overallTimeoutTimer.stop();
    destroySocket(!m_waitingForConfirmation);
    disconnectStatusRelay();
    m_writer = {};
    m_fileData.clear();
    m_fileName.clear();
    m_bytesQueued = 0;
    m_bytesAcknowledged = 0;
    m_pendingBytes = 0;
    m_uploadPort = 0;
    m_waitingForConfirmation = false;
    m_radioProgressSeen = false;
    m_radioProgressPercent = 0;
    m_radioProgressAge.invalidate();
    m_uploading = false;
    m_generation = nextGeneration();
    ++m_timeoutToken;
    ++m_overallTimeoutToken;
    // Release the barrier only for an outcome the RADIO settled (`file update
    // failed=`, either value): that leaves nothing pending to misattribute to a
    // later attempt. Every other terminal — socket error, byte-accounting
    // failure, cancel, any timeout — can still have dispatched bytes whose fate
    // the radio never reported, which is precisely what the barrier is for.
    if (m_outcomeSettledByRadio) {
        m_requiresFreshConnection = false;
        if (m_model) {
            m_model->clearFirmwareRetryBlock();
        }
    }
    m_outcomeSettledByRadio = false;
    emit finished(outcome, message);
}

void FirmwareUploader::destroySocket(bool abortConnection)
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

void FirmwareUploader::disconnectStatusRelay()
{
    if (m_statusConnection) {
        QObject::disconnect(m_statusConnection);
        m_statusConnection = {};
    }
}

bool FirmwareUploader::isCurrent(Generation generation) const
{
    return m_uploading && generation == m_generation;
}

FirmwareUploader::Generation FirmwareUploader::nextGeneration()
{
    if (m_generation == std::numeric_limits<Generation>::max()) {
        return 1;
    }
    return m_generation + 1;
}

} // namespace AetherSDR
