#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

#include "core/ProfileTransfer.h"

#include <functional>
#include <iostream>
#include <memory>

namespace AetherSDR {

class ProfileTransferTestAccess {
public:
    using Phase = ProfileTransfer::Phase;

    static void begin(ProfileTransfer& transfer, ProfileTransfer::Operation operation,
                      ProfileTransfer::Phase phase)
    {
        transfer.begin(operation, phase);
    }

    static std::function<void(int, const QString&)> armUploadPortReply(ProfileTransfer& transfer)
    {
        const quint64 requestId = transfer.nextAsyncId();
        transfer.m_uploadPortRequestId = requestId;
        return transfer.makeUploadPortCallback(transfer.m_operationGeneration, transfer.m_phase,
                                               requestId);
    }

    static std::function<void(int, const QString&)> armDownloadPortReply(ProfileTransfer& transfer)
    {
        const quint64 requestId = transfer.nextAsyncId();
        transfer.m_downloadPortRequestId = requestId;
        return transfer.makeDownloadPortCallback(transfer.m_operationGeneration, transfer.m_phase,
                                                 requestId);
    }

    static QTcpSocket* socket(const ProfileTransfer& transfer)
    {
        return transfer.m_socket;
    }

    static quint16 uploadPort(const ProfileTransfer& transfer)
    {
        return transfer.m_uploadPort;
    }

    static void connectUploadSocket(ProfileTransfer& transfer, quint16 port)
    {
        transfer.connectUploadSocket(port);
    }

    static void tryFallbackUploadPort(ProfileTransfer& transfer)
    {
        transfer.tryFallbackUploadPort();
    }

    static void setUploadPort(ProfileTransfer& transfer, quint16 port)
    {
        transfer.m_uploadPort = port;
    }

    static std::function<void()> makeUploadConnectCallback(ProfileTransfer& transfer,
                                                            QTcpSocket* socket, int& attempts)
    {
        return transfer.makeUploadConnectCallback(
            transfer.m_operationGeneration, transfer.m_phase, socket,
            [&attempts] { ++attempts; });
    }

    static std::function<void()> makeMetadataSettleCallback(ProfileTransfer& transfer,
                                                            int& completions)
    {
        return transfer.makeMetadataSettleCallback(
            transfer.m_operationGeneration, transfer.m_phase,
            [&completions] { ++completions; });
    }

    static void scheduleImportCompletion(ProfileTransfer& transfer)
    {
        transfer.scheduleImportCompletion();
    }

    static void setPhase(ProfileTransfer& transfer, Phase phase)
    {
        transfer.m_phase = phase;
    }

    static bool isInPhase(const ProfileTransfer& transfer, ProfileTransfer::Phase phase)
    {
        return transfer.m_phase == phase;
    }
};

} // namespace AetherSDR

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

void waitForMilliseconds(int milliseconds)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(5);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    bool ok = true;

    {
        // This is the exact callback factory handed to RadioModel. A current
        // reply must still advance the production port-reply path.
        AetherSDR::ProfileTransfer transfer(nullptr);
        AetherSDR::ProfileTransferTestAccess::begin(
            transfer, AetherSDR::ProfileTransfer::Operation::ImportDatabase,
            AetherSDR::ProfileTransferTestAccess::Phase::UploadImport);
        std::function<void(int, const QString&)> reply =
            AetherSDR::ProfileTransferTestAccess::armUploadPortReply(transfer);
        reply(0, QStringLiteral("port=42607"));

        ok &= expect(AetherSDR::ProfileTransferTestAccess::socket(transfer) != nullptr,
                     "current upload-port reply creates the upload socket");
        ok &= expect(AetherSDR::ProfileTransferTestAccess::uploadPort(transfer) == 42607,
                     "current upload-port reply retains its selected port");
    }

    {
        // The timeout fallback retires the still-pending primary-port reply.
        AetherSDR::ProfileTransfer transfer(nullptr);
        AetherSDR::ProfileTransferTestAccess::begin(
            transfer, AetherSDR::ProfileTransfer::Operation::ImportDatabase,
            AetherSDR::ProfileTransferTestAccess::Phase::UploadImport);
        std::function<void(int, const QString&)> staleReply =
            AetherSDR::ProfileTransferTestAccess::armUploadPortReply(transfer);
        AetherSDR::ProfileTransferTestAccess::setUploadPort(transfer, 4995);
        AetherSDR::ProfileTransferTestAccess::tryFallbackUploadPort(transfer);
        QTcpSocket* fallbackSocket = AetherSDR::ProfileTransferTestAccess::socket(transfer);

        staleReply(0, QStringLiteral("4995"));
        ok &= expect(AetherSDR::ProfileTransferTestAccess::socket(transfer) == fallbackSocket,
                     "stale same-phase upload-port reply cannot replace the fallback socket");
        ok &= expect(AetherSDR::ProfileTransferTestAccess::uploadPort(transfer) == 42607,
                     "stale same-phase upload-port reply cannot restore the primary port");

    }

    {
        // The original 200 ms delayed connect must remain tied to the primary
        // socket instead of using m_socket after a same-phase replacement.
        AetherSDR::ProfileTransfer transfer(nullptr);
        AetherSDR::ProfileTransferTestAccess::begin(
            transfer, AetherSDR::ProfileTransfer::Operation::ImportDatabase,
            AetherSDR::ProfileTransferTestAccess::Phase::UploadImport);
        AetherSDR::ProfileTransferTestAccess::connectUploadSocket(transfer, 4995);
        QTcpSocket* primarySocket = AetherSDR::ProfileTransferTestAccess::socket(transfer);
        int connectAttempts = 0;
        std::function<void()> staleConnect =
            AetherSDR::ProfileTransferTestAccess::makeUploadConnectCallback(
                transfer, primarySocket, connectAttempts);
        QTimer::singleShot(200, &transfer, staleConnect);
        AetherSDR::ProfileTransferTestAccess::tryFallbackUploadPort(transfer);
        QTcpSocket* fallbackSocket = AetherSDR::ProfileTransferTestAccess::socket(transfer);

        // Deliver before deferred deletion too: the old socket is still alive,
        // so this assertion specifically needs identity, not just QPointer.
        staleConnect();
        ok &= expect(connectAttempts == 0,
                     "live retired socket cannot invoke its delayed connection action");
        waitForMilliseconds(250);
        ok &= expect(AetherSDR::ProfileTransferTestAccess::socket(transfer) == fallbackSocket,
                     "stale delayed upload connect cannot use the replacement socket");
        ok &= expect(connectAttempts == 0,
                     "stale delayed upload connect cannot invoke its connection action");

        std::function<void()> currentConnect =
            AetherSDR::ProfileTransferTestAccess::makeUploadConnectCallback(
                transfer, fallbackSocket, connectAttempts);
        currentConnect();
        ok &= expect(connectAttempts == 1,
                     "current delayed upload connect invokes its connection action");
    }

    {
        // Download replies use an independent request ID too. An error reply
        // makes this socket-free: handling it would terminate the transfer.
        AetherSDR::ProfileTransfer transfer(nullptr);
        AetherSDR::ProfileTransferTestAccess::begin(
            transfer, AetherSDR::ProfileTransfer::Operation::ExportDatabase,
            AetherSDR::ProfileTransferTestAccess::Phase::DownloadPackage);
        std::function<void(int, const QString&)> staleReply =
            AetherSDR::ProfileTransferTestAccess::armDownloadPortReply(transfer);
        std::function<void(int, const QString&)> currentReply =
            AetherSDR::ProfileTransferTestAccess::armDownloadPortReply(transfer);
        Q_UNUSED(currentReply)
        staleReply(1, QString());
        ok &= expect(transfer.isBusy(),
                     "stale download-port reply cannot terminate the current export");
    }

    for (const bool replaceWithExport : {false, true}) {
        // Preserve the real five-second completion callback while canceling
        // and beginning a replacement import in the same phase.
        AetherSDR::ProfileTransfer transfer(nullptr);
        int finished = 0;
        QObject::connect(&transfer, &AetherSDR::ProfileTransfer::finished,
                         [&finished](AetherSDR::ProfileTransfer::Operation, const QString&) {
                             ++finished;
                         });
        AetherSDR::ProfileTransferTestAccess::begin(
            transfer, AetherSDR::ProfileTransfer::Operation::ImportDatabase,
            AetherSDR::ProfileTransferTestAccess::Phase::WaitingForImport);
        AetherSDR::ProfileTransferTestAccess::scheduleImportCompletion(transfer);
        transfer.cancel();
        const auto replacementPhase = replaceWithExport
            ? AetherSDR::ProfileTransferTestAccess::Phase::UploadMetaSubset
            : AetherSDR::ProfileTransferTestAccess::Phase::WaitingForImport;
        AetherSDR::ProfileTransferTestAccess::begin(
            transfer, replaceWithExport ? AetherSDR::ProfileTransfer::Operation::ExportDatabase
                                        : AetherSDR::ProfileTransfer::Operation::ImportDatabase,
            replacementPhase);

        waitForMilliseconds(5600);

        ok &= expect(finished == 0,
                     "the canceled import completion callback cannot finish a replacement");
        ok &= expect(transfer.isBusy(), "replacement remains active after stale import callback");
        ok &= expect(AetherSDR::ProfileTransferTestAccess::isInPhase(
                         transfer, replacementPhase),
                     "stale import completion callback cannot change the replacement phase");
    }

    {
        // The five-second metadata settle must not run after a replacement
        // import begins. The shared callback factory is the production timer's
        // actual guard, with an injected action for a socket-free assertion.
        AetherSDR::ProfileTransfer transfer(nullptr);
        AetherSDR::ProfileTransferTestAccess::begin(
            transfer, AetherSDR::ProfileTransfer::Operation::ExportDatabase,
            AetherSDR::ProfileTransferTestAccess::Phase::UploadMetaSubset);
        int metadataCompletions = 0;
        std::function<void()> staleMetadata =
            AetherSDR::ProfileTransferTestAccess::makeMetadataSettleCallback(
                transfer, metadataCompletions);
        // A phase transition alone must also retire a callback; generation is
        // deliberately unchanged for this assertion.
        AetherSDR::ProfileTransferTestAccess::setPhase(
            transfer, AetherSDR::ProfileTransferTestAccess::Phase::DownloadPackage);
        staleMetadata();
        ok &= expect(metadataCompletions == 0,
                     "metadata completion is refused after a same-operation phase transition");
        QTimer::singleShot(5000, &transfer, staleMetadata);
        transfer.cancel();
        AetherSDR::ProfileTransferTestAccess::begin(
            transfer, AetherSDR::ProfileTransfer::Operation::ImportDatabase,
            AetherSDR::ProfileTransferTestAccess::Phase::UploadImport);

        waitForMilliseconds(5600);

        ok &= expect(metadataCompletions == 0,
                     "stale metadata settle cannot invoke its completion action");
        ok &= expect(transfer.isBusy(),
                     "stale metadata settle cannot finish or replace the import");
        ok &= expect(AetherSDR::ProfileTransferTestAccess::isInPhase(
                         transfer, AetherSDR::ProfileTransferTestAccess::Phase::UploadImport),
                     "stale metadata settle cannot change the replacement phase");

        AetherSDR::ProfileTransfer currentTransfer(nullptr);
        AetherSDR::ProfileTransferTestAccess::begin(
            currentTransfer, AetherSDR::ProfileTransfer::Operation::ExportDatabase,
            AetherSDR::ProfileTransferTestAccess::Phase::UploadMetaSubset);
        int currentMetadataCompletions = 0;
        AetherSDR::ProfileTransferTestAccess::makeMetadataSettleCallback(
            currentTransfer, currentMetadataCompletions)();
        ok &= expect(currentMetadataCompletions == 1,
                     "current metadata settle invokes its completion action");
    }

    {
        // A current scheduled completion remains a positive import outcome.
        AetherSDR::ProfileTransfer transfer(nullptr);
        int finished = 0;
        QObject::connect(&transfer, &AetherSDR::ProfileTransfer::finished,
                         [&finished](AetherSDR::ProfileTransfer::Operation, const QString&) {
                             ++finished;
                         });
        AetherSDR::ProfileTransferTestAccess::begin(
            transfer, AetherSDR::ProfileTransfer::Operation::ImportDatabase,
            AetherSDR::ProfileTransferTestAccess::Phase::WaitingForImport);
        AetherSDR::ProfileTransferTestAccess::scheduleImportCompletion(transfer);

        waitForMilliseconds(5600);

        ok &= expect(finished == 1, "current import completion emits finished once");
        ok &= expect(!transfer.isBusy(), "current import completion returns the transfer to idle");
    }

    {
        // An old reply must also be rejected after cancel/restart in the same
        // phase, where a phase-only guard would still match.
        AetherSDR::ProfileTransfer transfer(nullptr);
        AetherSDR::ProfileTransferTestAccess::begin(
            transfer, AetherSDR::ProfileTransfer::Operation::ImportDatabase,
            AetherSDR::ProfileTransferTestAccess::Phase::UploadImport);
        std::function<void(int, const QString&)> staleReply =
            AetherSDR::ProfileTransferTestAccess::armUploadPortReply(transfer);
        transfer.cancel();
        AetherSDR::ProfileTransferTestAccess::begin(
            transfer, AetherSDR::ProfileTransfer::Operation::ImportDatabase,
            AetherSDR::ProfileTransferTestAccess::Phase::UploadImport);
        staleReply(1, QString());
        ok &= expect(transfer.isBusy(),
                     "canceled operation upload reply cannot fail same-phase replacement");
    }

    {
        // RadioModel owns these callbacks and can outlive ProfileTransfer.
        // A late response after the transfer is deleted must be a no-op.
        std::function<void(int, const QString&)> lateReply;
        {
            auto transfer = std::make_unique<AetherSDR::ProfileTransfer>(nullptr);
            AetherSDR::ProfileTransferTestAccess::begin(
                *transfer, AetherSDR::ProfileTransfer::Operation::ImportDatabase,
                AetherSDR::ProfileTransferTestAccess::Phase::UploadImport);
            lateReply = AetherSDR::ProfileTransferTestAccess::armUploadPortReply(*transfer);
            transfer.reset();
        }
        lateReply(0, QStringLiteral("4995"));
        // Reaching scope exit proves the weak callback did not dereference its destroyed owner.
    }

    return ok ? 0 : 1;
}
