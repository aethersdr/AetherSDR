#include <QCoreApplication>
#include <QMetaObject>
#include <QTcpSocket>

#include "core/ProfileTransfer.h"

#include <iostream>

namespace AetherSDR {

class ProfileTransferTestAccess {
public:
    static void prepareIncompleteUpload(ProfileTransfer& transfer, QTcpSocket* socket)
    {
        transfer.m_busy = true;
        transfer.m_phase = ProfileTransfer::Phase::UploadImport;
        transfer.m_operation = ProfileTransfer::Operation::ImportDatabase;
        transfer.m_bytesDone = 0;
        transfer.m_bytesTotal = 1;
        transfer.m_socket = socket;
        QObject::connect(socket, &QTcpSocket::disconnected,
                         &transfer, &ProfileTransfer::onUploadDisconnected);
    }

    static void fail(ProfileTransfer& transfer, const QString& error)
    {
        transfer.fail(error);
    }

    static void connectUploadSocket(ProfileTransfer& transfer, quint16 port)
    {
        transfer.connectUploadSocket(port);
    }

    static QTcpSocket* socket(const ProfileTransfer& transfer)
    {
        return transfer.m_socket;
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

class IncompleteSocket final : public QTcpSocket {
public:
    explicit IncompleteSocket(QObject* parent)
        : QTcpSocket(parent)
    {
        setSocketState(QAbstractSocket::ConnectingState);
    }
};

IncompleteSocket* prepareIncompleteUpload(AetherSDR::ProfileTransfer& transfer,
                                          int& abortDisconnects)
{
    auto* socket = new IncompleteSocket(&transfer);
    AetherSDR::ProfileTransferTestAccess::prepareIncompleteUpload(transfer, socket);
    QObject::connect(socket, &QTcpSocket::stateChanged, socket,
                     [socket, &abortDisconnects](QAbstractSocket::SocketState state) {
                         if (state != QAbstractSocket::UnconnectedState) {
                             return;
                         }
                         ++abortDisconnects;
                         QMetaObject::invokeMethod(socket, "disconnected", Qt::DirectConnection);
                     });
    return socket;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    bool ok = true;

    {
        AetherSDR::ProfileTransfer transfer(nullptr);
        int failures = 0;
        int abortDisconnects = 0;
        QObject::connect(&transfer, &AetherSDR::ProfileTransfer::failed,
                         [&failures](AetherSDR::ProfileTransfer::Operation, const QString&) {
                             ++failures;
                         });

        IncompleteSocket* socket = prepareIncompleteUpload(transfer, abortDisconnects);
        QObject::connect(socket, &QTcpSocket::stateChanged, socket,
                         [&transfer](QAbstractSocket::SocketState state) {
                             if (state == QAbstractSocket::UnconnectedState) {
                                 AetherSDR::ProfileTransferTestAccess::fail(
                                     transfer,
                                     QStringLiteral("re-entrant cleanup failure"));
                             }
                         });

        // No listener, descriptor, or firmware peer is used. This socket starts
        // in an incomplete state so abort() produces synchronous callbacks.
        ok &= expect(socket->state() != QAbstractSocket::UnconnectedState,
                     "socket enters an incomplete connection state before cleanup");

        AetherSDR::ProfileTransferTestAccess::fail(
            transfer, QStringLiteral("forced cleanup failure"));

        ok &= expect(abortDisconnects == 1,
                     "failure cleanup synchronously aborts the incomplete socket");
        ok &= expect(!transfer.isBusy(), "failure cleanup leaves the transfer idle");
        ok &= expect(AetherSDR::ProfileTransferTestAccess::socket(transfer) == nullptr,
                     "failure cleanup clears the active socket");
        ok &= expect(failures == 1, "cleanup cannot emit a re-entrant second failure");
    }

    {
        AetherSDR::ProfileTransfer transfer(nullptr);
        int failures = 0;
        int abortDisconnects = 0;
        QObject::connect(&transfer, &AetherSDR::ProfileTransfer::failed,
                         [&failures](AetherSDR::ProfileTransfer::Operation, const QString&) {
                             ++failures;
                         });

        IncompleteSocket* oldSocket = prepareIncompleteUpload(transfer, abortDisconnects);
        AetherSDR::ProfileTransferTestAccess::connectUploadSocket(transfer, 42607);

        ok &= expect(abortDisconnects == 1,
                     "socket replacement synchronously aborts the incomplete socket");
        QTcpSocket* replacementSocket =
            AetherSDR::ProfileTransferTestAccess::socket(transfer);
        ok &= expect(replacementSocket != nullptr && replacementSocket != oldSocket,
                     "socket replacement installs a new upload socket");
        ok &= expect(transfer.isBusy(), "socket replacement keeps the transfer active");
        ok &= expect(failures == 0, "socket replacement cannot emit a stale upload failure");
    }

    return ok ? 0 : 1;
}
