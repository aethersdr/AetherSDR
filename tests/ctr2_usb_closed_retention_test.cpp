// Injected transport only: the socket never opens, and HID output remains
// queued until the test acknowledges it. No device or synthetic firmware peer.
#include "core/Ctr2HidPort.h"
#include "core/Ctr2UsbRelay.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QTcpSocket>
#include <QTimer>

#include <cstdio>
#include <functional>
#include <vector>

using namespace AetherSDR;
using namespace AetherSDR::ctr2hid;

namespace {

int g_failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

bool waitUntil(const std::function<bool()>& predicate)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return predicate();
}

class StalledPort final : public Ctr2HidPort {
public:
    bool isOpen() const override { return true; }
    QString description() const override { return QStringLiteral("Injected HID port"); }

    void send(const std::vector<Report>& reports) override
    {
        pending.insert(pending.end(), reports.begin(), reports.end());
    }

    void discardQueued() override { pending.clear(); }
    void shutdown(const std::vector<Report>&) override { deleteLater(); }

    void deliver(const Report& report)
    {
        emit reportsReceived(QByteArray(reinterpret_cast<const char*>(report.data()),
                                       kReportBytes));
    }

    void acknowledge()
    {
        const int count = static_cast<int>(pending.size());
        pending.clear();
        emit reportsSent(count);
    }

    std::vector<Report> pending;
};

class RefusedSocket final : public QTcpSocket {
public:
    void connectToHost(const QString&, quint16, OpenMode, NetworkLayerProtocol) override
    {
        QTimer::singleShot(0, this, [this] {
            emit errorOccurred(QAbstractSocket::ConnectionRefusedError);
        });
    }
};

void testClosedSurvivesAnotherFault()
{
    Ctr2UsbRelay relay;
    Ctr2UsbRelay::Tuning tuning;
    tuning.incompleteMessageTimeoutMs = 30;
    relay.setTuning(tuning);
    int connections = 0;
    relay.setRadioSocketFactory([&connections] {
        ++connections;
        return new RefusedSocket;
    });
    auto* port = new StalledPort;
    check(relay.start(port, QHostAddress(QStringLiteral("192.0.2.10")), 4992),
          "relay accepts the injected port");

    FrameEncoder device;
    std::vector<Report> hello;
    device.encodeControl(MessageType::Hello, &hello);
    port->deliver(hello.front());
    check(waitUntil([&] { return port->pending.size() == 1; }),
          "refused connection queues CLOSED");
    const std::vector<Report> closed = port->pending;
    check(closed.size() == 1
              && closed.front()[3] == static_cast<std::uint8_t>(MessageType::Closed),
          "the pending report is CLOSED");

    // The device sent this before seeing CLOSED. Hold the payload reports so
    // its incomplete-message timeout fires while CLOSED remains unsent.
    std::vector<Report> inFlight;
    device.encodeData(QByteArray(20, 'x'), &inFlight);
    port->deliver(inFlight.front());
    check(waitUntil([&] {
              return relay.lastError().contains(QStringLiteral("message incomplete"));
          }),
          "the second fault reaches the incomplete-message path");
    check(port->pending == closed, "the original unsent CLOSED survives the second fault");
    check(connections == 1, "no connection is opened without another HELLO");

    port->acknowledge();
    check(port->pending.empty(), "CLOSED can still be acknowledged after the fault");
    hello.clear();
    device.encodeControl(MessageType::Hello, &hello);
    port->deliver(hello.front());
    check(waitUntil([&] { return port->pending.size() == 1; }),
          "the next HELLO gets its own CLOSED on refusal");
    check(connections == 2, "the next HELLO opens a fresh injected connection");
    relay.stop();
}

// A READY that answers a HELLO sent before a radio refusal arrives while the
// resulting CLOSED is still undelivered and the back-off is running. It must
// neither discard that CLOSED nor open another radio connection.
void testLateReadyKeepsClosedAndBackoff()
{
    Ctr2UsbRelay relay;
    Ctr2UsbRelay::Tuning tuning;
    tuning.helloIntervalMs = 20;
    tuning.helloRetryAfterFailureMs = 60000;
    relay.setTuning(tuning);
    int connections = 0;
    relay.setRadioSocketFactory([&connections] {
        ++connections;
        return new RefusedSocket;
    });
    auto* port = new StalledPort;
    check(relay.start(port, QHostAddress(QStringLiteral("192.0.2.10")), 4992), "relay starts");
    check(port->pending.size() == 1
              && port->pending.front()[3] == static_cast<std::uint8_t>(MessageType::Hello),
          "Start queues one HELLO");
    port->acknowledge();

    FrameEncoder device;
    std::vector<Report> ready;
    device.encodeControl(MessageType::Ready, &ready);
    port->deliver(ready.front());
    check(waitUntil([&] { return port->pending.size() == 1
                                 && port->pending.front()[3]
                                        == static_cast<std::uint8_t>(MessageType::Closed); }),
          "the answer opens one connection, whose refusal queues CLOSED");
    check(connections == 1, "one connection attempt");

    port->deliver(ready.front());  // late answer to the earlier HELLO
    QCoreApplication::processEvents();
    check(connections == 1, "a late READY does not open another connection");
    check(port->pending.size() == 1
              && port->pending.front()[3] == static_cast<std::uint8_t>(MessageType::Closed),
          "a late READY does not discard the undelivered CLOSED");

    port->acknowledge();
    port->deliver(ready.front());
    QCoreApplication::processEvents();
    check(connections == 1 && port->pending.empty(),
          "during the back-off, with no new HELLO sent, READY still changes nothing");

    std::vector<Report> hello;
    device.encodeControl(MessageType::Hello, &hello);
    port->deliver(hello.front());
    check(waitUntil([&] { return connections == 2; }),
          "a device HELLO still forces a restart during the back-off");
    relay.stop();
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testClosedSurvivesAnotherFault();
    testLateReadyKeepsClosedAndBackoff();
    if (g_failures) {
        return 1;
    }
    std::printf("ctr2_usb_closed_retention_test: all checks passed\n");
    return 0;
}
