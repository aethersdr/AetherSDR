// TciPeerProcess: the OS socket->process lookup behind the TCI
// client-identity log line (#5087).  A self-connected TCP pair must resolve
// to THIS test binary; a non-loopback peer must not resolve at all.
//
// Socket-owning test (AGENTS.md, test-layer boundary): the lookup under test
// asks the kernel which process owns a socket, so it needs a real one — this
// binary listens on an ephemeral loopback port and connects to itself.  No
// peer process, no fake firmware.  A loopback listen that fails is reported
// and the run exits 77 (ctest "skipped"), never a silent pass and never a
// wait on the timeout.

#include "core/TciPeerProcess.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

#include <iostream>

using namespace AetherSDR;

namespace {

bool expect(bool condition, const char* label)
{
    std::cout << (condition ? "[ OK ] " : "[FAIL] ") << label << '\n';
    return condition;
}

constexpr int kExitSkip = 77;   // tests.cmake: SKIP_RETURN_CODE 77

// Returns true on pass; sets *skipped (and returns true) when the loopback
// listen itself is unavailable, so the caller can exit 77 instead of 0.
bool selfConnectResolves(const QHostAddress& listenOn, const char* tag, bool* skipped)
{
    QTcpServer server;
    if (!server.listen(listenOn, 0)) {
        std::cout << "[SKIP] " << tag << ": cannot listen ("
                  << server.errorString().toStdString() << ")\n";
        *skipped = true;
        return true;
    }
    QTcpSocket client;
    client.connectToHost(listenOn, server.serverPort());
    bool ok = expect(client.waitForConnected(3000), "client connects");
    ok &= expect(server.waitForNewConnection(3000), "server accepts");
    QTcpSocket* accepted = server.nextPendingConnection();
    if (!accepted) return false;

    const TciPeerProcessInfo info =
        resolveLoopbackPeerProcess(accepted->peerAddress(), accepted->peerPort());
    std::cout << "       " << tag << ": peer " << accepted->peerAddress().toString().toStdString()
              << ":" << accepted->peerPort()
              << " -> resolved=" << info.resolved
              << " name=\"" << info.name.toStdString() << "\""
              << " exe=\"" << info.exePath.toStdString() << "\""
              << " version=\"" << info.version.toStdString() << "\"\n";
    ok &= expect(info.resolved, "self-connected peer resolves");
    const QString self = QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
    const QString got  = QFileInfo(info.exePath).canonicalFilePath();
    ok &= expect(!info.exePath.isEmpty() && got == self,
                 "resolved exe is this test binary");
    ok &= expect(!info.name.isEmpty(), "resolved name is non-empty");
    client.disconnectFromHost();
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    bool skipped = false;

    ok &= selfConnectResolves(QHostAddress(QHostAddress::LocalHost), "ipv4 loopback", &skipped);
    ok &= selfConnectResolves(QHostAddress(QHostAddress::LocalHostIPv6), "ipv6 loopback", &skipped);

    const TciPeerProcessInfo remote =
        resolveLoopbackPeerProcess(QHostAddress(QStringLiteral("192.0.2.1")), 50001);
    ok &= expect(!remote.resolved, "a non-loopback peer never resolves");
    const TciPeerProcessInfo noPort =
        resolveLoopbackPeerProcess(QHostAddress(QHostAddress::LocalHost), 0);
    ok &= expect(!noPort.resolved, "port 0 never resolves");

    if (!ok) return 1;
    if (skipped) {
        std::cout << "[SKIP] a loopback listen was unavailable; exiting " << kExitSkip << '\n';
        return kExitSkip;
    }
    return 0;
}
