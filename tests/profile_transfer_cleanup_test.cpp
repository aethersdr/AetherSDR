#include <QCoreApplication>
#include <QMetaObject>
#include <QTcpServer>
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

    static void prepareCompleteUpload(ProfileTransfer& transfer, QTcpSocket* socket)
    {
        prepareIncompleteUpload(transfer, socket);
        transfer.m_phase = ProfileTransfer::Phase::UploadMetaSubset;
        transfer.m_bytesDone = 1;
        transfer.m_bytesTotal = 1;
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

    static void prepareDownloadServer(ProfileTransfer& transfer, QTcpServer* server)
    {
        transfer.m_busy = true;
        transfer.m_phase = ProfileTransfer::Phase::DownloadPackage;
        transfer.m_operation = ProfileTransfer::Operation::ExportDatabase;
        transfer.m_server = server;
        QObject::connect(server, &QTcpServer::newConnection,
                         &transfer, &ProfileTransfer::onDownloadConnection);
    }

    static QTcpServer* server(const ProfileTransfer& transfer)
    {
        return transfer.m_server;
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
    explicit IncompleteSocket(QObject* parent,
                              QAbstractSocket::SocketState state = QAbstractSocket::ConnectingState)
        : QTcpSocket(parent)
    {
        setSocketState(state);
    }
};

// Observes the teardown of `socket` from the inside. `abortDisconnects` counts
// how often the transfer drove the socket to UnconnectedState, and
// `detachedFirst` records whether the transfer had already released the member
// at that instant — i.e. whether disposal detaches before it acts. The busy
// guard hides a missing detach, so the ordering needs its own assertion.
void observeTeardown(AetherSDR::ProfileTransfer& transfer, IncompleteSocket* socket,
                     int& abortDisconnects, bool& detachedFirst)
{
    QObject::connect(socket, &QTcpSocket::stateChanged, socket,
                     [socket, &transfer, &abortDisconnects,
                      &detachedFirst](QAbstractSocket::SocketState state) {
                         if (state != QAbstractSocket::UnconnectedState) {
                             return;
                         }
                         ++abortDisconnects;
                         detachedFirst =
                             AetherSDR::ProfileTransferTestAccess::socket(transfer) != socket;
                         QMetaObject::invokeMethod(socket, "disconnected", Qt::DirectConnection);
                     });
}

IncompleteSocket* prepareIncompleteUpload(AetherSDR::ProfileTransfer& transfer,
                                          int& abortDisconnects, bool& detachedFirst)
{
    auto* socket = new IncompleteSocket(&transfer);
    AetherSDR::ProfileTransferTestAccess::prepareIncompleteUpload(transfer, socket);
    observeTeardown(transfer, socket, abortDisconnects, detachedFirst);
    return socket;
}

} // namespace

// This test deliberately runs WITHOUT an event loop. connectUploadSocket()
// schedules a QTimer::singleShot(200ms) that dereferences m_model, which is
// null here, so exec()/processEvents()/qWait() would turn that timer into a
// null dereference. Every path below is driven synchronously on purpose; keep
// it that way, or give the transfer a RadioModel first.
int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    bool ok = true;

    {
        AetherSDR::ProfileTransfer transfer(nullptr);
        int failures = 0;
        int abortDisconnects = 0;
        bool detachedFirst = false;
        QObject::connect(&transfer, &AetherSDR::ProfileTransfer::failed,
                         [&failures](AetherSDR::ProfileTransfer::Operation, const QString&) {
                             ++failures;
                         });

        IncompleteSocket* socket =
            prepareIncompleteUpload(transfer, abortDisconnects, detachedFirst);
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
        ok &= expect(detachedFirst,
                     "failure cleanup detaches the socket before aborting it");
        ok &= expect(!transfer.isBusy(), "failure cleanup leaves the transfer idle");
        ok &= expect(AetherSDR::ProfileTransferTestAccess::socket(transfer) == nullptr,
                     "failure cleanup clears the active socket");
        ok &= expect(failures == 1, "cleanup cannot emit a re-entrant second failure");
    }

    {
        AetherSDR::ProfileTransfer transfer(nullptr);
        int failures = 0;
        int abortDisconnects = 0;
        bool detachedFirst = false;
        QObject::connect(&transfer, &AetherSDR::ProfileTransfer::failed,
                         [&failures](AetherSDR::ProfileTransfer::Operation, const QString&) {
                             ++failures;
                         });

        IncompleteSocket* oldSocket =
            prepareIncompleteUpload(transfer, abortDisconnects, detachedFirst);
        // No event loop is pumped in this file, deliberately: connectUploadSocket()
        // arms a 200 ms singleShot that would dereference the null m_model and open
        // a real connection. The timer dies with `transfer` at scope exit.
        AetherSDR::ProfileTransferTestAccess::connectUploadSocket(transfer, 42607);

        ok &= expect(abortDisconnects == 1,
                     "socket replacement synchronously aborts the incomplete socket");
        ok &= expect(detachedFirst,
                     "socket replacement detaches the old socket before aborting it");
        QTcpSocket* replacementSocket =
            AetherSDR::ProfileTransferTestAccess::socket(transfer);
        ok &= expect(replacementSocket != nullptr && replacementSocket != oldSocket,
                     "socket replacement installs a new upload socket");
        ok &= expect(transfer.isBusy(), "socket replacement keeps the transfer active");
        ok &= expect(failures == 0, "socket replacement cannot emit a stale upload failure");
    }

    {
        // Successful completion disposes the socket through the same helper,
        // with abortConnection=false. This is the one disposal site where the
        // transfer is still busy, so the terminal-state guard cannot stand in
        // for detaching the handlers -- only the helper does that.
        AetherSDR::ProfileTransfer transfer(nullptr);
        int failures = 0;
        int disconnects = 0;
        bool detachedFirst = false;
        int completions = 0;
        QObject::connect(&transfer, &AetherSDR::ProfileTransfer::failed,
                         [&failures](AetherSDR::ProfileTransfer::Operation, const QString&) {
                             ++failures;
                         });
        // The completion branch emits exactly this progress message. Counting it
        // counts how many times the transfer ran that branch, which is the
        // re-entry question, independent of how many raw disconnected() emissions
        // Qt produces while the socket winds down.
        QObject::connect(&transfer, &AetherSDR::ProfileTransfer::progress,
                         [&completions](const QString& message, qint64, qint64) {
                             if (message.contains(QStringLiteral("prepare the database"))) {
                                 ++completions;
                             }
                         });

        auto* socket = new IncompleteSocket(&transfer, QAbstractSocket::ConnectedState);
        AetherSDR::ProfileTransferTestAccess::prepareCompleteUpload(transfer, socket);
        observeTeardown(transfer, socket, disconnects, detachedFirst);
        // The radio closing the connection on a complete upload is the entry
        // point; the handler must then release the socket exactly once.
        QMetaObject::invokeMethod(socket, "disconnected", Qt::DirectConnection);

        ok &= expect(disconnects == 1,
                     "completion disposes the socket through the shared helper");
        ok &= expect(detachedFirst,
                     "completion detaches the socket before disconnecting it");
        ok &= expect(AetherSDR::ProfileTransferTestAccess::socket(transfer) == nullptr,
                     "completion clears the active socket");
        ok &= expect(completions == 1,
                     "the synchronous disconnect during teardown cannot re-enter the "
                     "transfer's completion branch");
        ok &= expect(failures == 0, "completion emits no failure");
        ok &= expect(transfer.isBusy(), "completion keeps the transfer active for the next phase");
    }

    {
        // The download listener is torn down by the same detach-then-act rule.
        // This QTcpServer never calls listen(), so it binds no port and owns no
        // descriptor -- it is an inert QObject standing in for the listener's
        // signal wiring, which is the only part under test.
        AetherSDR::ProfileTransfer transfer(nullptr);
        int handlerCalls = 0;
        auto* server = new QTcpServer(&transfer);
        AetherSDR::ProfileTransferTestAccess::prepareDownloadServer(transfer, server);

        // Receiver is the transfer, so this rides the same connection set that
        // cleanup must sever. If the listener is closed while still attached, a
        // newConnection already queued before cleanup still reaches the transfer
        // -- with m_server null underneath it.
        QObject::connect(server, &QTcpServer::newConnection, &transfer,
                         [&handlerCalls] { ++handlerCalls; });

        ok &= expect(!server->isListening(), "the download listener test binds no port");

        AetherSDR::ProfileTransferTestAccess::fail(
            transfer, QStringLiteral("forced download cleanup failure"));

        ok &= expect(AetherSDR::ProfileTransferTestAccess::server(transfer) == nullptr,
                     "failure cleanup clears the download listener");

        QMetaObject::invokeMethod(server, "newConnection", Qt::DirectConnection);
        ok &= expect(handlerCalls == 0,
                     "failure cleanup detaches the listener's handlers before closing it");
    }

    return ok ? 0 : 1;
}
