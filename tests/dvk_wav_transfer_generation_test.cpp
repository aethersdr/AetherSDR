#include "core/DvkWavTransfer.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QPointer>
#include <QTcpSocket>
#include <QTimer>

#include <functional>
#include <iostream>

namespace AetherSDR {

// The seam invokes production terminal and deferred-connect handlers with
// inert QTcpSocket instances. No socket is connected or made to listen.
class DvkWavTransferTestAccess {
public:
    using Generation = DvkWavTransfer::Generation;
    using Direction = DvkWavTransfer::Direction;
    using Phase = DvkWavTransfer::Phase;

    static Generation beginDownload(DvkWavTransfer& transfer)
    {
        transfer.m_slotId = 1;
        transfer.m_bytesReceived = 0;
        return transfer.beginOperation(Direction::Download);
    }

    static Generation beginUpload(DvkWavTransfer& transfer, QByteArray data = QByteArray("test"))
    {
        transfer.m_slotId = 2;
        transfer.m_uploadData = std::move(data);
        transfer.m_bytesSent = 0;
        return transfer.beginOperation(Direction::Upload);
    }

    static QTcpSocket* installSocket(DvkWavTransfer& transfer, Phase phase)
    {
        auto* socket = new QTcpSocket(&transfer);
        transfer.m_client = socket;
        transfer.m_phase = phase;
        return socket;
    }

    static void replaceSocket(DvkWavTransfer& transfer, QTcpSocket* socket)
    {
        transfer.m_client = socket;
    }

    static void setPhase(DvkWavTransfer& transfer, Phase phase)
    {
        transfer.m_phase = phase;
    }

    static void setBytesReceived(DvkWavTransfer& transfer, qint64 bytes)
    {
        transfer.m_bytesReceived = bytes;
    }

    static void setBytesSent(DvkWavTransfer& transfer, qint64 bytes)
    {
        transfer.m_bytesSent = bytes;
    }

    static void downloadFinished(DvkWavTransfer& transfer, Generation generation, QTcpSocket* socket)
    {
        transfer.onDownloadFinished(generation, socket);
    }

    static void downloadError(DvkWavTransfer& transfer, Generation generation, QTcpSocket* socket)
    {
        transfer.onDownloadError(generation, socket);
    }

    static void uploadError(DvkWavTransfer& transfer, Generation generation, QTcpSocket* socket)
    {
        transfer.onUploadError(generation, socket);
    }

    static void uploadBytesWritten(DvkWavTransfer& transfer, Generation generation,
                                   QTcpSocket* socket, qint64 bytes)
    {
        transfer.onUploadBytesWritten(generation, socket, bytes);
    }

    static void uploadPort(DvkWavTransfer& transfer, Generation generation, int code)
    {
        transfer.onUploadPortReceived(generation, code, QStringLiteral("4995"));
    }

    static DvkWavTransfer::PortResponseCallback portCallback(DvkWavTransfer& transfer,
                                                              Generation generation,
                                                              Direction direction)
    {
        return transfer.makePortResponseCallback(generation, direction);
    }

    static DvkWavTransfer::DeferredConnectCallback deferredConnect(DvkWavTransfer& transfer,
                                                                     Generation generation,
                                                                     QTcpSocket* socket,
                                                                     quint16 port, int& calls,
                                                                     QTcpSocket*& observedSocket,
                                                                     quint16& observedPort)
    {
        return transfer.makeUploadConnectTimerCallback(
            generation, socket, port,
            [&calls, &observedSocket, &observedPort](QTcpSocket* candidate, quint16 candidatePort) {
                ++calls;
                observedSocket = candidate;
                observedPort = candidatePort;
            });
    }

    static Phase phase(const DvkWavTransfer& transfer) { return transfer.m_phase; }
    static qint64 bytesSent(const DvkWavTransfer& transfer) { return transfer.m_bytesSent; }
    static QFile* file(const DvkWavTransfer& transfer) { return transfer.m_file; }
};

} // namespace AetherSDR

namespace {

int g_failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void checkStaleDownloadCallbacksCannotSettleReplacement(bool replaceWithUpload)
{
    AetherSDR::DvkWavTransfer transfer(nullptr);
    int completions = 0;
    QObject::connect(&transfer, &AetherSDR::DvkWavTransfer::finished,
                     [&completions](bool, const QString&) { ++completions; });

    const auto oldGeneration = AetherSDR::DvkWavTransferTestAccess::beginDownload(transfer);
    const auto oldPortResponse = AetherSDR::DvkWavTransferTestAccess::portCallback(
        transfer, oldGeneration, AetherSDR::DvkWavTransferTestAccess::Direction::Download);
    QTcpSocket* oldSocket = AetherSDR::DvkWavTransferTestAccess::installSocket(
        transfer, AetherSDR::DvkWavTransferTestAccess::Phase::ReceivingDownload);
    AetherSDR::DvkWavTransferTestAccess::setBytesReceived(transfer, 16);
    transfer.cancel();
    const auto replacement = replaceWithUpload
        ? AetherSDR::DvkWavTransferTestAccess::beginUpload(transfer)
        : AetherSDR::DvkWavTransferTestAccess::beginDownload(transfer);

    oldPortResponse(1, QStringLiteral("old rejection"));
    oldPortResponse(0, QStringLiteral("4995"));
    AetherSDR::DvkWavTransferTestAccess::downloadFinished(transfer, oldGeneration, oldSocket);
    AetherSDR::DvkWavTransferTestAccess::downloadError(transfer, oldGeneration, oldSocket);

    check(completions == 1, "old download port, success, and error callbacks cannot finish the replacement");
    check(transfer.isTransferring()
              && AetherSDR::DvkWavTransferTestAccess::phase(transfer)
                     == (replaceWithUpload
                             ? AetherSDR::DvkWavTransferTestAccess::Phase::WaitingForUploadPort
                             : AetherSDR::DvkWavTransferTestAccess::Phase::WaitingForDownloadPort),
          "stale download callbacks leave the replacement waiting for its own port");
    check(AetherSDR::DvkWavTransferTestAccess::file(transfer) == nullptr,
          "a stale download port reply cannot allocate a replacement output file");
    check(replacement != oldGeneration, "cancel advances the download operation generation");
}

void checkStaleUploadCallbacksCannotSettleReplacement(bool replaceWithUpload)
{
    AetherSDR::DvkWavTransfer transfer(nullptr);
    int completions = 0;
    QObject::connect(&transfer, &AetherSDR::DvkWavTransfer::finished,
                     [&completions](bool, const QString&) { ++completions; });

    const auto oldGeneration = AetherSDR::DvkWavTransferTestAccess::beginUpload(transfer);
    const auto oldPortResponse = AetherSDR::DvkWavTransferTestAccess::portCallback(
        transfer, oldGeneration, AetherSDR::DvkWavTransferTestAccess::Direction::Upload);
    QTcpSocket* oldSocket = AetherSDR::DvkWavTransferTestAccess::installSocket(
        transfer, AetherSDR::DvkWavTransferTestAccess::Phase::SendingUpload);
    transfer.cancel();
    const auto replacement = replaceWithUpload
        ? AetherSDR::DvkWavTransferTestAccess::beginUpload(transfer)
        : AetherSDR::DvkWavTransferTestAccess::beginDownload(transfer);

    oldPortResponse(1, QStringLiteral("old rejection"));
    oldPortResponse(0, QStringLiteral("4995"));
    AetherSDR::DvkWavTransferTestAccess::uploadError(transfer, oldGeneration, oldSocket);
    AetherSDR::DvkWavTransferTestAccess::uploadBytesWritten(transfer, oldGeneration, oldSocket, 4);

    check(completions == 1, "old upload port, error, and success callbacks cannot finish the replacement");
    check(transfer.isTransferring()
              && AetherSDR::DvkWavTransferTestAccess::phase(transfer)
                     == (replaceWithUpload
                             ? AetherSDR::DvkWavTransferTestAccess::Phase::WaitingForUploadPort
                             : AetherSDR::DvkWavTransferTestAccess::Phase::WaitingForDownloadPort),
          "stale upload callbacks leave the replacement waiting for its own port");
    check(replacement != oldGeneration, "cancel advances the upload operation generation");
}

void checkCurrentCallbacksPreserveTerminalBehavior()
{
    {
        AetherSDR::DvkWavTransfer transfer(nullptr);
        int success = 0;
        QObject::connect(&transfer, &AetherSDR::DvkWavTransfer::finished,
                         [&success](bool completed, const QString&) { success += completed ? 1 : 0; });
        const auto generation = AetherSDR::DvkWavTransferTestAccess::beginDownload(transfer);
        QTcpSocket* socket = AetherSDR::DvkWavTransferTestAccess::installSocket(
            transfer, AetherSDR::DvkWavTransferTestAccess::Phase::ReceivingDownload);
        AetherSDR::DvkWavTransferTestAccess::setBytesReceived(transfer, 8);
        AetherSDR::DvkWavTransferTestAccess::downloadFinished(transfer, generation, socket);
        check(success == 1 && !transfer.isTransferring(),
              "the current download completion remains successful");
    }

    {
        AetherSDR::DvkWavTransfer transfer(nullptr);
        int success = 0;
        QObject::connect(&transfer, &AetherSDR::DvkWavTransfer::finished,
                         [&success](bool completed, const QString&) { success += completed ? 1 : 0; });
        const auto generation = AetherSDR::DvkWavTransferTestAccess::beginUpload(
            transfer, QByteArray("data"));
        QTcpSocket* socket = AetherSDR::DvkWavTransferTestAccess::installSocket(
            transfer, AetherSDR::DvkWavTransferTestAccess::Phase::SendingUpload);
        AetherSDR::DvkWavTransferTestAccess::uploadBytesWritten(transfer, generation, socket, 4);
        check(success == 1 && !transfer.isTransferring(),
              "the current upload completion remains successful");
    }
}

void checkPhaseAndSocketIdentityRejectDuplicates()
{
    AetherSDR::DvkWavTransfer transfer(nullptr);
    int completions = 0;
    QObject::connect(&transfer, &AetherSDR::DvkWavTransfer::finished,
                     [&completions](bool, const QString&) { ++completions; });

    const auto generation = AetherSDR::DvkWavTransferTestAccess::beginUpload(transfer);
    QTcpSocket* socket = AetherSDR::DvkWavTransferTestAccess::installSocket(
        transfer, AetherSDR::DvkWavTransferTestAccess::Phase::WaitingForUploadConnection);
    AetherSDR::DvkWavTransferTestAccess::uploadPort(transfer, generation, 1);
    AetherSDR::DvkWavTransferTestAccess::setPhase(
        transfer, AetherSDR::DvkWavTransferTestAccess::Phase::WaitingForUploadPort);
    AetherSDR::DvkWavTransferTestAccess::uploadError(transfer, generation, socket);

    check(completions == 0 && transfer.isTransferring(),
          "a duplicate port reply and wrong-phase error do not settle an active upload");

    AetherSDR::DvkWavTransferTestAccess::setPhase(
        transfer, AetherSDR::DvkWavTransferTestAccess::Phase::WaitingForUploadConnection);
    int calls = 0;
    QTcpSocket* observedSocket = nullptr;
    quint16 observedPort = 0;
    auto* replacementSocket = new QTcpSocket(&transfer);
    AetherSDR::DvkWavTransferTestAccess::replaceSocket(transfer, replacementSocket);
    const auto oldConnect = AetherSDR::DvkWavTransferTestAccess::deferredConnect(
        transfer, generation, socket, 4995, calls, observedSocket, observedPort);
    const auto replacementConnect = AetherSDR::DvkWavTransferTestAccess::deferredConnect(
        transfer, generation, replacementSocket, 4996, calls, observedSocket, observedPort);
    oldConnect();
    replacementConnect();

    check(calls == 1,
          "the deferred timer rejects an old socket even when generation and phase match");
    check(observedSocket == replacementSocket && observedPort == 4996,
          "the deferred timer acts only on the current socket and its port");
}

void checkDelayedTimerCannotConnectReplacement()
{
    AetherSDR::DvkWavTransfer transfer(nullptr);
    int calls = 0;
    QTcpSocket* observedSocket = nullptr;
    quint16 observedPort = 0;
    const auto oldGeneration = AetherSDR::DvkWavTransferTestAccess::beginUpload(transfer);
    QTcpSocket* oldSocket = AetherSDR::DvkWavTransferTestAccess::installSocket(
        transfer, AetherSDR::DvkWavTransferTestAccess::Phase::WaitingForUploadConnection);
    const auto delayedConnect = AetherSDR::DvkWavTransferTestAccess::deferredConnect(
        transfer, oldGeneration, oldSocket, 4995, calls, observedSocket, observedPort);

    // This schedules the same 200 ms callback shape production uses, with the
    // injected connector preventing any network activity.
    QEventLoop wait;
    bool timerDelivered = false;
    QTimer::singleShot(200, &transfer, [&wait, &timerDelivered, delayedConnect] {
        timerDelivered = true;
        delayedConnect();
        wait.quit();
    });
    transfer.cancel();
    AetherSDR::DvkWavTransferTestAccess::beginDownload(transfer);
    QTimer::singleShot(1000, &wait, &QEventLoop::quit);
    wait.exec();

    check(timerDelivered && calls == 0 && observedSocket == nullptr && observedPort == 0,
          "a cancelled 200 ms upload timer cannot connect a replacement operation");
}

void checkOldUploadTimerRejectsSameSocketReplacement()
{
    AetherSDR::DvkWavTransfer transfer(nullptr);
    int calls = 0;
    QTcpSocket* observedSocket = nullptr;
    quint16 observedPort = 0;
    const auto oldGeneration = AetherSDR::DvkWavTransferTestAccess::beginUpload(transfer);
    QTcpSocket* socket = AetherSDR::DvkWavTransferTestAccess::installSocket(
        transfer, AetherSDR::DvkWavTransferTestAccess::Phase::WaitingForUploadConnection);
    const auto oldTimer = AetherSDR::DvkWavTransferTestAccess::deferredConnect(
        transfer, oldGeneration, socket, 4995, calls, observedSocket, observedPort);

    transfer.cancel();
    const auto replacement = AetherSDR::DvkWavTransferTestAccess::beginUpload(transfer);
    // deleteLater() has not run yet, so reusing this inert socket makes the
    // generation guard independently observable from the socket-identity guard.
    AetherSDR::DvkWavTransferTestAccess::replaceSocket(transfer, socket);
    AetherSDR::DvkWavTransferTestAccess::setPhase(
        transfer, AetherSDR::DvkWavTransferTestAccess::Phase::WaitingForUploadConnection);
    oldTimer();

    const auto currentTimer = AetherSDR::DvkWavTransferTestAccess::deferredConnect(
        transfer, replacement, socket, 4996, calls, observedSocket, observedPort);
    currentTimer();

    check(calls == 1 && observedSocket == socket && observedPort == 4996,
          "an old upload timer rejects the same socket while the current timer still connects it");
}

void checkStatusReentrancyCannotMutateReplacement()
{
    AetherSDR::DvkWavTransfer transfer(nullptr);
    bool restarted = false;
    AetherSDR::DvkWavTransferTestAccess::Generation replacement = 0;
    QObject::connect(&transfer, &AetherSDR::DvkWavTransfer::statusChanged,
                     [&transfer, &restarted, &replacement](const QString& status) {
                         if (restarted || !status.startsWith(QStringLiteral("Uploading to slot"))) {
                             return;
                         }
                         restarted = true;
                         transfer.cancel();
                         replacement = AetherSDR::DvkWavTransferTestAccess::beginUpload(
                             transfer, QByteArray("replacement"));
                     });

    const auto oldGeneration = AetherSDR::DvkWavTransferTestAccess::beginUpload(
        transfer, QByteArray("old"));
    QTcpSocket* socket = AetherSDR::DvkWavTransferTestAccess::installSocket(
        transfer, AetherSDR::DvkWavTransferTestAccess::Phase::SendingUpload);
    AetherSDR::DvkWavTransferTestAccess::uploadBytesWritten(transfer, oldGeneration, socket, 1);

    check(restarted && transfer.isTransferring() && replacement != oldGeneration,
          "statusChanged cancellation can begin an independent replacement");
    check(AetherSDR::DvkWavTransferTestAccess::phase(transfer)
              == AetherSDR::DvkWavTransferTestAccess::Phase::WaitingForUploadPort
              && AetherSDR::DvkWavTransferTestAccess::bytesSent(transfer) == 0,
          "the old bytes-written continuation cannot mutate the replacement");
}

void checkQPointerOwnerGuardSurvivesDestruction()
{
    auto* transfer = new AetherSDR::DvkWavTransfer(nullptr);
    const auto callback = AetherSDR::DvkWavTransferTestAccess::portCallback(
        *transfer, 1, AetherSDR::DvkWavTransferTestAccess::Direction::Upload);
    QPointer<AetherSDR::DvkWavTransfer> owner = transfer;
    delete owner;
    callback(0, QStringLiteral("4995"));
    check(owner.isNull(), "a retained production command callback is harmless after transfer destruction");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    checkStaleDownloadCallbacksCannotSettleReplacement(false);
    checkStaleDownloadCallbacksCannotSettleReplacement(true);
    checkStaleUploadCallbacksCannotSettleReplacement(false);
    checkStaleUploadCallbacksCannotSettleReplacement(true);
    checkCurrentCallbacksPreserveTerminalBehavior();
    checkPhaseAndSocketIdentityRejectDuplicates();
    checkDelayedTimerCannotConnectReplacement();
    checkOldUploadTimerRejectsSameSocketReplacement();
    checkStatusReentrancyCannotMutateReplacement();
    checkQPointerOwnerGuardSurvivesDestruction();
    return g_failures == 0 ? 0 : 1;
}
