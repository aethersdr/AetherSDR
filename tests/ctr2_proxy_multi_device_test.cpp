// Ctr2ProxyModel relaying several USB CTR2s at once. Each device answers the
// host's call over an injected HID port and gets its own radio connection;
// bytes never cross between devices, one device failing leaves the others
// relaying, the audio spectrum goes only to devices that negotiated it, and
// Stop releases every device. The "radio" is a generic byte peer on an
// ephemeral 127.0.0.1 TCP port; exits 77 when loopback cannot be bound.

#include "core/Ctr2HidFraming.h"
#include "core/Ctr2HidPort.h"
#include "models/Ctr2ProxyModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

using AetherSDR::Ctr2HidPort;
using AetherSDR::Ctr2ProxyModel;
using AetherSDR::TcpByteProxy;
using namespace AetherSDR::ctr2hid;

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

// Injected HID port that acknowledges every report at once and decodes what
// the host sends, as the CTR2 firmware would.
class FakePort : public Ctr2HidPort {
public:
    std::vector<Message> received;
    QByteArray data;
    bool shutDown{false};
    bool closedOnShutdown{false};

    bool isOpen() const override { return !shutDown; }
    void send(const std::vector<Report>& reports) override
    {
        for (const Report& r : reports) {
            m_rx.feed(r, &received);
        }
        for (const Message& m : received) {
            if (m.type == MessageType::Data) {
                data += m.payload;
            }
        }
        m_kept.insert(m_kept.end(), received.begin(), received.end());
        received.clear();
        const int n = static_cast<int>(reports.size());
        QTimer::singleShot(0, this, [this, n] { emit reportsSent(n); });
    }
    void discardQueued() override {}
    void shutdown(const std::vector<Report>& final) override
    {
        for (const Report& r : final) {
            closedOnShutdown = closedOnShutdown
                || r[3] == static_cast<std::uint8_t>(MessageType::Closed);
        }
        shutDown = true;
        deleteLater();
    }
    QString description() const override { return QStringLiteral("Fake CTR2"); }

    void negotiate(quint32 extensions)
    {
        m_rx.setExtensions(extensions);
        setExtensions(extensions);
    }
    void deliver(const std::vector<Report>& reports)
    {
        QByteArray bytes;
        for (const Report& r : reports) {
            bytes.append(reinterpret_cast<const char*>(r.data()), kReportBytes);
        }
        emit reportsReceived(bytes);
    }
    // The CTR2's answer to the host's HELLO.
    void answer()
    {
        m_tx.reset();
        std::vector<Report> r;
        m_tx.encodeControl(MessageType::Ready, &r);
        deliver(r);
    }
    void sendData(const QByteArray& bytes)
    {
        std::vector<Report> r;
        m_tx.encodeData(bytes, &r);
        deliver(r);
    }
    void fail() { emit failed(QStringLiteral("device unplugged")); }
    int count(MessageType type) const
    {
        int n = 0;
        for (const Message& m : m_kept) {
            n += m.type == type ? 1 : 0;
        }
        return n;
    }

private:
    FrameReassembler m_rx;
    FrameEncoder m_tx;
    std::vector<Message> m_kept;
};

struct RadioPeer {
    struct Conn {
        QTcpSocket* socket{nullptr};
        QByteArray received;
        bool disconnected{false};
    };
    QTcpServer server;
    std::vector<std::shared_ptr<Conn>> conns;

    bool listen()
    {
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (QTcpSocket* s = server.nextPendingConnection()) {
                auto c = std::make_shared<Conn>();
                c->socket = s;
                conns.push_back(c);
                QObject::connect(s, &QIODevice::readyRead, s, [c] { c->received += c->socket->readAll(); });
                QObject::connect(s, &QAbstractSocket::disconnected, s, [c] { c->disconnected = true; });
            }
        });
        return server.listen(QHostAddress::LocalHost, 0);
    }
};

int linksIn(const Ctr2ProxyModel& model, TcpByteProxy::State state)
{
    int n = 0;
    for (const auto& link : model.links()) {
        n += link.state == state ? 1 : 0;
    }
    return n;
}

void testSeveralDevicesAtOnce()
{
    RadioPeer radio;
    if (!radio.listen()) {
        std::fprintf(stderr, "SKIP: cannot bind loopback\n");
        std::exit(kSkip);
    }
    using Info = Ctr2HidPort::DeviceInfo;
    const Info knob{QStringLiteral("knob"), 0x303A, 0x1001, {}, QStringLiteral("M5STACK_DIAL"), {}};
    const Info ctr2{QStringLiteral("ctr2"), 0x303A, 0x1001, {}, QStringLiteral("ESP32S3_DEV"), {}};
    std::vector<FakePort*> ports;   // in open order: knob, then ctr2
    Ctr2ProxyModel model;
    model.setUsbBackend([=] { return QList<Info>{knob, ctr2}; },
                        [&ports](const Info&, QString*) -> Ctr2HidPort* {
                            ports.push_back(new FakePort);
                            return ports.back();
                        });
    model.setWifiEnabled(false);
    model.setAetherRadio(QHostAddress::LocalHost, radio.server.serverPort(),
                         QStringLiteral("loopback radio"), {});
    check(model.start(), "both devices start");
    check(ports.size() == 2, "each device is opened");
    if (ports.size() != 2) {
        return;
    }
    FakePort* a = ports[0];
    FakePort* b = ports[1];
    check(a->count(MessageType::Hello) >= 1 && b->count(MessageType::Hello) >= 1,
          "the host calls every device");
    check(!model.audioSpectrumWanted(), "no spectrum before any device relays");

    a->negotiate(capabilityBit(MessageType::AudioSpectrum));
    a->answer();
    check(waitUntil([&] { return radio.conns.size() == 1 && a->count(MessageType::Ready) == 1; }),
          "the first device gets its own radio connection");
    b->answer();
    check(waitUntil([&] { return radio.conns.size() == 2 && b->count(MessageType::Ready) == 1; }),
          "the second device gets another radio connection");
    check(waitUntil([&] { return linksIn(model, TcpByteProxy::State::Relaying) == 2; }),
          "both devices relay at once");

    a->sendData("from-knob");
    b->sendData("from-ctr2");
    radio.conns[0]->socket->write("to-knob");
    radio.conns[1]->socket->write("to-ctr2");
    check(waitUntil([&] {
              return radio.conns[0]->received == "from-knob" && radio.conns[1]->received == "from-ctr2"
                  && a->data == "to-knob" && b->data == "to-ctr2";
          }),
          "each device's bytes travel only on its own radio connection");

    check(model.audioSpectrumWanted(), "a relaying device with the extension wants the spectrum");
    const std::vector<float> bars(24, -40.0f);
    check(model.sendAudioSpectrum(100, 3000, bars), "the spectrum is sent");
    check(waitUntil([&] { return a->count(MessageType::AudioSpectrum) == 1; }),
          "the device that negotiated it receives the spectrum");
    check(b->count(MessageType::AudioSpectrum) == 0, "a stock CTR2 never receives it");

    a->fail();
    check(waitUntil([&] { return radio.conns[0]->disconnected; }),
          "a failed device's radio connection closes");
    check(model.isRunning() && linksIn(model, TcpByteProxy::State::Relaying) == 1,
          "the other device keeps relaying");
    check(!model.audioSpectrumWanted(), "no spectrum once its only taker failed");
    b->sendData("still");
    check(waitUntil([&] { return radio.conns[1]->received == "from-ctr2still"; }),
          "bytes still flow for the surviving device");

    model.stop();
    // Checked before the event loop runs: the released port deletes itself.
    check(b->shutDown && b->closedOnShutdown, "Stop releases the device and tells it CLOSED");
    check(waitUntil([&] { return radio.conns[1]->disconnected; }), "Stop closes the radio connection");
    check(!model.isRunning() && model.links().isEmpty(), "nothing runs after Stop");
    QCoreApplication::processEvents();
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testSeveralDevicesAtOnce();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ctr2_proxy_multi_device_test: all checks passed\n");
    return 0;
}
