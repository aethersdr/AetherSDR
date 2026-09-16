#include <QCoreApplication>
#include <QEventLoop>
#include <QTcpSocket>
#include <QTimer>

#include "core/ProfileTransfer.h"

// Socket-free by construction. Every fixture is ProfileTransfer(nullptr), and
// that null model is load-bearing: connectUploadSocket() does build a real
// QTcpSocket, but its deferred connect action is gated on the model, so
// connectToHost() is unreachable and no descriptor is ever opened. Give a
// fixture a RadioModel and this test starts doing real network I/O.

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

    static void completeImport(ProfileTransfer& transfer)
    {
        transfer.completeImport();
    }

    static void startCommandTimeout(ProfileTransfer& transfer, int timeoutMs, Phase phase)
    {
        transfer.startCommandTimeout(timeoutMs, phase);
    }

    static void startIdleTimeout(ProfileTransfer& transfer, Phase phase)
    {
        transfer.startIdleTimeout(phase);
    }

    static bool anyTimeoutArmed(const ProfileTransfer& transfer)
    {
        return transfer.m_timeout->isActive() || transfer.m_idleTimer->isActive()
            || transfer.m_overallTimer->isActive();
    }

    static bool anyTimeoutGenerationLive(const ProfileTransfer& transfer)
    {
        return transfer.m_commandTimeoutGeneration != 0
            || transfer.m_idleTimeoutGeneration != 0
            || transfer.m_overallTimeoutGeneration != 0;
    }

    // Read the production constants rather than restating them: a raised
    // settle time would otherwise make every wait below expire early and the
    // assertions pass vacuously.
    static constexpr int importSettleMs() { return ProfileTransfer::kImportSettleMs; }
    static constexpr int metaSubsetSettleMs() { return ProfileTransfer::kMetaSubsetSettleMs; }
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
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

// Long enough for a settle callback to have fired if it were going to.
int pastSettle(int settleMs)
{
    return settleMs + 600;
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

        waitForMilliseconds(pastSettle(AetherSDR::ProfileTransferTestAccess::importSettleMs()));

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
        QTimer::singleShot(AetherSDR::ProfileTransferTestAccess::metaSubsetSettleMs(),
                           &transfer, staleMetadata);
        transfer.cancel();
        AetherSDR::ProfileTransferTestAccess::begin(
            transfer, AetherSDR::ProfileTransfer::Operation::ImportDatabase,
            AetherSDR::ProfileTransferTestAccess::Phase::UploadImport);

        waitForMilliseconds(
            pastSettle(AetherSDR::ProfileTransferTestAccess::metaSubsetSettleMs()));

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

        waitForMilliseconds(pastSettle(AetherSDR::ProfileTransferTestAccess::importSettleMs()));

        ok &= expect(finished == 1, "current import completion emits finished once");
        ok &= expect(!transfer.isBusy(), "current import completion returns the transfer to idle");
    }

    {
        // The three shared QTimers are generation-bound like everything else,
        // but the reason a stale one cannot fire at all is that cleanup()
        // disarms them on every terminal path. That invariant is what this
        // pins -- it is what would actually break, and it is what keeps the
        // generation check on those timers unreachable rather than dead.
        AetherSDR::ProfileTransfer transfer(nullptr);
        AetherSDR::ProfileTransferTestAccess::begin(
            transfer, AetherSDR::ProfileTransfer::Operation::ImportDatabase,
            AetherSDR::ProfileTransferTestAccess::Phase::UploadImport);
        AetherSDR::ProfileTransferTestAccess::startCommandTimeout(
            transfer, 50, AetherSDR::ProfileTransferTestAccess::Phase::UploadImport);
        AetherSDR::ProfileTransferTestAccess::startIdleTimeout(
            transfer, AetherSDR::ProfileTransferTestAccess::Phase::UploadImport);
        ok &= expect(AetherSDR::ProfileTransferTestAccess::anyTimeoutArmed(transfer),
                     "a running operation arms its timeouts");

        transfer.cancel();
        ok &= expect(!AetherSDR::ProfileTransferTestAccess::anyTimeoutArmed(transfer),
                     "cancel disarms every shared timeout");
        ok &= expect(!AetherSDR::ProfileTransferTestAccess::anyTimeoutGenerationLive(transfer),
                     "cancel retires every armed timeout generation");

        AetherSDR::ProfileTransferTestAccess::begin(
            transfer, AetherSDR::ProfileTransfer::Operation::ImportDatabase,
            AetherSDR::ProfileTransferTestAccess::Phase::UploadImport);
        waitForMilliseconds(300);
        ok &= expect(transfer.isBusy(),
                     "a cancelled operation's command timeout cannot fail its replacement");
        ok &= expect(AetherSDR::ProfileTransferTestAccess::socket(transfer) == nullptr,
                     "a cancelled operation's command timeout cannot open a fallback socket "
                     "on its replacement");
    }

    {
        // A progress slot that cancels and begins a same-phase replacement
        // import must not let the old completion finish the replacement. This
        // is the only thing pinning completeImport()'s post-emit re-checks;
        // with them removed, every other assertion in this file still passes.
        // Calls completeImport() directly, so it adds no wall-clock.
        AetherSDR::ProfileTransfer transfer(nullptr);
        int finished = 0;
        QObject::connect(&transfer, &AetherSDR::ProfileTransfer::finished, &transfer,
                         [&finished](AetherSDR::ProfileTransfer::Operation, const QString&) {
                             ++finished;
                         });
        AetherSDR::ProfileTransferTestAccess::begin(
            transfer, AetherSDR::ProfileTransfer::Operation::ImportDatabase,
            AetherSDR::ProfileTransferTestAccess::Phase::WaitingForImport);
        bool restarted = false;
        QObject::connect(&transfer, &AetherSDR::ProfileTransfer::progress, &transfer,
                         [&transfer, &restarted](const QString& status) {
            if (!restarted && status.startsWith(QStringLiteral("Import complete."))) {
                restarted = true;
                transfer.cancel();
                AetherSDR::ProfileTransferTestAccess::begin(
                    transfer, AetherSDR::ProfileTransfer::Operation::ImportDatabase,
                    AetherSDR::ProfileTransferTestAccess::Phase::WaitingForImport);
            }
        });
        AetherSDR::ProfileTransferTestAccess::completeImport(transfer);
        ok &= expect(restarted, "the progress slot restarted the import during completion");
        ok &= expect(finished == 0,
                     "an import restarted from a progress slot is not finished by the old completion");
        ok &= expect(transfer.isBusy(), "the replacement import remains active");
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
