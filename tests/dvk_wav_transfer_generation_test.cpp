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
#include <QFile>
#include <QStringList>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtEndian>

#include "core/DvkWavTransfer.h"
#include "gui/DvkAvailabilityGate.h"
#include "models/DvkModel.h"

#include <functional>
#include <iostream>
#include <memory>
#include <vector>

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

    using Reply = std::function<void(int, const QString&)>;
    // Records each command and keeps its reply so the test plays the radio.
    static void recordCommands(DvkWavTransfer& transfer, QStringList& sent,
                               std::vector<Reply>& replies)
    {
        transfer.m_commandSender = [&sent, &replies](const QString& command, Reply reply) {
            sent << command;
            replies.push_back(std::move(reply));
        };
        // RadioModel::requestFileUploadPort() encodes this leg; record what it would send.
        transfer.m_uploadPortRequester = [&sent, &replies](qint64 size, Reply reply) {
            sent << QStringLiteral("file upload %1 dvk_recording").arg(size);
            replies.push_back(std::move(reply));
        };
    }

    static void attachDvk(DvkWavTransfer& transfer, DvkModel* dvk) { transfer.m_dvk = dvk; }
    static int uploadPort(const DvkWavTransfer& transfer) { return transfer.m_uploadPort; }
    static void uploadError(DvkWavTransfer& transfer)
    {
        transfer.handleUploadError(transfer.m_operationGeneration, transfer.m_client);
    }
    static void finishOk(DvkWavTransfer& transfer) { transfer.finish(true, QString(), false); }

    static qint64 uploadSize(const DvkWavTransfer& transfer) { return transfer.m_uploadData.size(); }

    static QTcpSocket* client(const DvkWavTransfer& transfer) { return transfer.m_client; }
    static QTcpServer* server(const DvkWavTransfer& transfer) { return transfer.m_server; }
};

} // namespace AetherSDR

namespace {

// 0.5 s of 48 kHz stereo 16-bit silence: the converter's 24 kHz mono output is
// 24 000 bytes of samples plus a 44-byte header.
QByteArray stereo48kWav()
{
    const quint32 dataBytes = 24'000 * 4;
    QByteArray wav("RIFF");
    char b[4];
    qToLittleEndian<quint32>(36 + dataBytes, b); wav.append(b, 4);
    wav += "WAVEfmt ";
    qToLittleEndian<quint32>(16, b); wav.append(b, 4);
    const quint16 fmt[] = {1, 2};
    for (quint16 v : fmt) { qToLittleEndian(v, b); wav.append(b, 2); }
    qToLittleEndian<quint32>(48'000, b); wav.append(b, 4);
    qToLittleEndian<quint32>(48'000 * 4, b); wav.append(b, 4);
    const quint16 tail[] = {4, 16};
    for (quint16 v : tail) { qToLittleEndian(v, b); wav.append(b, 2); }
    wav += "data";
    qToLittleEndian(dataBytes, b); wav.append(b, 4);
    wav.append(QByteArray(static_cast<qsizetype>(dataBytes), '\0'));
    return wav;
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

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
        // #6244: the wiki's two-step upload. `dvk upload` only names the slot
        // and replies with no body; the bytes go through `file upload <size>
        // dvk_recording`, whose reply carries the port.
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("cq.wav"));
        ok &= expect(writeFile(path, stereo48kWav()), "fixture WAV written");

        AetherSDR::DvkWavTransfer transfer(nullptr);
        QStringList sent;
        std::vector<TA::Reply> replies;
        TA::recordCommands(transfer, sent, replies);

        transfer.upload(3, path);
        ok &= expect(sent == QStringList{QStringLiteral("dvk upload id=3")},
                     "upload first names the slot with dvk upload");
        ok &= expect(TA::uploadSize(transfer) == 44 + 24'000,
                     "the queued bytes are the 24 kHz mono conversion");

        replies.at(0)(0, QString());
        ok &= expect(sent.size() == 2
                         && sent.at(1) == QStringLiteral("file upload 24044 dvk_recording"),
                     "an accepted slot is followed by file upload <size> dvk_recording");
        ok &= expect(TA::client(transfer) == nullptr,
                     "no socket opens before the file server names its port");

        replies.at(1)(0, QStringLiteral("42607"));
        ok &= expect(TA::client(transfer) != nullptr && transfer.isTransferring(),
                     "the file-upload port reply opens the upload socket");
    }

    {
        // A refused slot ends the upload with the wiki's meaning, not hex.
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("cq.wav"));
        ok &= expect(writeFile(path, stereo48kWav()), "fixture WAV written");

        AetherSDR::DvkWavTransfer transfer(nullptr);
        QStringList sent;
        std::vector<TA::Reply> replies;
        TA::recordCommands(transfer, sent, replies);
        QString message;
        QObject::connect(&transfer, &AetherSDR::DvkWavTransfer::finished, &transfer,
                         [&message](bool, const QString& m) { message = m; });

        transfer.upload(3, path);
        replies.at(0)(static_cast<int>(0xE2000000u), QString());
        ok &= expect(!transfer.isTransferring() && sent.size() == 1,
                     "a refused dvk upload sends no file upload");
        ok &= expect(message.contains(QStringLiteral("upload is already pending")),
                     "the refusal is described, not shown as bare hex");
    }

    {
        // An empty port body falls back to the file server's 42607.
        AetherSDR::DvkWavTransfer transfer(nullptr);
        TA::begin(transfer, TA::Direction::Upload, 1);
        auto reply = TA::armUploadPortReply(transfer);
        reply(0, QString());
        ok &= expect(TA::client(transfer) != nullptr && transfer.isTransferring(),
                     "an empty file-upload reply still opens the upload socket");
    }

    {
        // A file the converter refuses never reaches the radio.
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("junk.wav"));
        ok &= expect(writeFile(path, QByteArray("not a wav file at all")), "fixture written");

        AetherSDR::DvkWavTransfer transfer(nullptr);
        QStringList sent;
        std::vector<TA::Reply> replies;
        TA::recordCommands(transfer, sent, replies);
        transfer.upload(3, path);
        ok &= expect(sent.isEmpty() && !transfer.isTransferring(),
                     "an unreadable WAV sends nothing to the radio");
    }

    {
        // #6244 review: a refusal on any transfer leg reaches the DVK license
        // latch, so an unlicensed radio's first Import closes the gate too.
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("cq.wav"));
        ok &= expect(writeFile(path, stereo48kWav()), "fixture WAV written");
        const auto blocked = [](const AetherSDR::DvkModel& dvk) {
            return AetherSDR::dvkIndicatorBlocker(true, false, false, dvk.licenseRefused())
                == AetherSDR::DvkIndicatorBlocker::NotLicensed;
        };
        for (int leg = 0; leg < 3; ++leg) {
            AetherSDR::DvkModel dvk;
            AetherSDR::DvkWavTransfer transfer(nullptr);
            QStringList sent;
            std::vector<TA::Reply> replies;
            TA::recordCommands(transfer, sent, replies);
            TA::attachDvk(transfer, &dvk);
            if (leg == 2) {
                transfer.download(3, dir.filePath(QStringLiteral("out.wav")));
            } else {
                transfer.upload(3, path);
                if (leg == 1) {
                    replies.at(0)(0, QString());
                }
            }
            replies.back()(static_cast<int>(0x50004001u), QString());
            const char* label = leg == 0 ? "a refused dvk upload latches the license refusal"
                              : leg == 1 ? "a refused file upload latches the license refusal"
                                         : "a refused dvk download latches the license refusal";
            ok &= expect(dvk.licenseRefused() && blocked(dvk) && !transfer.isTransferring(), label);
        }
    }

    {
        // A WAV transfer holds DVK admission for its whole life (#6244 item 6).
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("cq.wav"));
        ok &= expect(writeFile(path, stereo48kWav()), "fixture WAV written");
        AetherSDR::DvkModel dvk;
        AetherSDR::DvkWavTransfer transfer(nullptr);
        QStringList sent;
        std::vector<TA::Reply> replies;
        TA::recordCommands(transfer, sent, replies);
        TA::attachDvk(transfer, &dvk);
        transfer.upload(3, path);
        ok &= expect(!dvk.canStartOperation(), "a running upload blocks a DVK start");
        TA::finishOk(transfer);
        ok &= expect(dvk.canStartOperation(), "the upload's end admits DVK starts again");

        // fw 4.2.20's file server answers 50000053 for ~2.2 s after an upload.
        ok &= expect(transfer.isBusy(), "the file server settle window follows an upload");
        QString message;
        QObject::connect(&transfer, &AetherSDR::DvkWavTransfer::finished, &transfer,
                         [&message](bool, const QString& m) { message = m; });
        const int before = sent.size();
        transfer.upload(4, path);
        ok &= expect(sent.size() == before && message.contains(QStringLiteral("still busy")),
                     "a transfer inside the settle window is refused locally");
    }

    {
        // A named port that refuses the connection gets one retry on 42607.
        AetherSDR::DvkWavTransfer transfer(nullptr);
        TA::begin(transfer, TA::Direction::Upload, 1);
        auto reply = TA::armUploadPortReply(transfer);
        reply(0, QStringLiteral("4995"));
        QTcpSocket* first = TA::client(transfer);
        ok &= expect(TA::uploadPort(transfer) == 4995, "the named port is tried first");
        TA::uploadError(transfer);
        ok &= expect(transfer.isTransferring() && TA::client(transfer) != first
                         && TA::uploadPort(transfer) == 42607,
                     "a refused named port retries once on 42607");
        TA::uploadError(transfer);
        ok &= expect(!transfer.isTransferring(), "a refused 42607 ends the upload");
    }

    {
        // Export accepts only the radio as its peer.
        const QHostAddress radio(QStringLiteral("192.168.50.100"));
        ok &= expect(AetherSDR::DvkWavTransfer::isRadioPeer(radio, radio), "the radio is accepted");
        ok &= expect(AetherSDR::DvkWavTransfer::isRadioPeer(
                         QHostAddress(QStringLiteral("::ffff:192.168.50.100")), radio),
                     "the radio over a v4-mapped socket is accepted");
        ok &= expect(!AetherSDR::DvkWavTransfer::isRadioPeer(
                         QHostAddress(QStringLiteral("192.168.50.77")), radio),
                     "another LAN host is rejected");
        ok &= expect(AetherSDR::DvkWavTransfer::isRadioPeer(
                         QHostAddress(QStringLiteral("192.168.50.77")), QHostAddress()),
                     "an unknown radio address accepts (not connected)");
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
