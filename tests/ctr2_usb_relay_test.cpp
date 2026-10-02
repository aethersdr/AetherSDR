// Ctr2UsbRelay link state machine with an injected HID port (no device) and
// a generic loopback byte peer standing in for the radio's TCP endpoint.
// Binds 127.0.0.1 ephemeral TCP ports only; exits 77 if loopback is refused.

#include "core/Ctr2HidFraming.h"
#include "core/Ctr2HidPort.h"
#include "core/Ctr2UsbRelay.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTcpServer>
#include <QNetworkDatagram>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <random>
#include <vector>

using namespace AetherSDR;
using namespace AetherSDR::ctr2hid;
using State = Ctr2UsbRelay::State;

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

// Scripted HID port. Reports the relay sends are decoded as the CTR2 would.
class FakePort : public Ctr2HidPort {
public:
    bool open{true};
    bool autoAck{true};
    int unacked{0};
    std::vector<Report> sent;
    std::vector<Report> finalReports;
    bool closedAfter{false};

    bool isOpen() const override { return open; }
    void send(const std::vector<Report>& reports) override
    {
        sent.insert(sent.end(), reports.begin(), reports.end());
        unacked += static_cast<int>(reports.size());
        if (autoAck) {
            ack(unacked);
        }
    }
    void close() override { open = false; }
    void closeAfter(const std::vector<Report>& final) override
    {
        finalReports = final;
        closedAfter = true;
        open = false;
    }
    QString description() const override { return QStringLiteral("Fake CTR2"); }

    void ack(int n)
    {
        n = std::min(n, unacked);
        if (n <= 0) {
            return;
        }
        unacked -= n;
        QTimer::singleShot(0, this, [this, n] { emit reportsSent(n); });
    }
    void deliver(const std::vector<Report>& reports)
    {
        QByteArray bytes;
        for (const Report& r : reports) {
            bytes.append(reinterpret_cast<const char*>(r.data()), kReportBytes);
        }
        emit reportsReceived(bytes);
    }
    void failNow(const QString& msg)
    {
        open = false;
        emit failed(msg);
    }
};

// What the CTR2 firmware would see and send.
struct Device {
    FakePort* port;
    FrameEncoder tx;
    FrameReassembler rx;
    size_t decoded{0};
    std::vector<Message> messages;
    QByteArray data;
    std::vector<Message> datagrams;
    bool rxError{false};

    void pump()
    {
        for (; decoded < port->sent.size(); ++decoded) {
            std::vector<Message> m;
            if (!rx.feed(port->sent[decoded], &m)) {
                rxError = true;
                rx.reset();
            }
            for (const Message& msg : m) {
                messages.push_back(msg);
                if (msg.type == MessageType::Data) {
                    data += msg.payload;
                } else if (msg.type == MessageType::Datagram) {
                    datagrams.push_back(msg);
                }
            }
        }
    }
    int count(MessageType t)
    {
        pump();
        int n = 0;
        for (const Message& m : messages) {
            n += m.type == t;
        }
        return n;
    }
    MessageType last()
    {
        pump();
        return messages.empty() ? MessageType::Data : messages.back().type;
    }
    void hello()
    {
        tx.reset();
        std::vector<Report> r;
        tx.encodeControl(MessageType::Hello, &r);
        port->deliver(r);
    }
    void closed()
    {
        std::vector<Report> r;
        tx.encodeControl(MessageType::Closed, &r);
        port->deliver(r);
    }
    void datagram(quint16 port, const QByteArray& bytes)
    {
        std::vector<Report> r;
        tx.encodeDatagram(port, bytes, &r);
        port_deliver(r);
    }
    void port_deliver(const std::vector<Report>& r) { port->deliver(r); }
    // Sends bytes as DATA messages, delivering reports in irregular batches.
    void send(const QByteArray& bytes, unsigned seed = 1)
    {
        std::vector<Report> r;
        tx.encodeData(bytes, &r);
        std::mt19937 rng(seed);
        size_t i = 0;
        while (i < r.size()) {
            const size_t n = std::min<size_t>(1 + rng() % 40, r.size() - i);
            port->deliver(std::vector<Report>(r.begin() + static_cast<long>(i),
                                              r.begin() + static_cast<long>(i + n)));
            i += n;
        }
    }
};

// Generic radio-side byte peer.
class RadioPeer {
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

    QTcpServer server;
    QByteArray greeting;
    std::vector<std::shared_ptr<Conn>> conns;
};

class HangingSocket : public QTcpSocket {
public:
    using QTcpSocket::connectToHost;
    void connectToHost(const QString&, quint16, OpenMode = ReadWrite,
                       NetworkLayerProtocol = AnyIPProtocol) override
    {
    }
};

struct Rig {
    RadioPeer radio;
    Ctr2UsbRelay relay;
    FakePort* port{new FakePort};
    Device dev{port};

    explicit Rig(const QByteArray& greeting = {}, const Ctr2UsbRelay::Tuning& tuning = {})
    {
        radio.greeting = greeting;
        if (!radio.listen()) {
            std::fprintf(stderr, "SKIP: cannot bind loopback\n");
            std::exit(kSkip);
        }
        relay.setTuning(tuning);
    }
    bool start() { return relay.start(port, QHostAddress::LocalHost, radio.server.serverPort()); }
    bool linkUp()
    {
        const int readies = dev.count(MessageType::Ready);
        dev.hello();
        return waitUntil([this, readies] { return dev.count(MessageType::Ready) == readies + 1; });
    }
};

void testRelayBothWays()
{
    const QByteArray greeting = randomBytes(900, 1) + QByteArray("\r\n\0\xff", 4);
    Rig rig(greeting);
    check(rig.start() && rig.relay.state() == State::Listening, "starts waiting for HELLO");
    check(rig.radio.conns.empty(), "no radio connection before HELLO");
    check(rig.port->sent.empty(), "nothing is sent to the CTR2 before HELLO");
    check(rig.linkUp(), "HELLO opens a radio connection and READY follows");
    check(rig.radio.conns.size() == 1, "exactly one radio connection");
    check(rig.relay.state() == State::Relaying, "state is Relaying");
    rig.dev.pump();
    check(!rig.dev.messages.empty() && rig.dev.messages.front().type == MessageType::Ready,
          "READY is the first message to the CTR2");
    check(rig.port->sent.front()[2] == 0, "READY starts the host counter at 0");
    check(waitUntil([&] { rig.dev.pump(); return rig.dev.data.size() >= greeting.size(); }),
          "radio output that arrived before READY follows it");

    const QByteArray up = randomBytes(30000, 2);
    const QByteArray down = randomBytes(40000, 3);
    rig.dev.send(up, 4);
    rig.radio.conns[0]->socket->write(down);
    check(waitUntil([&] {
              rig.dev.pump();
              return rig.radio.conns[0]->received.size() >= up.size()
                  && rig.dev.data.size() >= greeting.size() + down.size();
          }, 20000),
          "both directions complete");
    check(rig.radio.conns[0]->received == up, "CTR2 -> radio bytes are identical");
    check(rig.dev.data == greeting + down, "radio -> CTR2 bytes are identical");
    check(!rig.dev.rxError, "every report the relay sent is valid framing");
    bool sevenBit = true;
    for (const Report& r : rig.port->sent) {
        sevenBit = sevenBit && (r[0] == kMarker || r[0] <= kCounterMask);
    }
    check(sevenBit, "the host never sends a data report starting above 0x7F");
    const Ctr2UsbRelay::Stats s = rig.relay.stats();
    check(s.toUpstream == static_cast<quint64>(up.size())
              && s.toDownstream == static_cast<quint64>(greeting.size() + down.size()),
          "counters match the bytes relayed");
    rig.relay.stop();
    check(rig.port->closedAfter && rig.port->finalReports.size() == 1
              && rig.port->finalReports[0][3] == static_cast<std::uint8_t>(MessageType::Closed),
          "Stop sends CLOSED to the CTR2, then closes the port");
    check(waitUntil([&] { return rig.radio.conns[0]->disconnected; }), "Stop closes the radio connection");
    check(rig.relay.state() == State::Stopped, "state is Stopped");
}

void testRadioCloseDrainsThenClosed()
{
    Rig rig;
    rig.start();
    check(rig.linkUp(), "link up");
    const QByteArray tail = randomBytes(12000, 5);
    rig.radio.conns[0]->socket->write(tail);
    rig.radio.conns[0]->socket->disconnectFromHost();
    check(waitUntil([&] { return rig.dev.count(MessageType::Closed) == 1; }, 10000),
          "CTR2 receives CLOSED when the radio closes");
    rig.dev.pump();
    check(rig.dev.data == tail && rig.dev.last() == MessageType::Closed,
          "every radio byte arrives, and CLOSED comes after the last one");
    check(waitUntil([&] { return rig.relay.state() == State::Listening; }), "back to waiting for HELLO");
    check(rig.relay.lastError().isEmpty(), "an orderly radio close is not an error");
    rig.dev.send(QByteArray("sent before CTR2 saw CLOSED\n"));
    spin(50);
    check(rig.dev.count(MessageType::Closed) == 1 && rig.relay.lastError().isEmpty(),
          "data in flight after a radio close is dropped quietly, not a fault");
    check(rig.linkUp() && rig.radio.conns.size() == 2, "HELLO reconnects after a radio close");
}

void testDeviceCloseDrainsToRadio()
{
    Rig rig;
    rig.start();
    check(rig.linkUp(), "link up");
    const QByteArray last = randomBytes(3000, 6);
    rig.dev.send(last, 7);
    rig.dev.closed();
    check(waitUntil([&] { return rig.radio.conns[0]->disconnected; }), "radio connection closes after device CLOSED");
    check(rig.radio.conns[0]->received == last, "bytes sent before CLOSED reach the radio");
    check(rig.dev.count(MessageType::Closed) == 0, "no CLOSED is echoed back to the CTR2");
    check(waitUntil([&] { return rig.relay.state() == State::Listening; }), "waiting for HELLO");
}

void testHelloRestartsWithFreshConnection()
{
    Rig rig;
    rig.start();
    check(rig.linkUp(), "first link up");
    rig.dev.send(QByteArray("first\n"));
    check(waitUntil([&] { return rig.radio.conns[0]->received == "first\n"; }), "first bytes");
    const quint64 gen = rig.relay.linkGeneration();
    rig.dev.hello();  // CTR2 restarted
    check(waitUntil([&] { return rig.radio.conns.size() == 2 && rig.dev.count(MessageType::Ready) == 2; }),
          "a new HELLO opens a new radio connection and a new READY");
    check(waitUntil([&] { return rig.radio.conns[0]->disconnected; }), "the old radio connection is closed");
    check(rig.relay.linkGeneration() > gen, "new link generation");
    rig.radio.conns[0]->socket->write("stale");
    rig.dev.send(QByteArray("second\n"));
    check(waitUntil([&] { return rig.radio.conns[1]->received == "second\n"; }), "second bytes on the new connection");
    spin(100);
    rig.dev.pump();
    check(!rig.dev.data.contains("stale"), "nothing from the old connection reaches the CTR2");
    check(rig.radio.conns[0]->received == "first\n", "nothing new reaches the old connection");
}

void testFramingFaultClosesRadio()
{
    Rig rig;
    rig.start();
    check(rig.linkUp(), "link up");
    rig.port->deliver({Report{0x12, 1, 2, 3, 4, 5, 6, 7}});  // data report where a header belongs
    check(waitUntil([&] { return rig.dev.count(MessageType::Closed) == 1; }), "framing fault sends CLOSED");
    check(waitUntil([&] { return rig.radio.conns[0]->disconnected; }), "framing fault closes the radio connection");
    check(rig.relay.lastError().contains(QStringLiteral("framing")), "fault is reported locally");
    check(rig.relay.state() == State::Listening, "waiting for HELLO");
    rig.dev.send(QByteArray("in flight\n"));
    spin(50);
    check(rig.dev.count(MessageType::Closed) == 1 && rig.radio.conns.size() == 1,
          "data in flight before the CTR2 saw CLOSED is dropped quietly");
    check(rig.linkUp() && rig.radio.conns.size() == 2, "HELLO recovers the link");
}

void testProtocolViolations()
{
    {
        Rig rig;
        rig.relay.setRadioSocketFactory([] { return new HangingSocket; });
        rig.start();
        rig.dev.hello();
        check(waitUntil([&] { return rig.relay.state() == State::Connecting; }), "connecting");
        rig.dev.send(QByteArray("too early\n"));
        check(waitUntil([&] { return rig.dev.count(MessageType::Closed) == 1; }), "DATA before READY is a fault");
    }
    {
        Rig rig;
        rig.start();
        check(rig.linkUp(), "link up");
        std::vector<Report> r;
        rig.dev.tx.encodeControl(MessageType::Ready, &r);
        rig.port->deliver(r);
        check(waitUntil([&] { return rig.dev.count(MessageType::Closed) == 1; }), "READY from the CTR2 is a fault");
    }
}

void testRadioUnavailableAndTimeout()
{
    {
        quint16 dead = 0;
        {
            QTcpServer probe;
            if (!probe.listen(QHostAddress::LocalHost, 0)) {
                std::exit(kSkip);
            }
            dead = probe.serverPort();
        }
        Ctr2UsbRelay relay;
        auto* port = new FakePort;
        Device dev{port};
        relay.start(port, QHostAddress::LocalHost, dead);
        dev.hello();
        check(waitUntil([&] { return dev.count(MessageType::Closed) == 1; }), "refused radio sends CLOSED");
        check(dev.count(MessageType::Ready) == 0, "no READY without a radio connection");
        check(relay.lastError().contains(QStringLiteral("Radio connection failed")), "refusal reported");
        check(relay.state() == State::Listening, "still waiting for HELLO");
    }
    {
        Ctr2UsbRelay::Tuning t;
        t.connectTimeoutMs = 80;
        Rig rig({}, t);
        rig.relay.setRadioSocketFactory([] { return new HangingSocket; });
        rig.start();
        rig.dev.hello();
        check(waitUntil([&] { return rig.dev.count(MessageType::Closed) == 1; }, 2000), "connect timeout sends CLOSED");
        check(rig.relay.lastError().contains(QStringLiteral("timed out")), "timeout reported");
    }
}

void testIncompleteMessageTimeout()
{
    Ctr2UsbRelay::Tuning t;
    t.incompleteMessageTimeoutMs = 60;
    Rig rig({}, t);
    rig.start();
    check(rig.linkUp(), "link up");
    std::vector<Report> r;
    rig.dev.tx.encodeData(QByteArray(20, 'x'), &r);  // header + 3 data reports
    r.resize(2);                                     // stop part-way
    rig.port->deliver(r);
    check(waitUntil([&] { return rig.dev.count(MessageType::Closed) == 1; }, 2000),
          "an incomplete message times out and ends the link");
    check(rig.radio.conns[0]->received.isEmpty(), "no partial message reaches the radio");
}

void testPortFailure()
{
    Rig rig;
    rig.start();
    check(rig.linkUp(), "link up");
    rig.port->failNow(QStringLiteral("unplugged"));
    check(waitUntil([&] { return rig.radio.conns[0]->disconnected; }), "unplug closes the radio connection");
    check(rig.relay.state() == State::Error && rig.relay.lastError() == QStringLiteral("unplugged"),
          "unplug is reported");
}

void testBackpressureTowardDevice()
{
    Rig rig;
    rig.port->autoAck = false;
    rig.start();
    rig.dev.hello();
    check(waitUntil([&] { return rig.relay.state() == State::Relaying; }), "relaying");
    const QByteArray flood = randomBytes(100000, 9);
    rig.radio.conns[0]->socket->write(flood);
    spin(200);
    const qint64 budget = Ctr2UsbRelay::Tuning{}.relay.directionBudget;
    check(rig.relay.stats().queuedToDownstream <= budget,
          "bytes queued toward an unread CTR2 stay within the budget");
    QElapsedTimer deadline;
    deadline.start();
    while (rig.dev.data.size() < flood.size() && g_failures == 0 && deadline.elapsed() < 30000) {
        rig.port->ack(20);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        rig.dev.pump();
        check(rig.relay.stats().queuedToDownstream <= budget, "queue stays bounded while draining");
    }
    if (rig.dev.data != flood) {
        std::fprintf(stderr, "  got %lld of %lld, unacked %d, state %d, err '%s', rxError %d\n",
                     static_cast<long long>(rig.dev.data.size()), static_cast<long long>(flood.size()),
                     rig.port->unacked, static_cast<int>(rig.relay.state()),
                     qPrintable(rig.relay.lastError()), rig.dev.rxError);
    }
    check(rig.dev.data == flood, "throttled stream arrives complete and in order");
}

// Generic radio-side UDP endpoint.
struct RadioUdp {
    QUdpSocket socket;
    std::vector<QNetworkDatagram> received;

    bool bind()
    {
        QObject::connect(&socket, &QUdpSocket::readyRead, &socket, [this] {
            while (socket.hasPendingDatagrams()) {
                received.push_back(socket.receiveDatagram());
            }
        });
        return socket.bind(QHostAddress::LocalHost, 0);
    }
    quint16 port() const { return socket.localPort(); }
};

void testDatagramsBothWays()
{
    Rig rig;
    RadioUdp udp;
    if (!udp.bind()) {
        std::exit(kSkip);
    }
    rig.start();
    check(rig.linkUp(), "link up");
    const QByteArray reg(1, '\0');
    const QByteArray big = randomBytes(kMaxDatagramBytes, 21);
    rig.dev.datagram(udp.port(), reg);
    rig.dev.send(QByteArray("tcp in between\n"));
    rig.dev.datagram(udp.port(), big);
    check(waitUntil([&] { return udp.received.size() == 2; }), "both CTR2 datagrams reach the radio");
    check(udp.received.size() == 2 && udp.received[0].data() == reg && udp.received[1].data() == big,
          "datagram boundaries and bytes are exact");
    check(waitUntil([&] { return rig.radio.conns[0]->received == "tcp in between\n"; }),
          "TCP interleaved with datagrams is intact");
    const QHostAddress hostAddr = udp.received[0].senderAddress();
    const quint16 hostPort = static_cast<quint16>(udp.received[0].senderPort());
    check(udp.received[1].senderPort() == hostPort, "one host UDP socket per link");

    const QByteArray meter1 = randomBytes(300, 22);
    const QByteArray meter2("\x00\xff\x0a", 3);
    udp.socket.writeDatagram(meter1, hostAddr, hostPort);
    udp.socket.writeDatagram(meter2, hostAddr, hostPort);
    check(waitUntil([&] { rig.dev.pump(); return rig.dev.datagrams.size() == 2; }),
          "radio datagrams reach the CTR2");
    check(rig.dev.datagrams.size() == 2 && rig.dev.datagrams[0].payload == meter1
              && rig.dev.datagrams[1].payload == meter2
              && rig.dev.datagrams[0].port == udp.port(),
          "radio datagrams arrive whole, with the radio's source port");

    QUdpSocket stranger;
    if (stranger.bind(QHostAddress(QStringLiteral("127.0.0.2")), 0)) {
        stranger.writeDatagram(QByteArray("spoof"), hostAddr, hostPort);
        spin(100);
        rig.dev.pump();
        check(rig.dev.datagrams.size() == 2, "datagrams from anyone but the radio are ignored");
    }
    const Ctr2UsbRelay::Stats s = rig.relay.stats();
    check(s.datagramsToRadio == 2 && s.datagramsToDevice == 2 && s.datagramsDropped == 0,
          "datagram counters match");
}

void testDatagramFloodIsDropped()
{
    Rig rig;
    RadioUdp udp;
    if (!udp.bind()) {
        std::exit(kSkip);
    }
    rig.port->autoAck = false;
    rig.start();
    rig.dev.hello();
    check(waitUntil([&] { return rig.relay.state() == State::Relaying; }), "relaying");
    rig.port->ack(rig.port->unacked);
    rig.dev.datagram(udp.port(), QByteArray(1, '\0'));
    check(waitUntil([&] { return udp.received.size() == 1; }), "registration datagram arrives");
    const QHostAddress host = udp.received[0].senderAddress();
    const quint16 hostPort = static_cast<quint16>(udp.received[0].senderPort());
    for (int i = 0; i < 50; ++i) {
        udp.socket.writeDatagram(randomBytes(1000, static_cast<unsigned>(i)), host, hostPort);
    }
    rig.radio.conns[0]->socket->write("after the flood\n");
    check(waitUntil([&] { const auto st = rig.relay.stats(); return st.datagramsToDevice + st.datagramsDropped == 50; }),
          "every flood datagram is either queued or dropped");
    check(rig.relay.stats().datagramsDropped > 0, "a backed-up link drops datagrams instead of growing");
    QElapsedTimer deadline;
    deadline.start();
    while (!rig.dev.data.contains("after the flood\n") && deadline.elapsed() < 20000) {
        rig.port->ack(20);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        rig.dev.pump();
    }
    check(rig.dev.data == "after the flood\n", "TCP still arrives intact after a UDP flood");
    check(static_cast<quint64>(rig.dev.datagrams.size()) == rig.relay.stats().datagramsToDevice,
          "every datagram counted as sent reaches the CTR2 whole");
}

void testDatagramBeforeReady()
{
    Rig rig;
    rig.relay.setRadioSocketFactory([] { return new HangingSocket; });
    rig.start();
    rig.dev.hello();
    check(waitUntil([&] { return rig.relay.state() == State::Connecting; }), "connecting");
    rig.dev.datagram(4992, QByteArray(1, '\0'));
    check(waitUntil([&] { return rig.dev.count(MessageType::Closed) == 1; }), "a datagram before READY is a fault");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testRelayBothWays();
    testRadioCloseDrainsThenClosed();
    testDeviceCloseDrainsToRadio();
    testHelloRestartsWithFreshConnection();
    testFramingFaultClosesRadio();
    testProtocolViolations();
    testRadioUnavailableAndTimeout();
    testIncompleteMessageTimeout();
    testPortFailure();
    testBackpressureTowardDevice();
    testDatagramsBothWays();
    testDatagramFloodIsDropped();
    testDatagramBeforeReady();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ctr2_usb_relay_test: all checks passed\n");
    return 0;
}
