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
            return size;
        };
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

void testIdleDestructorDoesNotHang()
{
    auto dev = std::make_shared<FakeDevice>();
    QElapsedTimer t;
    t.start();
    delete new Ctr2HidThreadPort(dev->io(), QStringLiteral("fake"));
    check(t.elapsed() < 1000 && dev->closed, "deleting an idle port closes it promptly");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testShutdownNeverBlocks();
    testDiscardIsAFence();
    testReadsAndFailures();
    testIdleDestructorDoesNotHang();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ctr2_hid_thread_port_test: all checks passed\n");
    return 0;
}
