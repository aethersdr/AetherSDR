// Ctr2HidThreadPort with injected device operations (no hidapi, no device):
// shutdown never blocks the owner's event loop even when writes are slow,
// discardQueued() is a real fence, and reads/failures surface correctly.

#include "core/Ctr2HidFraming.h"
#include "core/Ctr2HidThreadPort.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
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

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 5000)
{
    QElapsedTimer t;
    t.start();
    while (!predicate() && t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::usleep(500);
    }
    return predicate();
}

// The fake device, shared between the test and the port's I/O thread.
struct FakeDevice {
    std::mutex lock;
    std::vector<QByteArray> writes;     // every write, report ID included
    std::deque<QByteArray> reads;       // what read() returns next
    std::atomic<int> writeDelayMs{0};
    std::atomic<int> writesBegun{0};
    std::atomic<bool> failWrites{false};
    std::atomic<bool> closed{false};
    std::function<void()> onWritten;   // after a write completes, on the I/O thread
    QByteArray featureReply;           // Get Feature 0x02 answer, ID first; empty = stall
    std::vector<QByteArray> featureSets;
    std::atomic<bool> provideFeatureIo{true};

    Ctr2HidDeviceIo io()
    {
        Ctr2HidDeviceIo ops;
        ops.read = [this](unsigned char* buffer, int size) {
            std::lock_guard<std::mutex> g(lock);
            if (reads.empty()) {
                return 0;
            }
            const QByteArray r = reads.front();
            reads.pop_front();
            const int n = std::min<int>(size, static_cast<int>(r.size()));
            std::copy(r.constBegin(), r.constBegin() + n, buffer);
            return n;
        };
        ops.write = [this](const unsigned char* buffer, int size) {
            ++writesBegun;
            if (writeDelayMs > 0) {
                QThread::msleep(static_cast<unsigned long>(writeDelayMs.load()));
            }
            if (failWrites) {
                return -1;
            }
            std::lock_guard<std::mutex> g(lock);
            writes.emplace_back(reinterpret_cast<const char*>(buffer), size);
            if (onWritten) {
                onWritten();
            }
            return size;
        };
        if (provideFeatureIo) {
            ops.getFeature = [this](unsigned char* buffer, int size) {
                std::lock_guard<std::mutex> g(lock);
                if (featureReply.isEmpty() || buffer[0] != 0x02) {
                    return -1;  // what hidapi reports for a stalled request
                }
                const int n = std::min<int>(size, static_cast<int>(featureReply.size()));
                std::copy(featureReply.constBegin(), featureReply.constBegin() + n, buffer);
                return n;
            };
            ops.setFeature = [this](const unsigned char* buffer, int size) {
                std::lock_guard<std::mutex> g(lock);
                featureSets.emplace_back(reinterpret_cast<const char*>(buffer), size);
                return size;
            };
        }
        ops.close = [this] { closed = true; };
        ops.lastError = [] { return QStringLiteral("injected failure"); };
        return ops;
    }
    int writeCount()
    {
        std::lock_guard<std::mutex> g(lock);
        return static_cast<int>(writes.size());
    }
};

std::vector<Report> reports(int n, std::uint8_t tag)
{
    std::vector<Report> out;
    for (int i = 0; i < n; ++i) {
        out.push_back(Report{tag, static_cast<std::uint8_t>(i), 0, 0, 0, 0, 0, 0});
    }
    return out;
}

void testShutdownNeverBlocks()
{
    auto dev = std::make_shared<FakeDevice>();
    dev->writeDelayMs = 250;
    QPointer<Ctr2HidThreadPort> port = new Ctr2HidThreadPort(dev->io(), QStringLiteral("fake"));
    port->send(reports(20, 0x10));
    check(waitUntil([&] { return dev->writesBegun >= 1; }), "a slow write is in progress");

    int heartbeats = 0;
    QTimer beat;
    beat.setInterval(10);
    QObject::connect(&beat, &QTimer::timeout, [&] { ++heartbeats; });
    beat.start();

    std::vector<Report> closed;
    FrameEncoder enc;
    enc.encodeControl(MessageType::Closed, &closed);
    QElapsedTimer t;
    t.start();
    port->shutdown(closed);
    check(t.elapsed() < 50, "shutdown returns immediately, whatever the device is doing");
    check(waitUntil([&] { return port.isNull(); }, 3000), "the port deletes itself once its thread is done");
    check(heartbeats >= 10, "the owner's event loop keeps running throughout");
    check(dev->closed, "the device is closed");
    check(dev->writeCount() <= 2, "only the write already in progress and the final CLOSED are written");
    {
        std::lock_guard<std::mutex> g(dev->lock);
        check(!dev->writes.empty() && dev->writes.back().size() == 9
                  && static_cast<std::uint8_t>(dev->writes.back()[0]) == kReportId
                  && std::equal(closed[0].begin(), closed[0].end(),
                                reinterpret_cast<const std::uint8_t*>(dev->writes.back().constData() + 1)),
              "the final CLOSED is the last report written, with its report ID");
    }
}

void testDiscardIsAFence()
{
    auto dev = std::make_shared<FakeDevice>();
    dev->writeDelayMs = 50;
    auto* port = new Ctr2HidThreadPort(dev->io(), QStringLiteral("fake"));
    port->send(reports(10, 0x20));
    check(waitUntil([&] { return dev->writesBegun >= 1; }), "old output is being written");
    port->discardQueued();
    int sentAfterFence = 0;
    QObject::connect(port, &Ctr2HidPort::reportsSent, port, [&](int n) { sentAfterFence += n; });
    port->send(reports(1, 0x30));
    check(waitUntil([&] {
              std::lock_guard<std::mutex> g(dev->lock);
              return !dev->writes.empty() && static_cast<std::uint8_t>(dev->writes.back()[1]) == 0x30;
          }),
          "the post-fence report is written");
    QCoreApplication::processEvents();
    int old = 0;
    {
        std::lock_guard<std::mutex> g(dev->lock);
        for (const QByteArray& w : dev->writes) {
            old += static_cast<std::uint8_t>(w[1]) == 0x20;
        }
    }
    check(old <= 1, "at most the write already in progress survives the fence");
    check(waitUntil([&] { return sentAfterFence == 1; }) && sentAfterFence == 1,
          "acknowledgements count only post-fence reports");
    port->shutdown({});
    waitUntil([] { return false; }, 100);
}

void testReadsAndFailures()
{
    auto dev = std::make_shared<FakeDevice>();
    auto* port = new Ctr2HidThreadPort(dev->io(), QStringLiteral("fake"));
    QByteArray got;
    QString failure;
    QObject::connect(port, &Ctr2HidPort::reportsReceived, port, [&](const QByteArray& r) { got += r; });
    QObject::connect(port, &Ctr2HidPort::failed, port, [&](const QString& m) { failure = m; });
    {
        std::lock_guard<std::mutex> g(dev->lock);
        dev->reads.push_back(QByteArray("\x01" "ABCDEFGH", 9));  // numbered report
        dev->reads.push_back(QByteArray("IJKLMNOP", 8));         // ID already stripped
    }
    check(waitUntil([&] { return got.size() == 16; }) && got == "ABCDEFGHIJKLMNOP",
          "reports arrive in order without their report ID");
    {
        std::lock_guard<std::mutex> g(dev->lock);
        dev->reads.push_back(QByteArray("short", 5));
    }
    check(waitUntil([&] { return !failure.isEmpty(); }) && !port->isOpen(),
          "a malformed report fails the port");
    port->shutdown({});

    auto dev2 = std::make_shared<FakeDevice>();
    dev2->failWrites = true;
    auto* port2 = new Ctr2HidThreadPort(dev2->io(), QStringLiteral("fake"));
    QString failure2;
    QObject::connect(port2, &Ctr2HidPort::failed, port2, [&](const QString& m) { failure2 = m; });
    port2->send(reports(1, 0x40));
    check(waitUntil([&] { return !failure2.isEmpty(); }) && failure2.contains("injected failure"),
          "a failed write fails the port with the device's reason");
    port2->shutdown({});
    waitUntil([] { return false; }, 100);
}

// Stop then Start on one device: the old port's slow final CLOSED and the new
// port's first report. Returns the order they reached the device in.
std::string reopenOrder(const QString& oldKey, const QString& newKey)
{
    auto order = std::make_shared<std::string>();
    auto orderLock = std::make_shared<std::mutex>();
    auto oldDev = std::make_shared<FakeDevice>();
    auto newDev = std::make_shared<FakeDevice>();
    oldDev->writeDelayMs = 150;
    oldDev->onWritten = [order, orderLock] { std::lock_guard<std::mutex> g(*orderLock); *order += 'C'; };
    newDev->onWritten = [order, orderLock] { std::lock_guard<std::mutex> g(*orderLock); *order += 'H'; };

    QPointer<Ctr2HidThreadPort> oldPort = new Ctr2HidThreadPort(oldDev->io(), QStringLiteral("old"), oldKey);
    waitUntil([] { return false; }, 20);  // the old port is up and idle
    std::vector<Report> closed;
    std::vector<Report> hello;
    FrameEncoder enc;
    enc.encodeControl(MessageType::Closed, &closed);
    enc.encodeControl(MessageType::Hello, &hello);
    oldPort->shutdown(closed);
    auto* newPort = new Ctr2HidThreadPort(newDev->io(), QStringLiteral("new"), newKey);
    newPort->send(hello);
    waitUntil([&] { return oldPort.isNull() && newDev->writeCount() == 1; }, 3000);
    newPort->shutdown({});
    waitUntil([] { return false; }, 50);
    std::lock_guard<std::mutex> g(*orderLock);
    return *order;
}

void testReopenWaitsForTheOldPort()
{
    const QString path = QStringLiteral("/dev/hidraw-ctr2");
    check(reopenOrder(path, path) == "CH",
          "a new port for the same device writes only after the old port's final CLOSED");
    check(reopenOrder(QStringLiteral("/dev/hidraw-a"), QStringLiteral("/dev/hidraw-b")) == "HC",
          "ports for different devices do not wait for each other");

    // A port that failed has already let go of the device, even before it is
    // shut down, so reopening does not sit out the full wait.
    const QString key = QStringLiteral("/dev/hidraw-failed");
    auto failedDev = std::make_shared<FakeDevice>();
    failedDev->failWrites = true;
    auto* failed = new Ctr2HidThreadPort(failedDev->io(), QStringLiteral("failed"), key);
    bool sawFailure = false;
    QObject::connect(failed, &Ctr2HidPort::failed, [&] { sawFailure = true; });
    failed->send(reports(1, 0x20));
    check(waitUntil([&] { return sawFailure; }), "the first port fails");
    auto freshDev = std::make_shared<FakeDevice>();
    auto* fresh = new Ctr2HidThreadPort(freshDev->io(), QStringLiteral("fresh"), key);
    QElapsedTimer t;
    t.start();
    fresh->send(reports(1, 0x21));
    check(waitUntil([&] { return freshDev->writeCount() == 1; }, 3000) && t.elapsed() < 500,
          "a reopen after a failure writes at once");
    fresh->shutdown({});
    failed->shutdown({});
    waitUntil([] { return false; }, 50);
}

void testIdleDestructorDoesNotHang()
{
    auto dev = std::make_shared<FakeDevice>();
    QElapsedTimer t;
    t.start();
    delete new Ctr2HidThreadPort(dev->io(), QStringLiteral("fake"));
    check(t.elapsed() < 1000 && dev->closed, "deleting an idle port closes it promptly");
}

} // namespace

QByteArray featureReport(const QByteArray& body)
{
    return QByteArray(1, char(capabilities::kReportId)) + body;
}

// Extensions are negotiated on the I/O thread before the first report, and
// only with a device that answers Feature report 0x02 properly.
void testNegotiation()
{
    const quint32 spectrumBit = capabilityBit(MessageType::AudioSpectrum);
    struct Case {
        const char* name;
        QByteArray reply;
        bool featureIo;
        quint32 expect;
    };
    const Case cases[] = {
        {"a stock CTR2 (no Feature report) stays version 0", {}, true, 0},
        {"a build without Feature report access stays version 0", {}, false, 0},
        {"a device answering with something else offers nothing",
         featureReport(QByteArray("NOTCAPS!")), true, 0},
        {"an offer with a nonzero reserved byte offers nothing",
         featureReport(QByteArray::fromHex("435801ff00000003")), true, 0},
        {"an offer is narrowed to what the host uses",
         featureReport(capabilities::encode(spectrumBit | (1u << 9))), true, spectrumBit},
    };
    for (const Case& c : cases) {
        auto dev = std::make_shared<FakeDevice>();
        dev->featureReply = c.reply;
        dev->provideFeatureIo = c.featureIo;
        // A report already waiting: it must arrive after the negotiation result.
        QByteArray header(1, char(kReportId));
        header.append(QByteArray(8, char(0xFF)));
        dev->reads.push_back(header);

        auto* port = new Ctr2HidThreadPort(dev->io(), QStringLiteral("fake"));
        bool negotiatedFirst = false, gotReports = false, negotiated = false;
        quint32 result = 0xFFFFFFFF;
        QObject::connect(port, &Ctr2HidPort::extensionsNegotiated, [&](quint32 ext) {
            negotiated = true;
            result = ext;
            negotiatedFirst = !gotReports;
        });
        QObject::connect(port, &Ctr2HidPort::reportsReceived, [&](const QByteArray&) {
            gotReports = true;
        });
        check(waitUntil([&] { return negotiated && gotReports; }), c.name);
        check(result == c.expect && port->extensions() == c.expect, c.name);
        check(negotiatedFirst, "the negotiation result arrives before any report");
        std::vector<QByteArray> sets;
        {
            std::lock_guard<std::mutex> g(dev->lock);
            sets = dev->featureSets;
        }
        if (c.expect) {
            check(sets.size() == 1
                      && sets[0] == featureReport(capabilities::encode(c.expect)),
                  "the host tells the device exactly the extensions it will use");
        } else {
            check(sets.empty(), "a device offering nothing is never written a Feature report");
        }
        port->shutdown({});
        waitUntil([&] { return dev->closed.load(); });
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testShutdownNeverBlocks();
    testDiscardIsAFence();
    testReadsAndFailures();
    testIdleDestructorDoesNotHang();
    testReopenWaitsForTheOldPort();
    testNegotiation();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ctr2_hid_thread_port_test: all checks passed\n");
    return 0;
}
