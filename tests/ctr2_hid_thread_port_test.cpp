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
    std::atomic<int> failWritesAfter{-1}; // not -1: writes after this many fail
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
            if (failWritesAfter >= 0 && static_cast<int>(writes.size()) >= failWritesAfter) {
                return -1;
            }
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

    // Writes that went out before a failing one in the same tick are counted,
    // and reported before the failure: the firmware updater relies on it.
    auto dev3 = std::make_shared<FakeDevice>();
    dev3->failWritesAfter = 2;
    auto* port3 = new Ctr2HidThreadPort(dev3->io(), QStringLiteral("fake"));
    int sent3 = 0;
    int sentAtFailure = -1;
    QObject::connect(port3, &Ctr2HidPort::reportsSent, port3, [&](int n) { sent3 += n; });
    QObject::connect(port3, &Ctr2HidPort::failed, port3,
                     [&](const QString&) { sentAtFailure = sent3; });
    port3->send(reports(3, 0x50));
    check(waitUntil([&] { return sentAtFailure >= 0; }) && dev3->writeCount() == 2
              && sentAtFailure == 2,
          "writes before a failing one in the same tick are reported sent, before the failure");
    port3->shutdown({});
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
        quint32 offered;   // what the device offered, taken or not
    };
    const Case cases[] = {
        {"a stock CTR2 (no Feature report) stays version 0", {}, true, 0, 0},
        {"a build without Feature report access stays version 0", {}, false, 0, 0},
        {"a device answering with something else offers nothing",
         featureReport(QByteArray("NOTCAPS!")), true, 0, 0},
        {"an offer with a nonzero reserved byte offers nothing",
         featureReport(QByteArray::fromHex("435801ff00000003")), true, 0, 0},
        {"an offer is narrowed to what the host uses",
         featureReport(capabilities::encode(spectrumBit | (1u << 9))), true, spectrumBit,
         spectrumBit | (1u << 9)},
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
        check(port->offeredExtensions() == c.offered,
              "the port remembers everything the device offered, taken or not");
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

// Wide reports: once accepted, reports go out and come in bundled in report
// 0x04, up to seven at a time; report 0x01 padded by Windows still reads.
void testWideReports()
{
    auto dev = std::make_shared<FakeDevice>();
    dev->featureReply = featureReport(capabilities::encode(kWideReportsBit));
    auto* port = new Ctr2HidThreadPort(dev->io(), QStringLiteral("fake"));
    QByteArray got;
    int sent = 0;
    QString failure;
    QObject::connect(port, &Ctr2HidPort::reportsReceived, port, [&](const QByteArray& r) { got += r; });
    QObject::connect(port, &Ctr2HidPort::reportsSent, port, [&](int n) { sent += n; });
    QObject::connect(port, &Ctr2HidPort::failed, port, [&](const QString& m) { failure = m; });
    check(waitUntil([&] { return port->extensions() == kWideReportsBit; }),
          "a device offering wide reports gets them");

    port->send(reports(10, 0x50));
    check(waitUntil([&] { return sent == 10; }), "every report is acknowledged, bundled or not");
    {
        std::lock_guard<std::mutex> g(dev->lock);
        check(dev->writes.size() == 2, "ten reports go out in two writes");
        if (dev->writes.size() == 2) {
            const QByteArray& a = dev->writes[0];
            const QByteArray& b = dev->writes[1];
            check(a.size() == 1 + kWideReportBytes && b.size() == 1 + kWideReportBytes,
                  "each write is report 0x04 with its 63 bytes");
            check(static_cast<std::uint8_t>(a[0]) == kWideReportId && a[1] == 7
                      && static_cast<std::uint8_t>(b[0]) == kWideReportId && b[1] == 3,
                  "seven reports, then the remaining three");
            const auto at = [&](const QByteArray& w, int i) {
                return QByteArray(w.constData() + 2 + i * kReportBytes, kReportBytes);
            };
            const std::vector<Report> want = reports(10, 0x50);
            bool same = true;
            for (int i = 0; i < 10; ++i) {
                const QByteArray& w = i < 7 ? a : b;
                same = same && at(w, i % 7) == QByteArray(reinterpret_cast<const char*>(want[i].data()),
                                                         kReportBytes);
            }
            check(same, "the reports are carried whole and in order");
            check(b.mid(2 + 3 * kReportBytes) == QByteArray(kWideReportBytes - 1 - 3 * kReportBytes, '\0'),
                  "the rest of a bundle is zero padding");
        }
    }

    QByteArray bundle(1 + kWideReportBytes, '\0');
    bundle[0] = char(kWideReportId);
    bundle[1] = 2;
    bundle.replace(2, 16, "ABCDEFGHIJKLMNOP");
    QByteArray padded(1 + kWideReportBytes, '\0');
    padded[0] = char(kReportId);
    padded.replace(1, 8, "QRSTUVWX");
    {
        std::lock_guard<std::mutex> g(dev->lock);
        dev->reads.push_back(bundle);
        dev->reads.push_back(padded);
    }
    check(waitUntil([&] { return got.size() == 24; }) && got == "ABCDEFGHIJKLMNOPQRSTUVWX",
          "bundled reports, and a report 0x01 padded to 64 bytes, arrive in order");
    // A short read carrying report 0x04 is not an 8-byte report with its ID
    // stripped. A DATA message left open mid-way: the read's first byte, 0x04,
    // is a valid counter for its next report, so only the port can refuse it.
    {
        std::vector<Report> open, skipped;
        FrameEncoder enc;
        for (int i = 0; i < 3; ++i) {
            enc.encodeControl(MessageType::Closed, &skipped);   // counters 0..2
        }
        enc.encodeControl(MessageType::Ready, &open);           // counter 3
        check(enc.counter() == 4, "the DATA message gets counter 4, the stray read's first byte");
        enc.encodeData(QByteArray(7, 'x'), &open);   // header and one data report
        open.pop_back();                              // the data report never comes
        QByteArray two(1 + kWideReportBytes, '\0');
        two[0] = char(kWideReportId);
        two[1] = 2;
        for (int i = 0; i < 2; ++i) {
            two.replace(2 + i * kReportBytes, kReportBytes,
                        QByteArray(reinterpret_cast<const char*>(open[i].data()), kReportBytes));
        }
        got.clear();
        std::lock_guard<std::mutex> g(dev->lock);
        dev->reads.push_back(two);
    }
    check(waitUntil([&] { return got.size() == 16; }), "READY and a DATA header arrive");
    {
        std::lock_guard<std::mutex> g(dev->lock);
        dev->reads.push_back(QByteArray("\x04\x00\x00\x00\x00\x00\x00\x00", 8));
    }
    check(waitUntil([&] { return !failure.isEmpty(); }) && got.size() == 16,
          "a truncated report 0x04 fails the port instead of passing as data");
    {
        FrameReassembler rx;
        std::vector<Message> msgs;
        for (int off = 0; off < got.size(); off += kReportBytes) {
            rx.feed(reinterpret_cast<const std::uint8_t*>(got.constData() + off), kReportBytes, &msgs);
        }
        check(msgs.size() == 1 && msgs[0].type == MessageType::Ready && rx.isMidMessage(),
              "nothing of the DATA message is released");
    }
    port->shutdown({});
    waitUntil([&] { return dev->closed.load(); });

    // A bundle claiming no reports fails the port too.
    dev = std::make_shared<FakeDevice>();
    dev->featureReply = featureReport(capabilities::encode(kWideReportsBit));
    port = new Ctr2HidThreadPort(dev->io(), QStringLiteral("fake"));
    failure.clear();
    QObject::connect(port, &Ctr2HidPort::failed, port, [&](const QString& m) { failure = m; });
    check(waitUntil([&] { return port->extensions() == kWideReportsBit; }), "wide again");
    QByteArray empty = bundle;
    empty[1] = 0;
    {
        std::lock_guard<std::mutex> g(dev->lock);
        dev->reads.push_back(empty);
    }
    check(waitUntil([&] { return !failure.isEmpty(); }), "a bundle claiming no reports fails the port");
    port->shutdown({});
    waitUntil([&] { return dev->closed.load(); });

    // Final reports at shutdown are bundled too.
    auto dev2 = std::make_shared<FakeDevice>();
    dev2->featureReply = featureReport(capabilities::encode(kWideReportsBit));
    QPointer<Ctr2HidThreadPort> port2 = new Ctr2HidThreadPort(dev2->io(), QStringLiteral("fake"));
    waitUntil([&] { return port2->extensions() == kWideReportsBit; });
    port2->shutdown(reports(3, 0x60));
    check(waitUntil([&] { return port2.isNull(); }), "the port closes");
    {
        std::lock_guard<std::mutex> g(dev2->lock);
        check(dev2->writes.size() == 1 && static_cast<std::uint8_t>(dev2->writes[0][0]) == kWideReportId
                  && dev2->writes[0][1] == 3,
              "the final reports go in one bundle");
    }

    // Without wide reports, report 0x04 is not part of the link.
    auto dev3 = std::make_shared<FakeDevice>();
    auto* port3 = new Ctr2HidThreadPort(dev3->io(), QStringLiteral("fake"));
    QString failure3;
    QByteArray got3;
    QObject::connect(port3, &Ctr2HidPort::failed, port3, [&](const QString& m) { failure3 = m; });
    QObject::connect(port3, &Ctr2HidPort::reportsReceived, port3, [&](const QByteArray& r) { got3 += r; });
    {
        std::lock_guard<std::mutex> g(dev3->lock);
        dev3->reads.push_back(padded);     // Windows: a device with report 0x04 declared
    }
    check(waitUntil([&] { return got3 == "QRSTUVWX"; }), "a padded report 0x01 reads without wide reports");
    {
        std::lock_guard<std::mutex> g(dev3->lock);
        dev3->reads.push_back(bundle);
    }
    check(waitUntil([&] { return !failure3.isEmpty(); }), "without wide reports, report 0x04 fails the port");
    port3->shutdown({});
    waitUntil([] { return false; }, 100);
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
    testWideReports();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ctr2_hid_thread_port_test: all checks passed\n");
    return 0;
}
