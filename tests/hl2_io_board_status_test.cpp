// `hl2` / `ioboard.status`: the IO board status read, from the verb through the
// backend's own MetisClient and back. Socket-free: the client is told it is
// streaming, its request bank is "sent" by hand, and the radio's reply is fed
// to its response path, all on the client's own thread.
//
// The wire encoding is hl2_metis_protocol_test's; the client's request rules
// are hl2_rqst_ack_client_test's. This pins the wiring between them: one
// answer per call, the registers decoded, and a refusal or an empty bus
// reported as an error.

#include "core/backends/IRadioBackend.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2ControlRequest.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>
#include <QEvent>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantMap>

#include <cstdint>
#include <cstdio>

namespace AetherSDR::hl2 {
struct MetisClientTestAccess {
    static void setStreaming(MetisClient& c) { c.m_running = true; c.m_linkUp = true; }
    // Put whatever is queued on the "wire", then deliver `reply` as the radio would.
    static void answer(MetisClient& c, const Ep6Response& reply)
    {
        for (int i = 0; i < 16 && c.controlRequest().state() == Hl2ControlRequest::State::Queued;
             ++i) {
            (void)c.buildNextControlPacket();
            c.onControlPacketSent(static_cast<qint64>(kUsbPacketSize), 0);
        }
        c.ingestControlResponse(reply);
    }
};

struct Hl2IoBoardStatusTestAccess {
    static void setConnected(Hl2Backend& b) { b.m_connected = true; }
    static MetisClient* metis(Hl2Backend& b) { return b.m_metis; }
};
}  // namespace AetherSDR::hl2

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

struct Replies {
    int results = 0, errors = 0;
    quint64 lastId = 0;
    QVariantMap lastResult;
    QString lastError;
};

static void watch(Hl2Backend& backend, Replies& r)
{
    QObject::connect(&backend, &AetherSDR::IRadioBackend::extensionResult, &backend,
                     [&r](quint64 id, const QVariant& v) {
        ++r.results; r.lastId = id; r.lastResult = v.toMap();
    });
    QObject::connect(&backend, &AetherSDR::IRadioBackend::extensionError, &backend,
                     [&r](quint64 id, const QString& reason) {
        ++r.errors; r.lastId = id; r.lastError = reason;
    });
}

// Run `fn` on the client's thread and wait, then deliver whatever it queued
// back to the backend, which lives on this thread with no event loop running.
template <typename Fn>
static void onClientThread(Hl2Backend& backend, Fn fn)
{
    MetisClient* metis = Hl2IoBoardStatusTestAccess::metis(backend);
    QMetaObject::invokeMethod(metis, [metis, fn] { fn(*metis); },
                              Qt::BlockingQueuedConnection);
    QCoreApplication::sendPostedEvents(&backend, QEvent::MetaCall);
}

static Ep6Response ack(int raddr, std::uint32_t data)
{
    Ep6Response r;
    r.ack = true;
    r.raddr = raddr;
    r.data = data;
    return r;
}

static void testRefusedWhileDisconnected()
{
    Hl2Backend backend;
    Replies r;
    watch(backend, r);
    backend.invokeExtension(QStringLiteral("hl2"), QStringLiteral("ioboard.status"), 5, {});
    check(r.errors == 1 && r.results == 0 && r.lastId == 5,
          "with no radio the read is refused, under the caller's requestId");
    check(r.lastError.contains(QStringLiteral("connected"), Qt::CaseInsensitive),
          "as not connected, not as an unknown verb");
    backend.invokeExtension(QStringLiteral("hl2"), QStringLiteral("ioboard.status"), 0, {});
    check(r.errors == 1 && r.results == 0, "requestId 0 asks for no reply and gets none");
}

static void testAnsweredWithTheRegistersDecoded()
{
    Hl2Backend backend;
    Replies r;
    watch(backend, r);
    Hl2IoBoardStatusTestAccess::setConnected(backend);
    onClientThread(backend, [](MetisClient& c) { MetisClientTestAccess::setStreaming(c); });

    backend.invokeExtension(QStringLiteral("hl2"), QStringLiteral("ioboard.status"), 11, {});
    backend.invokeExtension(QStringLiteral("hl2"), QStringLiteral("ioboard.status"), 12, {});
    check(r.errors == 1 && r.lastId == 12, "a second read while one is pending is refused");

    // Receiving, In1/In3/In5 high, the ATU asking for RF, no fault, firmware 7.
    onClientThread(backend, [](MetisClient& c) {
        MetisClientTestAccess::answer(c, ack(kC0I2c2 >> 1, 0x07'00'EE'2Bu));
    });
    check(r.results == 1 && r.lastId == 11, "the pending read is answered, once");
    const QVariantMap& m = r.lastResult;
    check(m.value(QStringLiteral("inputPins")).toInt() == 0x2B, "inputPins is REG_INPUT_PINS");
    check(m.value(QStringLiteral("receiving")).toBool(), "receiving follows EXTTR");
    check(m.value(QStringLiteral("in1")).toBool() && !m.value(QStringLiteral("in2")).toBool()
              && m.value(QStringLiteral("in3")).toBool() && !m.value(QStringLiteral("in4")).toBool()
              && m.value(QStringLiteral("in5")).toBool(),
          "in1..in5 are the input pins");
    check(m.value(QStringLiteral("antennaTuner")).toInt() == 0xEE, "antennaTuner is REG_ANTENNA_TUNER");
    check(m.contains(QStringLiteral("fault")) && m.value(QStringLiteral("fault")).toInt() == 0,
          "fault is REG_FAULT, present when zero");
    check(m.value(QStringLiteral("firmwareMajor")).toInt() == 7, "firmwareMajor is REG_FIRMWARE_MAJOR");

    // The next read, refused by the radio: an error, and the slot is free again.
    backend.invokeExtension(QStringLiteral("hl2"), QStringLiteral("ioboard.status"), 13, {});
    onClientThread(backend, [](MetisClient& c) {
        MetisClientTestAccess::answer(c, ack(kRespAddrError, 0));
    });
    check(r.errors == 2 && r.lastId == 13 && !r.lastError.isEmpty(),
          "a refusal is reported as an error, with a reason");
    check(r.results == 1, "and not as a reading");
    backend.invokeExtension(QStringLiteral("hl2"), QStringLiteral("ioboard.status"), 14, {});
    onClientThread(backend, [](MetisClient& c) {
        MetisClientTestAccess::answer(c, ack(kC0I2c2 >> 1, 0x02'00'00'01u));
    });
    check(r.results == 2 && r.lastId == 14, "the read after a refusal is answered");
}

static void testAnEmptyBusIsNotAReading()
{
    // With no board fitted the read is expected to complete as 0xFF bytes
    // (docs/HERMES.md). Passed through, that would read as an ATU failure code.
    Hl2Backend backend;
    Replies r;
    watch(backend, r);
    Hl2IoBoardStatusTestAccess::setConnected(backend);
    onClientThread(backend, [](MetisClient& c) { MetisClientTestAccess::setStreaming(c); });

    backend.invokeExtension(QStringLiteral("hl2"), QStringLiteral("ioboard.status"), 21, {});
    onClientThread(backend, [](MetisClient& c) {
        MetisClientTestAccess::answer(c, ack(kC0I2c2 >> 1, 0xFF'FF'FF'FFu));
    });
    check(r.errors == 1 && r.results == 0 && r.lastId == 21,
          "an all-ones reply is an error, not a reading");
    check(r.lastError.contains(QStringLiteral("No IO board")), "and says no board answered");

    backend.invokeExtension(QStringLiteral("hl2"), QStringLiteral("ioboard.status"), 22, {});
    onClientThread(backend, [](MetisClient& c) {
        MetisClientTestAccess::answer(c, ack(kC0I2c2 >> 1, 0x02'00'00'01u));
    });
    check(r.results == 1 && r.lastId == 22, "and the next read is answered");

    // An all-zeros bus reads as firmware 0, which no board reports.
    backend.invokeExtension(QStringLiteral("hl2"), QStringLiteral("ioboard.status"), 23, {});
    onClientThread(backend, [](MetisClient& c) {
        MetisClientTestAccess::answer(c, ack(kC0I2c2 >> 1, 0x00'00'00'00u));
    });
    check(r.errors == 2 && r.results == 1 && r.lastId == 23
              && r.lastError.contains(QStringLiteral("No IO board")),
          "an all-zeros reply is not a reading either");
}

static void testAnIgnoredReadIsTheBusBeingBusy()
{
    // i2c_bus2.v ignores a read that arrives during a write, and control.v
    // answers with the request's own DATA: not a reading.
    Hl2Backend backend;
    Replies r;
    watch(backend, r);
    Hl2IoBoardStatusTestAccess::setConnected(backend);
    onClientThread(backend, [](MetisClient& c) { MetisClientTestAccess::setStreaming(c); });

    backend.invokeExtension(QStringLiteral("hl2"), QStringLiteral("ioboard.status"), 31, {});
    onClientThread(backend, [](MetisClient& c) {
        MetisClientTestAccess::answer(c, ack(kC0I2c2 >> 1, kIoBoardStatusReadData));
    });
    check(r.errors == 1 && r.results == 0 && r.lastId == 31,
          "an echo of the request is an error, not a reading");
    check(r.lastError.contains(QStringLiteral("busy")), "and says the bus was busy");

    backend.invokeExtension(QStringLiteral("hl2"), QStringLiteral("ioboard.status"), 32, {});
    onClientThread(backend, [](MetisClient& c) {
        MetisClientTestAccess::answer(c, ack(kC0I2c2 >> 1, 0x02'00'00'01u));
    });
    check(r.results == 1 && r.lastId == 32, "and the next read is answered");
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testRefusedWhileDisconnected();
    testAnsweredWithTheRegistersDecoded();
    testAnEmptyBusIsNotAReading();
    testAnIgnoredReadIsTheBusBeingBusy();

    if (g_failures == 0) {
        std::printf("hl2_io_board_status_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
