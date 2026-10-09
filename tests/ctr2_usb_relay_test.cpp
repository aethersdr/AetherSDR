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
#include <deque>
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

// Scripted HID port. Reports wait in `queue` until acknowledged, then are
// "delivered" to `sent`, which the Device decodes as the CTR2 would.
class FakePort : public Ctr2HidPort {
public:
    bool open{true};
    bool autoAck{true};
    std::vector<Report> sent;
    std::deque<Report> queue;
    std::vector<Report> finalReports;
    bool shutDown{false};
    int discards{0};

    int pending() const { return static_cast<int>(queue.size()); }
    bool isOpen() const override { return open; }
    void send(const std::vector<Report>& reports) override
    {
        queue.insert(queue.end(), reports.begin(), reports.end());
        if (autoAck) {
            ack(pending());
        }
    }
    void discardQueued() override
    {
        queue.clear();
        ++m_generation;
        ++discards;
    }
    void shutdown(const std::vector<Report>& final) override
    {
        finalReports = final;
        sent.insert(sent.end(), final.begin(), final.end());
        shutDown = true;
        open = false;
        deleteLater();
    }
    QString description() const override { return QStringLiteral("Fake CTR2"); }

    // Delivers up to n queued reports; the acknowledgement arrives later and
    // is dropped if a fence happened in between, as the interface requires.
    void ack(int n)
    {
        n = std::min(n, pending());
        if (n <= 0) {
            return;
        }
        for (int i = 0; i < n; ++i) {
            sent.push_back(queue.front());
            queue.pop_front();
        }
        const quint64 generation = m_generation;
        QTimer::singleShot(0, this, [this, n, generation] {
            if (generation == m_generation) {
                emit reportsSent(n);
            }
        });
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

private:
    quint64 m_generation{0};
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
    bool awaitingReady{false};
    int ignoredBeforeReady{0};
    int answeredHellos{0};

    void pump()
    {
        for (; decoded < port->sent.size(); ++decoded) {
            const Report& r = port->sent[decoded];
            // Spec: after sending HELLO or READY the device skips reports until
            // a READY, CLOSED or HELLO header, and starts its receiver fresh there.
            if (awaitingReady) {
                const bool answer = r[0] == kMarker
                    && (r[3] == static_cast<std::uint8_t>(MessageType::Ready)
                        || r[3] == static_cast<std::uint8_t>(MessageType::Closed)
                        || r[3] == static_cast<std::uint8_t>(MessageType::Hello));
                if (!answer) {
                    ++ignoredBeforeReady;
                    continue;
                }
                awaitingReady = false;
                rx.reset();
            }
            std::vector<Message> m;
            if (!rx.feed(r, &m)) {
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
    // The last message other than the host's HELLO calls, which may follow
    // any link's end.
    MessageType lastBesidesHello()
    {
        pump();
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if (it->type != MessageType::Hello) {
                return it->type;
            }
        }
        return MessageType::Data;
    }
    void hello()
    {
        pump();
        awaitingReady = true;
        tx.reset();
        std::vector<Report> r;
        tx.encodeControl(MessageType::Hello, &r);
        port->deliver(r);
    }
    // The CTR2's answer to the host's HELLO.
    void answer()
    {
        answeredHellos = count(MessageType::Hello);
        awaitingReady = true;
        tx.reset();
        std::vector<Report> r;
        tx.encodeControl(MessageType::Ready, &r);
        port->deliver(r);
    }
    // Waits for a HELLO from the host not yet answered (delivering anything
    // held on the port), then answers it: the normal way a link comes up.
    bool answerCall()
    {
        if (!waitUntil([this] {
                port->ack(port->pending());
                return count(MessageType::Hello) > answeredHellos;
            })) {
            return false;
        }
        answer();
        return true;
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

// The host calls quickly in tests, so a link comes up without waiting a second.
Ctr2UsbRelay::Tuning fastCalls()
{
    Ctr2UsbRelay::Tuning t;
    t.helloIntervalMs = 20;
    t.helloRetryAfterFaultMs = 60;
    return t;
}

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

// Connects normally but never accepts a written byte, so a drain toward the
// radio cannot finish until the relay's drain timeout.
class StalledWriteSocket : public QTcpSocket {
protected:
    qint64 writeData(const char*, qint64) override { return 0; }
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

    explicit Rig(const QByteArray& greeting = {}, const Ctr2UsbRelay::Tuning& tuning = fastCalls())
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
        if (!dev.answerCall()) {
            return false;
        }
        return waitUntil([this, readies] { return dev.count(MessageType::Ready) == readies + 1; });
    }
};

void testRelayBothWays()
{
    const QByteArray greeting = randomBytes(900, 1) + QByteArray("\r\n\0\xff", 4);
    Rig rig(greeting);
    check(rig.start() && rig.relay.state() == State::Listening, "starts calling the CTR2");
    check(rig.radio.conns.empty(), "no radio connection before the CTR2 answers");
    rig.dev.pump();
    check(rig.dev.messages.size() == 1 && rig.dev.messages.front().type == MessageType::Hello
              && rig.port->sent.front()[2] == 0,
          "Start calls the CTR2 with HELLO (counter 0) and nothing else");
    check(rig.linkUp(), "HELLO opens a radio connection and READY follows");
    check(rig.radio.conns.size() == 1, "exactly one radio connection");
    check(rig.relay.state() == State::Relaying, "state is Relaying");
    rig.dev.pump();
    const Report* readyReport = nullptr;
    for (const Report& r : rig.port->sent) {
        if (r[0] == kMarker && r[3] == static_cast<std::uint8_t>(MessageType::Ready)) {
            readyReport = &r;
            break;
        }
    }
    check(readyReport && (*readyReport)[2] == 0, "READY starts the host counter at 0");
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
    check(rig.port->shutDown && rig.port->finalReports.size() == 1
              && rig.port->finalReports[0][3] == static_cast<std::uint8_t>(MessageType::Closed),
          "Stop hands the port its final CLOSED and shuts it down without waiting");
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
    check(rig.dev.data == tail && rig.dev.lastBesidesHello() == MessageType::Closed,
          "every radio byte arrives, and CLOSED comes after the last one");
    check(waitUntil([&] { return rig.relay.state() == State::Listening; }), "back to calling the CTR2");
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
    check(waitUntil([&] { return rig.relay.state() == State::Listening; }), "calling the CTR2 again");
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
    check(rig.relay.state() == State::Listening, "calling the CTR2 again");
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
        rig.dev.answerCall();
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
        spin(100);
        check(rig.dev.count(MessageType::Closed) == 0 && rig.radio.conns.size() == 1
                  && rig.relay.state() == State::Relaying,
              "READY from the CTR2 during a link is a late answer and is ignored");
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
        dev.answerCall();
        check(waitUntil([&] { return dev.count(MessageType::Closed) == 1; }), "refused radio sends CLOSED");
        check(dev.count(MessageType::Ready) == 0, "no READY without a radio connection");
        check(relay.lastError().contains(QStringLiteral("Radio connection failed")), "refusal reported");
        check(relay.state() == State::Listening, "still calling the CTR2");
    }
    {
        Ctr2UsbRelay::Tuning t = fastCalls();
        t.connectTimeoutMs = 80;
        Rig rig({}, t);
        rig.relay.setRadioSocketFactory([] { return new HangingSocket; });
        rig.start();
        rig.dev.answerCall();
        check(waitUntil([&] { return rig.dev.count(MessageType::Closed) == 1; }, 2000), "connect timeout sends CLOSED");
        check(rig.relay.lastError().contains(QStringLiteral("timed out")), "timeout reported");
    }
}

void testIncompleteMessageTimeout()
{
    Ctr2UsbRelay::Tuning t = fastCalls();
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
    rig.dev.answerCall();
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
                     rig.port->pending(), static_cast<int>(rig.relay.state()),
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
    rig.dev.answerCall();
    check(waitUntil([&] { return rig.relay.state() == State::Relaying; }), "relaying");
    rig.port->ack(rig.port->pending());
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
    rig.dev.answerCall();
    check(waitUntil([&] { return rig.relay.state() == State::Connecting; }), "connecting");
    rig.dev.datagram(4992, QByteArray(1, '\0'));
    check(waitUntil([&] { return rig.dev.count(MessageType::Closed) == 1; }), "a datagram before READY is a fault");
}

void testDeviceLabels()
{
    Ctr2HidPort::DeviceInfo max{QStringLiteral("p"), 0x303A, 0x1001,
                                QStringLiteral("Espressif Systems"), QStringLiteral("ESP32S3_DEV"), {}};
    Ctr2HidPort::DeviceInfo dial{QStringLiteral("p"), 0x303A, 0x1001, {}, QStringLiteral("M5STACK_DIAL"), {}};
    Ctr2HidPort::DeviceInfo midi{QStringLiteral("p"), 0x2886, 0x0056, {}, QStringLiteral("XIAO_ESP32S3"), {}};
    Ctr2HidPort::DeviceInfo knob{QStringLiteral("p"), 0x303A, 0x1001, {}, QStringLiteral("AETHER_KNOB"), {}};
    Ctr2HidPort::DeviceInfo devBoard{QStringLiteral("p"), 0x303A, 0x1001, {}, QStringLiteral("Some_S3_Gadget"), {}};
    Ctr2HidPort::DeviceInfo wrongVid{QStringLiteral("p"), 0x1234, 0x1001, {}, QStringLiteral("ESP32S3_DEV"), {}};
    Ctr2HidPort::DeviceInfo other{QStringLiteral("p"), 0x04F3, 0x32BC, {}, QStringLiteral("Touchpad"), {}};
    check(max.label() == QStringLiteral("CTR2-Max / Nano: Espressif Systems ESP32S3_DEV (303a:1001)"),
          "CTR2-Max/Nano product string is named");
    check(dial.ctr2Model() == QStringLiteral("CTR2 (M5Dial)") && midi.ctr2Model() == QStringLiteral("CTR2-MIDI"),
          "M5Dial and MIDI product strings are named");
    check(knob.ctr2Model() == QStringLiteral("AetherKnob"), "the AetherKnob product string is named");
    check(other.ctr2Model().isEmpty() && other.label() == QStringLiteral("Touchpad (04f3:32bc)"),
          "other devices keep their plain label");
    check(devBoard.ctr2Model().isEmpty() && wrongVid.ctr2Model().isEmpty(),
          "a CTR2 needs both its USB IDs and its product string");
}

// Brings the link up with acknowledgements held: answer, then deliver READY.
bool linkUpHeld(Rig& rig)
{
    if (!rig.dev.answerCall()) {
        return false;
    }
    if (!waitUntil([&] { return rig.relay.state() == State::Relaying; })) {
        return false;
    }
    rig.port->ack(rig.port->pending());
    return waitUntil([&] { return rig.dev.count(MessageType::Ready) >= 1; });
}

void testRestartDiscardsQueuedOutput()
{
    Rig rig;
    rig.port->autoAck = false;
    rig.start();
    check(linkUpHeld(rig), "link up with acknowledgements held");
    const QByteArray burst = randomBytes(6000, 31);
    rig.radio.conns[0]->socket->write(burst);
    check(waitUntil([&] { return rig.port->pending() > 20; }), "old radio output is queued on the port");
    rig.port->ack(3);  // the CTR2 has received part of one DATA message
    QCoreApplication::processEvents();
    const int discardsBefore = rig.port->discards;

    rig.dev.hello();  // the CTR2 restarts mid-message
    check(rig.port->discards == discardsBefore + 1, "HELLO fences the port before the new link");
    check(waitUntil([&] { return rig.radio.conns.size() == 2 && rig.port->pending() >= 1; }),
          "a fresh radio connection opens");
    check(rig.port->pending() == 1 && rig.port->queue.front()[0] == kMarker
              && rig.port->queue.front()[2] == 0
              && rig.port->queue.front()[3] == static_cast<std::uint8_t>(MessageType::Ready),
          "only the new READY is queued; no old report precedes it");
    check(rig.relay.stats().queuedToDownstream == 0, "the new link's budget carries no old output");
    const QByteArray before = rig.dev.data;
    rig.port->ack(rig.port->pending());
    rig.radio.conns[1]->socket->write("fresh\n");
    QElapsedTimer t;
    t.start();
    while (!rig.dev.data.endsWith("fresh\n") && t.elapsed() < 5000) {
        rig.port->ack(rig.port->pending());
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        rig.dev.pump();
    }
    check(rig.dev.data == before + "fresh\n", "after READY only the new connection's bytes arrive");
    check(!rig.dev.rxError, "the device resynchronizes on READY without a framing error");

    for (int i = 0; i < 20; ++i) {
        rig.radio.conns.back()->socket->write(randomBytes(2000, static_cast<unsigned>(i)));
        waitUntil([&] { return rig.port->pending() > 5; }, 500);
        rig.dev.hello();
        waitUntil([&] { return rig.relay.state() == State::Relaying; });
    }
    check(rig.port->pending() <= 2, "restarts with a stalled port never accumulate output");
}

void testEachMessageGetsItsOwnDeadline()
{
    Ctr2UsbRelay::Tuning t = fastCalls();
    t.incompleteMessageTimeoutMs = 300;
    Rig rig({}, t);
    rig.start();
    check(rig.linkUp(), "link up");
    std::vector<Report> a;
    std::vector<Report> b;
    rig.dev.tx.encodeData(QByteArray(14, 'A'), &a);  // header + 2 data reports
    rig.dev.tx.encodeData(QByteArray(14, 'B'), &b);
    rig.port->deliver({a[0], a[1]});
    spin(200);
    rig.port->deliver({a[2], b[0]});  // A completes and B begins in one batch
    spin(200);                        // 400 ms after A began, 200 ms into B
    check(rig.dev.count(MessageType::Closed) == 0, "B is not judged by A's deadline");
    rig.port->deliver({b[1], b[2]});
    check(waitUntil([&] { return rig.radio.conns[0]->received == QByteArray(14, 'A') + QByteArray(14, 'B'); }),
          "both messages reach the radio");

    std::vector<Report> c;
    rig.dev.tx.encodeData(QByteArray(42, 'C'), &c);  // header + 6 data reports
    QElapsedTimer clock;
    clock.start();
    for (size_t i = 0; i < c.size() && rig.dev.count(MessageType::Closed) == 0; ++i) {
        rig.port->deliver({c[i]});
        spin(80);
    }
    check(rig.dev.count(MessageType::Closed) == 1 && clock.elapsed() < 560,
          "one message trickling in still has a single absolute deadline");
}

void testControlOutputBoundedWhileWaiting()
{
    Ctr2UsbRelay::Tuning fast;
    fast.helloIntervalMs = 20;
    Rig rig({}, fast);
    rig.port->autoAck = false;
    rig.start();
    spin(300);
    check(rig.port->pending() == 1, "HELLO retries never stack up on a CTR2 that is not reading");
    check(linkUpHeld(rig), "link up");
    const int before = rig.port->pending();
    for (int i = 0; i < 1000; ++i) {
        rig.port->deliver({Report{0x12, 1, 2, 3, 4, 5, 6, 7}});
    }
    check(rig.port->pending() == before + 1, "repeated faults queue a single CLOSED");
    spin(200);
    check(rig.port->pending() == before + 1, "no HELLO is queued behind an undelivered CLOSED");

    // A refused radio sends CLOSED; a message the CTR2 already had in flight
    // then times out half-received. That second fault must not add a CLOSED.
    quint16 dead = 0;
    {
        QTcpServer probe;
        if (!probe.listen(QHostAddress::LocalHost, 0)) {
            std::exit(kSkip);
        }
        dead = probe.serverPort();
    }
    Ctr2UsbRelay::Tuning tuning;
    tuning.incompleteMessageTimeoutMs = 60;
    Ctr2UsbRelay relay;
    relay.setTuning(tuning);
    auto* port = new FakePort;
    Device dev{port};
    relay.start(port, QHostAddress::LocalHost, dead);
    dev.answerCall();
    check(waitUntil([&] { return dev.count(MessageType::Closed) == 1; }), "refused radio: one CLOSED");
    std::vector<Report> inFlight;
    dev.tx.encodeData(QByteArray(20, 'x'), &inFlight);
    port->deliver({inFlight[0]});
    spin(250);
    check(dev.count(MessageType::Closed) == 1, "a later fault in the same episode sends no second CLOSED");
}

void testHostCallsAndDeviceAnswers()
{
    Ctr2UsbRelay::Tuning t;
    t.helloIntervalMs = 30;
    Rig rig(QByteArray("greeting\n"), t);
    rig.start();
    spin(200);
    const int hellos = rig.dev.count(MessageType::Hello);
    check(hellos >= 3, "HELLO is repeated until the CTR2 answers");
    rig.dev.answer();
    check(waitUntil([&] { return rig.dev.count(MessageType::Ready) == 1; }),
          "the CTR2's READY opens the radio connection and the host answers READY");
    check(rig.radio.conns.size() == 1 && rig.relay.state() == State::Relaying, "relaying");
    check(waitUntil([&] { rig.dev.pump(); return rig.dev.data == "greeting\n"; }),
          "radio output follows the host's READY");
    rig.dev.send(QByteArray("C1|info\n"));
    check(waitUntil([&] { return rig.radio.conns[0]->received == "C1|info\n"; }), "commands reach the radio");
    const int hellosAtLink = rig.dev.count(MessageType::Hello);
    spin(200);
    check(rig.dev.count(MessageType::Hello) == hellosAtLink, "no HELLO while the link is up");

    rig.dev.answer();  // a late answer to an earlier HELLO
    spin(100);
    check(rig.radio.conns.size() == 1 && rig.relay.state() == State::Relaying
              && rig.dev.count(MessageType::Closed) == 0 && !rig.radio.conns[0]->disconnected,
          "a late READY during a link changes nothing");
}

void testRetryBacksOffAfterRadioFailure()
{
    quint16 dead = 0;
    {
        QTcpServer probe;
        if (!probe.listen(QHostAddress::LocalHost, 0)) {
            std::exit(kSkip);
        }
        dead = probe.serverPort();
    }
    Ctr2UsbRelay::Tuning t;
    t.helloIntervalMs = 30;
    t.helloRetryAfterFaultMs = 400;
    Ctr2UsbRelay relay;
    relay.setTuning(t);
    auto* port = new FakePort;
    Device dev{port};
    relay.start(port, QHostAddress::LocalHost, dead);
    check(waitUntil([&] { return dev.count(MessageType::Hello) >= 1; }), "called");
    dev.answer();
    check(waitUntil([&] { return dev.count(MessageType::Closed) == 1; }), "unreachable radio: CLOSED");
    const int hellos = dev.count(MessageType::Hello);
    spin(250);
    check(dev.count(MessageType::Hello) == hellos, "no HELLO right after a radio failure");
    check(waitUntil([&] { return dev.count(MessageType::Hello) > hellos; }, 1000),
          "HELLO resumes after the back-off");
    const int resumed = dev.count(MessageType::Hello);
    spin(250);
    check(dev.count(MessageType::Hello) == resumed, "the back-off holds for every call, not just the first");
    check(waitUntil([&] { return dev.count(MessageType::Hello) > resumed; }, 1000), "and the calls continue");
    const int beforeStray = dev.count(MessageType::Hello);
    for (int i = 0; i < 12; ++i) {
        dev.closed();  // stray CLOSEDs with no link up
        spin(100);
    }
    check(dev.count(MessageType::Hello) > beforeStray, "stray CLOSEDs do not postpone the next call");
}

void testNoHelloWhileDraining()
{
    Ctr2UsbRelay::Tuning t;
    t.helloIntervalMs = 20;
    t.helloRetryAfterFaultMs = 100;
    t.relay.drainTimeoutMs = 600;
    Rig rig({}, t);
    rig.relay.setRadioSocketFactory([] { return new StalledWriteSocket; });
    rig.start();
    check(rig.linkUp(), "link up");
    const int hellos = rig.dev.count(MessageType::Hello);
    rig.dev.send(QByteArray("last command\n"));
    rig.dev.closed();  // the CTR2 ends the link with bytes still bound for the radio
    spin(300);
    check(rig.dev.count(MessageType::Hello) == hellos,
          "no HELLO while the previous session is still draining to the radio");
    check(rig.relay.state() == State::Closing, "the session is draining");
    check(waitUntil([&] { return rig.relay.state() == State::Listening; }, 2000),
          "the drain ends at its own timeout");
    check(waitUntil([&] { return rig.dev.count(MessageType::Hello) > hellos; }, 2000),
          "HELLO resumes once the session has ended");
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
    testDeviceLabels();
    testRestartDiscardsQueuedOutput();
    testEachMessageGetsItsOwnDeadline();
    testControlOutputBoundedWhileWaiting();
    testHostCallsAndDeviceAnswers();
    testRetryBacksOffAfterRadioFailure();
    testNoHelloWhileDraining();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ctr2_usb_relay_test: all checks passed\n");
    return 0;
}
