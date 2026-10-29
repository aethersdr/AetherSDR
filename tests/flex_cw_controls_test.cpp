// Production encoder/scheduler with inert transport capture. No sockets or peer.
#include "TestSettingsProfile.h"
#include "TxTestAuthority.h"
#include "core/backends/flex/FlexCwControls.h"
#include "core/backends/flex/PanadapterStream.h"
#include "core/backends/flex/WanConnection.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QtEndian>
#include <cstdio>
#include <memory>

using namespace AetherSDR;
namespace AetherSDR {
class WanConnectionTestAccess {
public:
    static bool reentrantReply()
    {
        WanConnection wan;
        bool called = false;
        wan.m_pendingCallbacks.insert(7, [&wan, &called](int, const QString&) {
            called = true;
            wan.forgetSessionReply(7, wan.commandSessionGeneration());
            wan.m_pendingCallbacks.clear();
        });
        wan.processLine(QStringLiteral("R7|0|accepted"));
        return called && wan.m_pendingCallbacks.isEmpty();
    }
};
class FlexCwControlsTestAccess {
public:
    static void record(PanadapterStream& stream, QList<QByteArray>& packets)
    { stream.m_packetSinkForTest = [&packets](const QByteArray& packet) { packets << packet; }; }
    static quint32 id(const FlexCwControls& cw) { return cw.m_streamId; }
    static void expireCreate(FlexCwControls& cw) { QMetaObject::invokeMethod(&cw.m_createDeadline, "timeout", Qt::DirectConnection); }
    static void expirePtt(FlexCwControls& cw) { QMetaObject::invokeMethod(&cw.m_pttDeadline, "timeout", Qt::DirectConnection); }
    static bool timestampVectors()
    {
        FlexCwControls cw(nullptr);
        if (cw.timestamp(1000, 90).value != 0) { return false; }
        auto stamp = cw.timestamp(1300, 200);
        if (stamp.value != 200 || stamp.backdate != 100 || stamp.fallback) { return false; }
        stamp = cw.timestamp(1301, 100);
        if (stamp.value != 201 || stamp.fallback) { return false; }
        stamp = cw.timestamp(1301, 100);
        if (stamp.value != 301 || !stamp.fallback) { return false; }
        cw.m_lastSendMs = 65530;
        if (cw.timestamp(66540, -1).value != 4) { return false; }
        if (cw.timestamp(70541, 50).value != 0) { return false; }
        return true;
    }
    static void index(FlexCwControls& cw, quint32 index) { cw.m_index = index; }
};
}
namespace {
int failures = 0;
void check(bool okay, const char* what)
{ if (!okay) { ++failures; std::fprintf(stderr, "FAIL: %s\n", what); } }
bool waitFor(const std::function<bool()>& done)
{
    QElapsedTimer elapsed; elapsed.start();
    while (!done() && elapsed.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return done();
}
struct Fixture {
    QList<QByteArray> packets;
    QStringList tcp;
    QList<RadioControlCompletion> replies;
    QList<SessionCommandGuard> guards;
    PanadapterStream stream;
    FlexCwControls cw{&stream};
    TxTestAuthority authority;
    Fixture()
    {
        FlexCwControlsTestAccess::record(stream, packets);
        cw.setControlSink([this](const QString&, const SessionCommandGuard& guard, const RadioControlCompletion& reply) {
            guards << guard; replies << reply; return true;
        });
        cw.setTxSink([this](const QString& text, const TxCoordinator::Command&) { tcp << text; });
    }
    void prepare(quint32 handle = 0xAu)
    {
        cw.prepare(handle, [] { return true; });
        replies.last()({RadioControlReply::Kind::Accepted, 0, "0x12345678"});
    }
    ReceiveDispatch edge(bool down, const TxCoordinator::Completion& done = {},
                          CwControlRequest::Kind kind = CwControlRequest::Kind::Carrier)
    { return cw.request({kind, down}, {authority.operation, down, done}); }
};
void lifecycle()
{
    Fixture f;
    f.cw.prepare(1, [] { return true; });
    f.cw.prepare(1, [] { return true; });
    check(f.replies.size() == 1, "duplicate preparation coalesces");
    const auto old = f.replies.first();
    f.cw.retire();
    old({RadioControlReply::Kind::Accepted, 0, "12345678"});
    check(FlexCwControlsTestAccess::id(f.cw) == 0, "old-session create cannot adopt a stream");
    f.prepare();
    f.replies.last()({RadioControlReply::Kind::Accepted, 0, "87654321"});
    check(FlexCwControlsTestAccess::id(f.cw) == 0x12345678, "duplicate reply cannot replace a stream");
    f.cw.retire();
    for (const QString& invalid : {QString("0"), QString("100000000"), QString("+1"), QString("xyz"), QString("1 extra")}) {
        f.cw.prepare(1, [] { return true; });
        f.replies.last()({RadioControlReply::Kind::Accepted, 0, invalid});
        check(FlexCwControlsTestAccess::id(f.cw) == 0, "malformed stream ID rejected");
        f.cw.retire();
    }
    f.cw.prepare(1, [] { return true; });
    FlexCwControlsTestAccess::expireCreate(f.cw);
    f.replies.last()({RadioControlReply::Kind::Accepted, 0, "12345678"});
    check(FlexCwControlsTestAccess::id(f.cw) == 0 && !f.guards.last().current(), "timeout fences late creation and queued write");
}
void transport()
{
    Fixture f;
    f.prepare();
    int completions = 0;
    f.edge(true, TxCoordinator::Completion([&] { ++completions; }));
    check(completions == 0, "TCP completion cannot retire UDP copies");
    check(waitFor([&] { return f.packets.size() == 4 && completions == 1; }), "all UDP copies finish once");
    check(f.tcp.size() == 1 && f.tcp.first() == "cw key 1 time=0x0000 index=1 client_handle=0x0000000A", "captured TCP golden command");
    for (int i = 0; i < 4; ++i) {
        const QByteArray& bytes = f.packets[i];
        const quint32 header = qFromBigEndian<quint32>(bytes.constData());
        check((header & 0xFFF00000u) == 0x38D00000u && ((header >> 16) & 15) == quint32(i)
            && (header & 0xFFFF) * 4 == quint32(bytes.size()), "VITA header/count/word length");
        check(qFromBigEndian<quint32>(bytes.constData() + 4) == 0x12345678u
            && qFromBigEndian<quint32>(bytes.constData() + 8) == 0x1C2Du
            && qFromBigEndian<quint32>(bytes.constData() + 12) == 0x534C03E3u, "stream and class identity");
        check(bytes.mid(16, 12) == QByteArray(12, '\0'), "zero header timestamps");
        check(bytes.mid(28).startsWith(f.tcp.first().toLatin1()), "TCP and UDP carry identical command");
        check(bytes.mid(28 + f.tcp.first().size()).trimmed().replace('\0', "").isEmpty(), "zero padded payload");
    }
    Fixture second;
    second.prepare(); second.edge(true);
    check(waitFor([&] { return second.packets.size() == 4; }), "second backend delivers");
    check(((qFromBigEndian<quint32>(second.packets.first().constData()) >> 16) & 15) == 0, "per-stream packet counter");
    f.cw.observeStreamRemoved(0x12345678);
    f.edge(false);
    check(f.tcp.last() == "cw key immediate 0", "stream loss retains immediate cleanup compatibility");
    f.edge(false, {}, CwControlRequest::Kind::PttEnvelope);
    check(f.tcp.last() == "cw ptt 0", "CW PTT never becomes generic xmit");
    f.cw.retire(); f.prepare();
    FlexCwControlsTestAccess::index(f.cw, 0x7FFFFFFF);
    f.edge(true); f.edge(false);
    check(f.tcp[f.tcp.size()-2].contains("index=2147483647 ") && f.tcp.last().contains("index=-2147483648 "), "signed FlexLib index wraps without C++ overflow");
    check(waitFor([&] { return f.packets.size() == 12; }), "wrap edges drain");
    f.packets.clear(); completions = 0;
    f.edge(true, TxCoordinator::Completion([&] { ++completions; }));
    f.cw.retire();
    check(waitFor([&] { return completions == 1; }) && f.packets.isEmpty(), "retirement drops queued copies and completes once");
}
void localPtt()
{
    Fixture f;
    int completions = 0;
    RadioControlReply::Kind kind{};
    const auto done = [&](const RadioControlReply& reply) { ++completions; kind = reply.kind; };
    check(f.cw.requestLocalPtt(done, [] { return true; }) == ReceiveDispatch::Dispatched, "PTT request dispatched");
    check(f.cw.requestLocalPtt(done, [] { return true; }) == ReceiveDispatch::Unsupported && f.replies.size() == 1, "PTT pending request bounded");
    f.replies.first()({RadioControlReply::Kind::Accepted, 0, {}});
    f.replies.first()({RadioControlReply::Kind::Accepted, 0, {}});
    check(completions == 1 && kind == RadioControlReply::Kind::Accepted, "duplicate PTT callback completes once");
    f.cw.requestLocalPtt(done, [] { return true; });
    f.cw.retire();
    f.replies.last()({RadioControlReply::Kind::Accepted, 0, {}});
    check(completions == 2 && kind == RadioControlReply::Kind::Cancelled, "session retirement cancels ownership request");
    f.cw.requestLocalPtt(done, [] { return true; });
    FlexCwControlsTestAccess::expirePtt(f.cw);
    f.replies.last()({RadioControlReply::Kind::Accepted, 0, {}});
    check(completions == 3 && kind == RadioControlReply::Kind::Failed, "PTT timeout is terminal");
}

void retirementAndDispatch()
{
    Fixture f;
    f.prepare();
    int completions = 0;
    f.edge(true, TxCoordinator::Completion([&] { ++completions; }));
    f.cw.observeStreamRemoved(0x12345678);
    check(waitFor([&] { return completions == 1; }) && f.packets.isEmpty(),
          "stream removal fences copies already queued to the worker");
    f.cw.retire();
    bool current = true;
    f.cw.prepare(1, [&] { return current; });
    current = false;
    f.replies.last()({RadioControlReply::Kind::Accepted, 0, "12345678"});
    check(FlexCwControlsTestAccess::id(f.cw) == 0
              && f.edge(true) == ReceiveDispatch::Unsupported,
          "expired owner fence cannot adopt a stream or send a new edge");
    check(f.edge(false) == ReceiveDispatch::Dispatched
              && f.tcp.last() == QStringLiteral("cw key immediate 0"),
          "closing admission still permits original-operation release cleanup");
    f.authority.coordinator.reset();
    check(f.edge(false) == ReceiveDispatch::Unsupported,
          "cleanup cannot cross an operation-session reset");

    QList<QByteArray> packets;
    PanadapterStream stream;
    FlexCwControlsTestAccess::record(stream, packets);
    TxTestAuthority authority;
    auto cw = std::make_unique<FlexCwControls>(&stream);
    cw->setTxSink([](const QString&, const TxCoordinator::Command& command) { command.completion.finish(); });
    cw->setControlSink([](const QString&, const SessionCommandGuard&, const RadioControlCompletion& reply) {
        reply({RadioControlReply::Kind::Accepted, 0, "12345678"}); return true;
    });
    cw->prepare(1, [] { return true; });
    int destroyed = 0;
    cw->request({CwControlRequest::Kind::Carrier, true},
                {authority.operation, true, TxCoordinator::Completion([&] { ++destroyed; })});
    cw.reset();
    check(waitFor([&] { return destroyed == 1; }) && packets.isEmpty(),
          "helper destruction cancels queued copies and completes once");

    Fixture invalid;
    invalid.prepare();
    check(invalid.cw.request({CwControlRequest::Kind::Carrier, true}, {}) == ReceiveDispatch::Unsupported
          && invalid.cw.request({CwControlRequest::Kind::Carrier, true}, {invalid.authority.operation, false}) == ReceiveDispatch::Unsupported
          && invalid.cw.request({static_cast<CwControlRequest::Kind>(99), true}, {invalid.authority.operation, true}) == ReceiveDispatch::Unsupported
          && invalid.tcp.isEmpty() && invalid.packets.isEmpty(),
          "missing authority, mismatched edge and unknown verb produce no writes");
}
}
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("flex-cw-controls-test"));
    QCoreApplication app(argc, argv);
    lifecycle(); transport(); localPtt(); retirementAndDispatch();
    check(WanConnectionTestAccess::reentrantReply(), "WAN callback may retire its own request safely");
    check(FlexCwControlsTestAccess::timestampVectors(), "clock reset/clamp/order/wrap vectors");
    std::printf("flex_cw_controls_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
