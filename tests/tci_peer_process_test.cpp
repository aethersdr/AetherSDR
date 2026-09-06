// TciPeerProcess: the OS socket->process lookup behind the TCI
// client-identity log line (#5087).  A self-connected TCP pair must resolve
// to THIS test binary; a non-loopback peer must not resolve at all.

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

bool selfConnectResolves(const QHostAddress& listenOn, const char* tag)
{
    QTcpServer server;
    if (!server.listen(listenOn, 0)) {
        std::cout << "[SKIP] " << tag << ": cannot listen ("
                  << server.errorString().toStdString() << ")\n";
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

    ok &= selfConnectResolves(QHostAddress(QHostAddress::LocalHost), "ipv4 loopback");
    ok &= selfConnectResolves(QHostAddress(QHostAddress::LocalHostIPv6), "ipv6 loopback");

    const TciPeerProcessInfo remote =
        resolveLoopbackPeerProcess(QHostAddress(QStringLiteral("192.0.2.1")), 50001);
    ok &= expect(!remote.resolved, "a non-loopback peer never resolves");
    const TciPeerProcessInfo noPort =
        resolveLoopbackPeerProcess(QHostAddress(QHostAddress::LocalHost), 0);
    ok &= expect(!noPort.resolved, "port 0 never resolves");

    return ok ? 0 : 1;
}
