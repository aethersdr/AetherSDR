// TcpByteProxy on loopback: our proxy server is the subject; both peers are
// generic byte endpoints with no radio protocol. Binds 127.0.0.1 ephemeral
// TCP ports only; exits 77 when loopback cannot be bound.

#include "core/TcpByteProxy.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QNetworkInterface>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <random>
#include <vector>

using AetherSDR::TcpByteProxy;

namespace {

constexpr int kSkip = 77;
int g_failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 5000)
{
    QElapsedTimer t;
    t.start();
    while (!predicate() && t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (!predicate()) {
            QThread::usleep(200);
        }
    }
    return predicate();
}

void spin(int ms)
{
    waitUntil([] { return false; }, ms);
}

QByteArray randomBytes(int size, unsigned seed)
{
    std::mt19937 rng(seed);
    QByteArray out(size, '\0');
    for (int i = 0; i < size; ++i) {
        out[i] = static_cast<char>(rng() & 0xFF);
    }
    return out;
}

// Generic upstream byte peer: records what each accepted connection receives
// and can send a fixed greeting the moment it accepts.
class UpstreamPeer {
public:
    struct Conn {
        QTcpSocket* socket{nullptr};
        QByteArray received;
        bool disconnected{false};
    };

    bool listen()
    {
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (QTcpSocket* s = server.nextPendingConnection()) {
                auto c = std::make_shared<Conn>();
                c->socket = s;
                conns.push_back(c);
                QObject::connect(s, &QIODevice::readyRead, s, [c] { c->received += c->socket->readAll(); });
                QObject::connect(s, &QAbstractSocket::disconnected, s, [c] {
                    if (c->socket->isOpen()) {
                        c->received += c->socket->readAll();
                    }
                    c->disconnected = true;
                });
                if (!greeting.isEmpty()) {
                    s->write(greeting);
                }
            }
        });
        return server.listen(QHostAddress::LocalHost, 0);
    }

    quint16 port() const { return server.serverPort(); }

    QTcpServer server;
    QByteArray greeting;
    std::vector<std::shared_ptr<Conn>> conns;
};

struct Client {
    QTcpSocket socket;
    QByteArray received;
    bool connected{false};
    bool disconnected{false};

    Client()
    {
        QObject::connect(&socket, &QAbstractSocket::connected, &socket, [this] { connected = true; });
        QObject::connect(&socket, &QIODevice::readyRead, &socket, [this] { received += socket.readAll(); });
        QObject::connect(&socket, &QAbstractSocket::disconnected, &socket, [this] {
            if (socket.isOpen()) {
                received += socket.readAll();
            }
            disconnected = true;
        });
    }

    // True once the TCP connect completed, even if the proxy has since closed it.
    bool connectTo(quint16 port)
    {
        socket.connectToHost(QHostAddress::LocalHost, port);
        return waitUntil([this] { return connected; });
    }
};

// A socket whose connect never completes, for a deterministic connect timeout.
class HangingSocket : public QTcpSocket {
public:
    using QTcpSocket::connectToHost;
    void connectToHost(const QString&, quint16, OpenMode = ReadWrite,
                       NetworkLayerProtocol = AnyIPProtocol) override
    {
    }
};

TcpByteProxy::Config loopbackConfig(quint16 upstreamPort)
{
    TcpByteProxy::Config c;
    c.listenAddress = QHostAddress::LocalHost;
    c.listenPort = 0;
    c.upstreamAddress = QHostAddress::LocalHost;
    c.upstreamPort = upstreamPort;
    return c;
}

bool startOrSkip(TcpByteProxy& proxy, const TcpByteProxy::Config& config)
{
    if (proxy.start(config)) {
        return true;
    }
    // Only a refused bind is environmental; any other refusal is a defect.
    if (proxy.lastError().contains(QStringLiteral("Cannot listen"))) {
        std::fprintf(stderr, "SKIP: proxy could not bind loopback: %s\n",
                     qPrintable(proxy.lastError()));
        std::exit(kSkip);
    }
    std::fprintf(stderr, "FAIL: proxy refused a valid loopback config: %s\n",
                 qPrintable(proxy.lastError()));
    std::exit(1);
}

void testBidirectionalStreamWithGreeting()
{
    UpstreamPeer up;
    up.greeting = randomBytes(1000, 1) + QByteArray("\r\n\0\xff", 4);
    if (!up.listen()) {
        std::exit(kSkip);
    }
    TcpByteProxy proxy;
    startOrSkip(proxy, loopbackConfig(up.port()));
    check(proxy.state() == TcpByteProxy::State::Listening, "proxy listens after start");

    Client client;
    check(client.connectTo(proxy.listeningPort()), "client connects to proxy");
    check(waitUntil([&] { return client.received.size() >= up.greeting.size(); }),
          "upstream greeting reaches the client before it sends anything");
    check(client.received == up.greeting, "greeting is byte-identical");
    check(proxy.state() == TcpByteProxy::State::Relaying, "state is Relaying");

    const QByteArray down = randomBytes(2 * 1024 * 1024, 2);
    const QByteArray upBytes = randomBytes(2 * 1024 * 1024, 3);
    client.socket.write(down);
    check(waitUntil([&] { return up.conns.size() == 1; }), "one upstream connection");
    up.conns[0]->socket->write(upBytes);
    check(waitUntil([&] {
              return up.conns[0]->received.size() >= down.size()
                  && client.received.size() >= up.greeting.size() + upBytes.size();
          }, 20000),
          "2 MiB each way completes");
    check(up.conns[0]->received == down, "client->upstream stream is byte-identical");
    check(client.received == up.greeting + upBytes, "upstream->client stream is byte-identical");
    const TcpByteProxy::Stats s = proxy.stats();
    check(s.toUpstream == static_cast<quint64>(down.size()), "toUpstream counter matches");
    check(s.toDownstream == static_cast<quint64>(up.greeting.size() + upBytes.size()),
          "toDownstream counter matches");

    const quint16 port = proxy.listeningPort();
    proxy.stop();
    check(waitUntil([&] { return client.disconnected && up.conns[0]->disconnected; }),
          "stop closes both peers");
    check(proxy.state() == TcpByteProxy::State::Stopped, "state is Stopped");
    check(proxy.listeningPort() == 0, "listener is closed after stop");
    Client late;
    late.socket.connectToHost(QHostAddress::LocalHost, port);
    check(waitUntil([&] {
              return late.socket.state() == QAbstractSocket::UnconnectedState;
          }),
          "a stopped proxy accepts no connections");
    check(up.conns.size() == 1, "no upstream connection after stop");
}

void testAdditionalClientRejected()
{
    UpstreamPeer up;
    if (!up.listen()) {
        std::exit(kSkip);
    }
    TcpByteProxy proxy;
    startOrSkip(proxy, loopbackConfig(up.port()));
    const quint16 port = proxy.listeningPort();

    Client first;
    check(first.connectTo(port), "first client connects");
    check(waitUntil([&] { return proxy.state() == TcpByteProxy::State::Relaying; }), "first relays");
    first.socket.write("before");
    check(waitUntil([&] { return !up.conns.empty() && up.conns[0]->received == "before"; }),
          "first client's bytes arrive");

    Client second;
    second.socket.connectToHost(QHostAddress::LocalHost, port);
    check(waitUntil([&] { return second.disconnected; }), "additional client is closed");
    check(second.received.isEmpty(), "no banner or bytes are sent to the rejected client");
    check(proxy.stats().rejectedClients == 1, "rejection is counted");

    first.socket.write("after");
    up.conns[0]->socket->write("reply");
    check(waitUntil([&] { return up.conns[0]->received == "beforeafter" && first.received == "reply"; }),
          "active connection is undisturbed by the rejection");
    check(up.conns.size() == 1, "rejected client never opened an upstream connection");
    proxy.stop();
}

void testUnavailableUpstream()
{
    quint16 deadPort = 0;
    {
        QTcpServer probe;
        if (!probe.listen(QHostAddress::LocalHost, 0)) {
            std::exit(kSkip);
        }
        deadPort = probe.serverPort();
    }
    TcpByteProxy proxy;
    startOrSkip(proxy, loopbackConfig(deadPort));
    Client client;
    check(client.connectTo(proxy.listeningPort()), "client connects");
    client.socket.write("lost");
    check(waitUntil([&] { return client.disconnected; }), "client is closed when upstream refuses");
    check(client.received.isEmpty(), "the error is not written into the client stream");
    check(proxy.lastError().contains(QStringLiteral("Radio connection failed")),
          "upstream failure is reported locally");
    check(proxy.state() == TcpByteProxy::State::Listening, "listener stays up after a peer failure");
    proxy.stop();
}

void testConnectTimeout()
{
    TcpByteProxy proxy;
    TcpByteProxy::Tuning tuning;
    tuning.connectTimeoutMs = 100;
    proxy.setTuning(tuning);
    proxy.setUpstreamSocketFactory([] { return new HangingSocket; });
    startOrSkip(proxy, loopbackConfig(9));
    Client client;
    check(client.connectTo(proxy.listeningPort()), "client connects");
    check(waitUntil([&] { return proxy.state() == TcpByteProxy::State::Connecting; }, 1000)
              || client.disconnected,
          "proxy waits for upstream");
    check(waitUntil([&] { return client.disconnected; }, 3000), "connect timeout closes the client");
    check(proxy.lastError().contains(QStringLiteral("timed out")), "timeout is reported locally");
    check(client.received.isEmpty(), "nothing written to the client on timeout");
    proxy.stop();
}

void testBindConflictNoFallback()
{
    QTcpServer holder;
    if (!holder.listen(QHostAddress::LocalHost, 0)) {
        std::exit(kSkip);
    }
    TcpByteProxy proxy;
    TcpByteProxy::Config c = loopbackConfig(holder.serverPort() == 65535 ? 65534 : holder.serverPort() + 1);
    c.listenPort = holder.serverPort();
    check(!proxy.start(c), "start fails when the port is taken");
    check(proxy.state() == TcpByteProxy::State::Error, "state is Error");
    check(proxy.lastError().contains(QStringLiteral("Cannot listen")), "bind conflict is reported");
    check(proxy.listeningPort() == 0, "no fallback port or address is bound");
}

void testSelfProxyRejected()
{
    TcpByteProxy::Config c;
    c.listenAddress = QHostAddress::LocalHost;
    c.listenPort = 4992;
    c.upstreamAddress = QHostAddress::LocalHost;
    c.upstreamPort = 4992;
    check(!TcpByteProxy::validate(c).isEmpty(), "upstream == listener is rejected");

    for (const QHostAddress& a : QNetworkInterface::allAddresses()) {
        bool ok = false;
        a.toIPv4Address(&ok);
        if (ok && !a.isLoopback()) {
            c.upstreamAddress = a;
            check(!TcpByteProxy::validate(c).isEmpty(),
                  "another local interface on the listening port is rejected");
            break;
        }
    }
    c.upstreamAddress = QHostAddress(QStringLiteral("192.0.2.10"));
    check(TcpByteProxy::validate(c).isEmpty(), "a remote radio on the same port is allowed");
    c.listenAddress = QHostAddress::AnyIPv4;
    check(!TcpByteProxy::validate(c).isEmpty(), "binding all interfaces is rejected");
    c.listenAddress = QHostAddress::LocalHost;
    c.upstreamPort = 0;
    check(!TcpByteProxy::validate(c).isEmpty(), "upstream port 0 is rejected");
}

void testNormalEofBothDirections()
{
    UpstreamPeer up;
    if (!up.listen()) {
        std::exit(kSkip);
    }
    TcpByteProxy proxy;
    startOrSkip(proxy, loopbackConfig(up.port()));

    // Client sends a final chunk and closes immediately.
    {
        Client client;
        check(client.connectTo(proxy.listeningPort()), "client connects");
        check(waitUntil([&] { return proxy.state() == TcpByteProxy::State::Relaying; }), "relaying");
        const QByteArray tail = randomBytes(300000, 5);
        client.socket.write(tail);
        client.socket.disconnectFromHost();
        check(waitUntil([&] { return !up.conns.empty() && up.conns.back()->disconnected; }, 10000),
              "upstream sees the close");
        check(up.conns.back()->received == tail, "client tail bytes arrive before the close");
        check(waitUntil([&] { return proxy.state() == TcpByteProxy::State::Listening; }),
              "proxy returns to Listening after orderly close");
        check(proxy.lastError().isEmpty(), "orderly close is not an error");
    }
    // Upstream sends a final chunk and closes immediately.
    {
        Client client;
        check(client.connectTo(proxy.listeningPort()), "client reconnects");
        check(waitUntil([&] { return up.conns.size() == 2; }), "fresh upstream connection");
        check(waitUntil([&] { return proxy.state() == TcpByteProxy::State::Relaying; }), "relaying again");
        const QByteArray tail = randomBytes(300000, 6);
        up.conns[1]->socket->write(tail);
        up.conns[1]->socket->disconnectFromHost();
        check(waitUntil([&] { return client.disconnected; }, 10000), "client sees the close");
        check(client.received == tail, "upstream tail bytes arrive before the close");
    }
    proxy.stop();
}

void testReconnectIsolation()
{
    UpstreamPeer up;
    if (!up.listen()) {
        std::exit(kSkip);
    }
    TcpByteProxy proxy;
    startOrSkip(proxy, loopbackConfig(up.port()));

    auto first = std::make_unique<Client>();
    check(first->connectTo(proxy.listeningPort()), "first client connects");
    check(waitUntil([&] { return proxy.state() == TcpByteProxy::State::Relaying; }), "relaying");
    first->socket.write("one");
    check(waitUntil([&] { return !up.conns.empty() && up.conns[0]->received == "one"; }), "first bytes");
    const quint64 gen1 = proxy.connectionGeneration();
    first->socket.abort();
    check(waitUntil([&] { return up.conns[0]->disconnected; }), "first upstream closed with its client");
    up.conns[0]->socket->write("late-from-old-upstream");

    Client second;
    check(second.connectTo(proxy.listeningPort()), "second client connects");
    check(waitUntil([&] { return up.conns.size() == 2; }), "second upstream connection opened");
    check(proxy.connectionGeneration() > gen1, "new connection generation");
    second.socket.write("two");
    check(waitUntil([&] { return up.conns[1]->received == "two"; }), "second bytes");
    spin(100);
    check(up.conns[0]->received == "one", "old upstream received nothing new");
    check(up.conns[1]->received == "two", "new upstream received no stale bytes");
    check(second.received.isEmpty(), "new client received nothing from the old upstream");
    proxy.stop();
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testSelfProxyRejected();
    testBindConflictNoFallback();
    testBidirectionalStreamWithGreeting();
    testAdditionalClientRejected();
    testUnavailableUpstream();
    testConnectTimeout();
    testNormalEofBothDirections();
    testReconnectIsolation();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("tcp_byte_proxy_test: all checks passed\n");
    return 0;
}
