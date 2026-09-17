// #5634 (sibling) — DvkWavTransfer carried the identical unversioned-callback
// shape ProfileTransfer did: every deferred continuation re-validated itself
// against the shared m_cancelled/m_finished/m_transferring flags, which
// download()/upload() reset. Cancel a transfer, start another, and the old
// reply or the old 200 ms connect timer acts on the replacement.
//
// Socket-free by construction. Every fixture is DvkWavTransfer(nullptr), and
// that null model is load-bearing: the upload path does build a real
// QTcpSocket, but its deferred connect action is gated on the model, so
// connectToHost() is unreachable and no descriptor is ever opened. Nothing
// here drives the download success path either, because that one legitimately
// calls QTcpServer::listen().
#include <QCoreApplication>
#include <QEventLoop>
#include <QTcpSocket>
#include <QTimer>

#include "core/DvkWavTransfer.h"

#include <functional>
#include <iostream>
#include <memory>

namespace AetherSDR {

class DvkWavTransferTestAccess {
public:
    using Direction = DvkWavTransfer::Direction;

    static quint64 begin(DvkWavTransfer& transfer, Direction direction, int slotId)
    {
        return transfer.begin(direction, slotId);
    }

    static std::function<void(int, const QString&)> armUploadPortReply(DvkWavTransfer& transfer)
    {
        const quint64 requestId = transfer.nextAsyncId();
        transfer.m_portRequestId = requestId;
        return transfer.makeUploadPortCallback(transfer.m_operationGeneration, requestId);
    }

    static std::function<void(int, const QString&)> armDownloadPortReply(DvkWavTransfer& transfer)
    {
        const quint64 requestId = transfer.nextAsyncId();
        transfer.m_portRequestId = requestId;
        return transfer.makeDownloadPortCallback(transfer.m_operationGeneration, requestId);
    }

    static std::function<void()> makeUploadConnectCallback(DvkWavTransfer& transfer,
                                                           QTcpSocket* socket, int& attempts)
    {
        return transfer.makeUploadConnectCallback(transfer.m_operationGeneration, socket,
                                                  [&attempts] { ++attempts; });
    }

    static void finish(DvkWavTransfer& transfer, const QString& message)
    {
        transfer.finish(false, message, false);
    }

    static QTcpSocket* client(const DvkWavTransfer& transfer) { return transfer.m_client; }
    static QTcpServer* server(const DvkWavTransfer& transfer) { return transfer.m_server; }
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

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    using TA = AetherSDR::DvkWavTransferTestAccess;
    bool ok = true;

    {
        // This is the exact callback factory handed to RadioModel. A current
        // reply must still build the upload socket.
        AetherSDR::DvkWavTransfer transfer(nullptr);
        TA::begin(transfer, TA::Direction::Upload, 1);
        auto reply = TA::armUploadPortReply(transfer);
        reply(0, QStringLiteral("4995"));

        ok &= expect(TA::client(transfer) != nullptr,
                     "current upload-port reply creates the upload socket");
        ok &= expect(transfer.isTransferring(),
                     "current upload-port reply leaves the transfer running");
    }

    {
        // The reply for a cancelled upload must not seize the replacement.
        AetherSDR::DvkWavTransfer transfer(nullptr);
        TA::begin(transfer, TA::Direction::Upload, 1);
        auto staleReply = TA::armUploadPortReply(transfer);
        transfer.cancel();
        TA::begin(transfer, TA::Direction::Upload, 2);
        QTcpSocket* replacementSocket = TA::client(transfer);

        staleReply(0, QStringLiteral("4995"));
        ok &= expect(TA::client(transfer) == replacementSocket,
                     "stale upload-port reply cannot replace the replacement's socket");
        ok &= expect(transfer.isTransferring(),
                     "stale upload-port reply cannot terminate the replacement");
    }

    {
        // The headline crash: cancel an upload inside the 200 ms connect
        // window and restart as a download, and the old timer reached
        // m_client->connectToHost() with m_client still null.
        AetherSDR::DvkWavTransfer transfer(nullptr);
        TA::begin(transfer, TA::Direction::Upload, 1);
        auto reply = TA::armUploadPortReply(transfer);
        reply(0, QStringLiteral("4995"));
        QTcpSocket* uploadSocket = TA::client(transfer);
        ok &= expect(uploadSocket != nullptr, "the upload socket exists before the restart");

        int connectAttempts = 0;
        std::function<void()> staleConnect =
            TA::makeUploadConnectCallback(transfer, uploadSocket, connectAttempts);
        QTimer::singleShot(200, &transfer, staleConnect);

        transfer.cancel();
        TA::begin(transfer, TA::Direction::Download, 2);
        ok &= expect(TA::client(transfer) == nullptr,
                     "a fresh download has no client socket yet");

        // Deliver directly as well as through the timer: the retired socket is
        // still alive pre-deleteLater, so this needs identity, not just QPointer.
        staleConnect();
        waitForMilliseconds(400);

        ok &= expect(connectAttempts == 0,
                     "stale delayed connect cannot run against a replacement download");
        ok &= expect(TA::client(transfer) == nullptr,
                     "stale delayed connect cannot attach a socket to the replacement");
        ok &= expect(transfer.isTransferring(),
                     "the replacement download survives the stale connect callback");
    }

    {
        // A current delayed connect still runs: the guard must not be a
        // blanket refusal.
        AetherSDR::DvkWavTransfer transfer(nullptr);
        TA::begin(transfer, TA::Direction::Upload, 1);
        auto reply = TA::armUploadPortReply(transfer);
        reply(0, QStringLiteral("4995"));
        int connectAttempts = 0;
        TA::makeUploadConnectCallback(transfer, TA::client(transfer), connectAttempts)();
        ok &= expect(connectAttempts == 1,
                     "current delayed connect invokes its connection action");
    }

    {
        // Download replies carry an independent request identity. An error
        // reply keeps this socket-free: handling it would end the transfer,
        // and it never reaches the listen() the success path performs.
        AetherSDR::DvkWavTransfer transfer(nullptr);
        TA::begin(transfer, TA::Direction::Download, 1);
        auto staleReply = TA::armDownloadPortReply(transfer);
        auto currentReply = TA::armDownloadPortReply(transfer);
        Q_UNUSED(currentReply)

        staleReply(1, QString());
        ok &= expect(transfer.isTransferring(),
                     "a superseded download-port reply cannot terminate the transfer");
        ok &= expect(TA::server(transfer) == nullptr,
                     "a superseded download-port reply opens no listener");
    }

    {
        // Same-direction, same-slot restart: only the generation separates the
        // two, so a flag-only guard would still match here.
        AetherSDR::DvkWavTransfer transfer(nullptr);
        TA::begin(transfer, TA::Direction::Download, 3);
        auto staleReply = TA::armDownloadPortReply(transfer);
        transfer.cancel();
        TA::begin(transfer, TA::Direction::Download, 3);

        staleReply(1, QString());
        ok &= expect(transfer.isTransferring(),
                     "a cancelled download's reply cannot fail its same-slot replacement");
    }

    {
        // Two replies armed inside ONE generation: the first is superseded
        // before it lands. Generation and direction are identical here, so
        // only the per-request identity can separate them.
        AetherSDR::DvkWavTransfer transfer(nullptr);
        TA::begin(transfer, TA::Direction::Upload, 1);
        auto supersededReply = TA::armUploadPortReply(transfer);
        auto currentReply = TA::armUploadPortReply(transfer);
        Q_UNUSED(currentReply)

        supersededReply(0, QStringLiteral("4995"));
        ok &= expect(TA::client(transfer) == nullptr,
                     "a superseded upload-port reply opens no socket");
        ok &= expect(transfer.isTransferring(),
                     "a superseded upload-port reply leaves the transfer running");
    }

    {
        // finish() must reach its terminal state before it emits, or a
        // replacement started from the finished() handler is torn down by the
        // operation that just ended.
        AetherSDR::DvkWavTransfer transfer(nullptr);
        TA::begin(transfer, TA::Direction::Upload, 1);
        bool restarted = false;
        bool transferringInsideHandler = true;
        QObject::connect(&transfer, &AetherSDR::DvkWavTransfer::finished, &transfer,
                         [&](bool, const QString&) {
            if (restarted) {
                return;
            }
            transferringInsideHandler = transfer.isTransferring();
            restarted = true;
            TA::begin(transfer, TA::Direction::Download, 2);
        });

        TA::finish(transfer, QStringLiteral("Upload error: connection refused"));

        ok &= expect(restarted, "the finished handler ran");
        ok &= expect(!transferringInsideHandler,
                     "isTransferring() is already false inside the finished handler");
        ok &= expect(transfer.isTransferring(),
                     "a replacement started from the finished handler survives");
        ok &= expect(TA::client(transfer) == nullptr && TA::server(transfer) == nullptr,
                     "the replacement starts from a clean transport state");
    }

    {
        // RadioModel owns these callbacks and can outlive the transfer. A late
        // response after deletion must be a no-op, not a use-after-free.
        std::function<void(int, const QString&)> lateReply;
        {
            auto transfer = std::make_unique<AetherSDR::DvkWavTransfer>(nullptr);
            TA::begin(*transfer, TA::Direction::Upload, 1);
            lateReply = TA::armUploadPortReply(*transfer);
            transfer.reset();
        }
        lateReply(0, QStringLiteral("4995"));
        // Reaching scope exit proves the weak callback did not touch its owner.
    }

    return ok ? 0 : 1;
}
