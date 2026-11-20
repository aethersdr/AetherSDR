// The host CW text engine in RadioModel (#7146): text sent through
// requestProducerCwx / CwxModel on a radio with no text buffer is keyed by the
// model's one HostCwTextEngine. The HL2 fixture takes its capabilities from a
// real, unconnected Hl2Backend with canTransmit set. The real CwxLocalKeyer
// worker times the text; every edge is recorded where RadioModel hands it to
// IRadioBackend::setCwKeying(). Socket-free: nothing is opened, nothing keyed.

#include "TestSettingsProfile.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/SliceDelta.h"
#include "core/backends/TransmitDelta.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "models/CwxModel.h"
#include "models/HostCwTextEngine.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <vector>

using namespace AetherSDR;
using Clock = std::chrono::steady_clock;

namespace {

int g_failed = 0;

void check(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

struct Edge {
    bool down;
    Clock::time_point at;
};

class KeyingStub final : public IRadioBackend {
public:
    RadioCapabilities caps;
    bool connected{false};
    std::vector<Edge>* edges{nullptr};
    QStringList* cwText{nullptr};

    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override { connected = false; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void setCwKeying(bool down, bool, int, const TxCoordinator::Operation&,
                     const TxCoordinator::Completion&) override
    {
        edges->push_back({down, Clock::now()});
    }
    ReceiveDispatch requestCwControl(const CwControlRequest& request,
                                     const TxCoordinator::Command& command) override
    {
        if (request.kind == CwControlRequest::Kind::PttEnvelope) {
            return IRadioBackend::requestCwControl(request, command);
        }
        setCwKeying(request.down, request.breakIn, request.breakInDelayMs,
                    command.operation, command.completion);
        return ReceiveDispatch::Dispatched;
    }
    QString sendCwText(const QString& text, const TxCoordinator::Operation&,
                       const TxCoordinator::Completion&) override
    {
        *cwText << text;
        return {};
    }
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

RadioCapabilities hl2Capabilities(bool canTransmit)
{
    hl2::Hl2Backend real;
    RadioCapabilities caps = real.capabilities();
    caps.canTransmit = canTransmit;
    return caps;
}

struct Fixture {
    std::vector<Edge> edges;   // outlives the model's backend
    QStringList cwText;
    RadioModel radio;
    KeyingStub* backend{nullptr};

    // Break-in on by default: the HL2 then raises MOX itself.
    explicit Fixture(const RadioCapabilities& caps, const QString& mode = QStringLiteral("CW"),
                     bool txSlice = true, bool breakIn = true)
    {
        radio.transmitModel().setCwBreakIn(breakIn);
        auto owned = std::make_unique<KeyingStub>();
        backend = owned.get();
        backend->caps = caps;
        backend->edges = &edges;
        backend->cwText = &cwText;
        radio.setBackendForTest(std::move(owned), caps.hostCwKeyingControl ? QStringLiteral("hl2")
                                                                            : QStringLiteral("test"));
        SliceDelta slice;
        slice.frequency = 7.020;
        slice.mode = mode;
        slice.panId = QStringLiteral("0x40000000");
        slice.txSlice = txSlice;
        radio.emitBackendSliceChangedForTest(0, slice);
        if (!radio.slice(0)) {
            std::fprintf(stderr, "FATAL: the slice was not materialised\n");
            std::exit(2);
        }
        backend->connected = true;
    }

    int downs() const
    {
        int n = 0;
        for (const Edge& e : edges) {
            n += e.down ? 1 : 0;
        }
        return n;
    }
};

// One text-CW client: its own producer, and a request renewed once it has
// ended, as rigctld and SmartCAT keep theirs.
struct Sender {
    RadioModel& radio;
    TxCoordinator::Producer producer;
    TxCoordinator::Request request;

    explicit Sender(RadioModel& r) : radio(r), producer(r.registerTxProducer()) {}
    ~Sender() { producer.invalidate(); }

    bool send(const QString& text)
    {
        if (!request.valid()) {
            request = producer.request();
        }
        return radio.requestProducerCwx(request, text);
    }
    bool sendMacro(int index)
    {
        if (!request.valid()) {
            request = producer.request();
        }
        return radio.requestProducerCwx(request, {}, {}, index);
    }
    void stop()
    {
        radio.abortProducerCwx(request);
        request = {};
    }
};

bool pumpUntil(const std::function<bool()>& done, int ms)
{
    QElapsedTimer t;
    t.start();
    QEventLoop loop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (done() || t.elapsed() >= ms) {
            loop.quit();
        }
    });
    poll.start(1);
    loop.exec();
    return done();
}

void pumpFor(int ms)
{
    (void)pumpUntil([] { return false; }, ms);
}

double ms(Clock::duration d)
{
    return std::chrono::duration<double, std::milli>(d).count();
}

// '.'/'-' for a down shorter/longer than two units, ' ' for an up of two units or more.
QString pattern(const std::vector<Edge>& edges, double unitMs, size_t from = 0)
{
    QString out;
    for (size_t i = from; i + 1 < edges.size(); ++i) {
        const double len = ms(edges[i + 1].at - edges[i].at);
        if (edges[i].down) {
            out += len < 2.0 * unitMs ? QLatin1Char('.') : QLatin1Char('-');
        } else if (len >= 2.0 * unitMs) {
            out += QLatin1Char(' ');
        }
    }
    return out;
}

// Key-down lengths in ms, in order.
std::vector<double> downLengths(const std::vector<Edge>& edges)
{
    std::vector<double> out;
    for (size_t i = 0; i + 1 < edges.size(); ++i) {
        if (edges[i].down && !edges[i + 1].down) {
            out.push_back(ms(edges[i + 1].at - edges[i].at));
        }
    }
    return out;
}

bool alternates(const std::vector<Edge>& edges)
{
    for (size_t i = 0; i < edges.size(); ++i) {
        if (edges[i].down != (i % 2 == 0)) {
            return false;
        }
    }
    return true;
}

void testHl2Shape()
{
    const RadioCapabilities caps = hl2Capabilities(true);
    check("HL2: no radio-side buffer, host CW keying engaged",
          !caps.hasRadioSideCwKeyer && caps.hostCwKeyingControl.has_value());
    check("HL2 shape: no progress index, stored macros or live typing",
          !caps.cwTextHasProgress && !caps.cwTextHasStoredMacros && !caps.cwTextSupportsLive);
    check("HL2 shape: per-segment speed modifiers", caps.cwTextSupportsSpeedModifiers);
    check("HL2 shape: the message cap is the engine's",
          caps.cwTextMaxMessageChars == HostCwTextEngine::kMaxMessageChars);
}

void testTimingAndSpeedSource()
{
    Fixture f(hl2Capabilities(true));
    Sender a(f.radio);
    check("HL2: hasHostCwTextKeyer() is true", f.radio.hasHostCwTextKeyer());
    // 15 WPM (an 80 ms unit). The CWX model's own speed differs on purpose.
    f.radio.transmitModel().setCwSpeed(15);
    f.radio.cwxModel().adoptSpeed(30);
    const double unitMs = 1200.0 / 15;
    check("timing: TEST accepted", a.send(QStringLiteral("TEST")));
    check("timing: the CWX speed follows TransmitModel", f.radio.cwxModel().speed() == 15);
    const bool done = pumpUntil([&] { return f.edges.size() >= 12; }, 10000);
    pumpFor(static_cast<int>(10 * unitMs));
    check("timing: twelve edges reach setCwKeying", done && f.edges.size() == 12);
    check("timing: edges alternate down/up, starting down", alternates(f.edges));
    check("timing: the elements spell T E S T", pattern(f.edges, unitMs) == QLatin1String("- . ... -"));
    if (f.edges.size() == 12) {
        const double total = ms(f.edges.back().at - f.edges.front().at);
        std::printf("       total %.1f ms, expected %.1f ms\n", total, 21 * unitMs);
        check("timing: 21 units at TransmitModel's speed (-20 % / +40 %)",
              total > 21 * unitMs * 0.80 && total < 21 * unitMs * 1.40);
    }
    pumpFor(50);
    check("timing: the run ended, the sender's Cwx intent with it", !f.radio.producerRequestHasWork(a.request));
}

// "++++T T": the first T at base + 4 steps (12 + 12 = 24 WPM), the second at base.
void testSpeedModifierSegments()
{
    Fixture f(hl2Capabilities(true));
    Sender a(f.radio);
    f.radio.transmitModel().setCwSpeed(12);
    f.radio.cwxModel().setSpeedStep(3);
    check("segments: ++++T T accepted", a.send(QStringLiteral("++++T T")));
    const bool done = pumpUntil([&] { return f.edges.size() >= 4; }, 5000);
    const std::vector<double> downs = downLengths(f.edges);
    check("segments: two dahs", done && downs.size() == 2);
    if (downs.size() == 2) {
        std::printf("       dahs %.1f ms and %.1f ms (150 / 300 nominal)\n", downs[0], downs[1]);
        check("segments: the modified word keys at 24 WPM", downs[0] < 225.0);
        check("segments: the next word keys at the base 12 WPM", downs[1] > 225.0);
    }
    if (f.edges.size() >= 3) {
        // The space leads the base segment (" T"): CharGap and WordGap at 12 WPM, 7 x 100 ms.
        const double gap = ms(f.edges[2].at - f.edges[1].at);
        std::printf("       word gap %.1f ms, expected 700 ms\n", gap);
        check("segments: a whole word gap after the modified word (-20 % / +40 %)", gap > 560.0 && gap < 980.0);
    }
    // "T ++++T": the space ends the base segment ("T "), so the modified word's
    // first letter owes the CharGap: WordGap at 12 WPM (4 x 100 ms), then 3 x 50 ms.
    Fixture g(hl2Capabilities(true));
    Sender b(g.radio);
    g.radio.transmitModel().setCwSpeed(12);
    g.radio.cwxModel().setSpeedStep(3);
    check("segments: T ++++T accepted", b.send(QStringLiteral("T ++++T")));
    const bool done2 = pumpUntil([&] { return g.edges.size() >= 4; }, 5000);
    check("segments: T ++++T keys two dahs", done2 && g.edges.size() == 4);
    if (g.edges.size() >= 3) {
        const double gap = ms(g.edges[2].at - g.edges[1].at);
        std::printf("       word gap %.1f ms, expected 550 ms\n", gap);
        check("segments: a whole word gap before the modified word (-20 % / +40 %)", gap > 440.0 && gap < 770.0);
    }
    pumpFor(100);
}

// Design point 2 of #7146: an element's key-up, a CwKey edge, is not swallowed
// by its sender's open Cwx intent (the OR rule in dispatchCwInput).
void testCwxIntentLeavesElementKeyUp()
{
    Fixture f(hl2Capabilities(true));
    Sender a(f.radio);
    f.radio.transmitModel().setCwSpeed(20);
    check("intent: TT accepted", a.send(QStringLiteral("TT")));
    const bool up = pumpUntil([&] { return f.edges.size() >= 2; }, 2000);
    check("intent: the first element's key-up reached the backend",
          up && f.edges.size() >= 2 && f.edges[0].down && !f.edges[1].down);
    check("intent: while the sender's Cwx intent was still open", f.radio.producerRequestHasWork(a.request));
    pumpUntil([&] { return f.edges.size() >= 4; }, 2000);
    pumpFor(100);
    check("intent: both elements up and down, the key ends up",
          f.edges.size() == 4 && alternates(f.edges));
}

void testStopMidDah()
{
    Fixture f(hl2Capabilities(true));
    Sender a(f.radio);
    f.radio.transmitModel().setCwSpeed(20);
    check("stop: accepted", a.send(QStringLiteral("MMMMMMMMMM")));
    check("stop: the first key-down arrives", pumpUntil([&] { return !f.edges.empty(); }, 2000));
    pumpFor(20);
    check("stop: precondition, the key is down mid-element", !f.edges.empty() && f.edges.back().down);
    const size_t before = f.edges.size();
    a.stop();
    check("stop: the key is up as soon as the stop returns", f.edges.size() == before + 1 && !f.edges.back().down);
    pumpFor(800);
    check("stop: no further edge follows", f.edges.size() == before + 1);
    f.edges.clear();
    check("stop: a later send accepted", a.send(QStringLiteral("E")));
    check("stop: and keys again", pumpUntil([&] { return f.edges.size() >= 2; }, 2000) && f.edges.size() == 2);
}

// Disconnect: the engine is closed in resetTxOperations(), key up before return.
void testDisconnectMidMessage()
{
    Fixture f(hl2Capabilities(true));
    Sender a(f.radio);
    Sender b(f.radio);
    int dropped = 0;
    QObject::connect(&f.radio, &RadioModel::interlockNotificationRequested,
                     [&](const QString&, const QString& key, const QString&) {
                         dropped += key == QLatin1String("cw-text-queued-dropped") ? 1 : 0;
                     });
    f.radio.transmitModel().setCwSpeed(20);
    check("disconnect: the HL2 connection has its engine", f.radio.hasHostCwTextEngineForTest());
    check("disconnect: accepted", a.send(QStringLiteral("MMMMMMMMMM")));
    check("disconnect: a second sender waits", b.send(QStringLiteral("MMMM")));
    check("disconnect: the first key-down arrives", pumpUntil([&] { return !f.edges.empty(); }, 2000));
    pumpFor(20);
    check("disconnect: precondition, the key is down", !f.edges.empty() && f.edges.back().down);
    const size_t before = f.edges.size();
    f.radio.forceDisconnect();
    const size_t after = f.edges.size();
    bool onlyUps = after > before;
    for (size_t i = before; i < after; ++i) {
        onlyUps = onlyUps && !f.edges[i].down;
    }
    check("disconnect: the key is up when forceDisconnect() returns, every edge since a key-up", onlyUps);
    pumpFor(800);
    check("disconnect: nothing keys afterwards, the waiting sender included", f.edges.size() == after);
    check("disconnect: the engine is destroyed with the session", !f.radio.hasHostCwTextEngineForTest());
    check("disconnect: the engine closed: its run is over and the waiting text discarded, not replayed",
          dropped == 0);
    check("disconnect: no further text is accepted", !a.send(QStringLiteral("E")));
}

void testRefusals()
{
    const auto refused = [](const char* name, Fixture& f, RadioModel::HostCwTextRefusal expected) {
        Sender a(f.radio);
        int notices = 0;
        const QMetaObject::Connection counting = QObject::connect(&f.radio, &RadioModel::interlockNotificationRequested,
            [&](const QString&, const QString& key, const QString&) {
                notices += key.startsWith(QLatin1String("cw-text-refused:")) ? 1 : 0;
            });
        const bool host = f.radio.hasHostCwTextKeyer();
        const QString prefix = QString::fromLatin1(name);
        check((prefix + QStringLiteral(": preflight names the reason")).toUtf8().constData(),
              f.radio.hostCwTextRefusal() == expected);
        check((prefix + QStringLiteral(": requestProducerCwx refuses")).toUtf8().constData(),
              !a.send(QStringLiteral("TEST")));
        pumpFor(300);
        check((prefix + QStringLiteral(": no edge")).toUtf8().constData(), f.edges.empty());
        // N3 (#7347 review): a refused host send names its reason and closes its request.
        if (host) {
            check((prefix + QStringLiteral(": one refusal notice")).toUtf8().constData(), notices == 1);
            check((prefix + QStringLiteral(": the refused request is closed")).toUtf8().constData(),
                  !a.request.valid());
        }
        QObject::disconnect(counting);
    };
    {
        Fixture f(hl2Capabilities(true), QStringLiteral("USB"));
        refused("not CW", f, RadioModel::HostCwTextRefusal::NotCwMode);
    }
    {
        Fixture f(hl2Capabilities(false));
        refused("receive-only HL2", f, RadioModel::HostCwTextRefusal::Unsupported);
    }
    {
        Fixture f(hl2Capabilities(true));
        f.radio.setPanTransmitInhibited(f.radio.slice(0)->panId(), true, QStringLiteral("test inhibit"));
        refused("inhibited", f, RadioModel::HostCwTextRefusal::Inhibited);
    }
    {
        Fixture f(hl2Capabilities(true), QStringLiteral("CW"), /*txSlice=*/false);
        refused("no TX slice", f, RadioModel::HostCwTextRefusal::NoTxSlice);
    }
    {
        Fixture f(hl2Capabilities(true));
        TransmitDelta tune;
        tune.tune = true;
        f.radio.transmitModel().applyChanges(tune);
        refused("TUNE", f, RadioModel::HostCwTextRefusal::Tuning);
    }
    {
        Fixture f(hl2Capabilities(true), QStringLiteral("CW"), true, /*breakIn=*/false);
        refused("break-in off, unkeyed", f, RadioModel::HostCwTextRefusal::BreakInOffUnkeyed);
    }
    {
        RadioCapabilities caps;
        caps.canTransmit = true;
        Fixture f(caps);
        refused("no host CW keying", f, RadioModel::HostCwTextRefusal::Unsupported);
    }
}

void testBreakInOffWithPttHeld()
{
    Fixture f(hl2Capabilities(true), QStringLiteral("CW"), true, /*breakIn=*/false);
    Sender a(f.radio);
    const TxCoordinator::Request ptt = a.producer.request();
    check("break-in off: PTT keyed", f.radio.setProducerTransmit(ptt, true, TransmitModel::PttSource::Dax));
    check("break-in off: MOX/PTT is held", f.radio.transmitModel().isTransmitting());
    check("break-in off, PTT held: accepted", a.send(QStringLiteral("E")));
    check("break-in off, PTT held: keys one dit",
          pumpUntil([&] { return f.edges.size() >= 2; }, 2000) && f.edges.size() == 2
              && f.edges[0].down && !f.edges[1].down);
    (void)f.radio.setProducerTransmit(ptt, false, TransmitModel::PttSource::Dax);
}

void testLengthBounds()
{
    const int maxMessage = HostCwTextEngine::kMaxMessageChars;
    const QString longest(maxMessage, QLatin1Char('E'));
    Fixture f(hl2Capabilities(true));
    Sender a(f.radio);
    check("bounds: 511 per message, 1024 pending",
          maxMessage == 511 && HostCwTextEngine::kMaxPendingChars == 1024);
    int notices = 0;
    QObject::connect(&f.radio, &RadioModel::interlockNotificationRequested,
                     [&](const QString&, const QString& key, const QString&) {
                         notices += key == QLatin1String("cw-text-refused:length") ? 1 : 0;
                     });
    check("bounds: 512 characters refused", !a.send(longest + QLatin1Char('E')));
    pumpFor(200);
    check("bounds: no edge for the refused message", f.edges.empty());
    check("bounds: 511 accepted", a.send(longest));
    check("bounds: a second 511 from the same sender appended (1022)", a.send(longest));
    check("bounds: 3 more refused (1025 > 1024)", !a.send(QStringLiteral("EEE")));
    check("bounds: the refusal names its reason", notices == 1);
    check("bounds: the refusal left the run playing", f.radio.producerRequestHasWork(a.request));
    check("bounds: 2 more still fit (1024)", a.send(QStringLiteral("EE")));
    Sender b(f.radio);
    check("bounds: a waiting sender counts against the same 1024", !b.send(QStringLiteral("E")));
    check("bounds: B's refused send closes its request", !b.request.valid());
    a.stop();
    f.edges.clear();
    const bool sentAfter = a.send(QStringLiteral("E"));
    const bool keyedAfter = pumpUntil([&] { return f.edges.size() >= 2; }, 2000);
    check("bounds: after the stop a send keys", sentAfter && keyedAfter && f.edges.size() == 2);
}

// Review nit 2 on #7347: the 511/1024 bounds apply to a stored macro once
// {CALL} is resolved, and a waiting macro counts its resolved length.
void testMacroBoundsAfterPlaceholder()
{
    Fixture f(hl2Capabilities(true));
    Sender a(f.radio);
    Sender b(f.radio);
    Sender c(f.radio);
    CwxModel& cwx = f.radio.cwxModel();
    cwx.setDxCall(QStringLiteral("ON8ST/QRP"));   // nine characters for {CALL}'s six
    cwx.saveMacro(0, QStringLiteral("{CALL}") + QString(505, QLatin1Char('E')));   // 511 typed, 514 keyed
    cwx.saveMacro(1, QStringLiteral("{CALL}").repeated(50));                        // 300 typed, 450 keyed
    f.radio.transmitModel().setCwSpeed(30);
    check("macro bounds: 511 typed, 514 once {CALL} is resolved: refused", !a.sendMacro(1));
    pumpFor(200);
    check("macro bounds: no edge for it", f.edges.empty());
    check("macro bounds: A plays 511", a.send(QString(511, QLatin1Char('E'))));
    check("macro bounds: B's 450-character macro waits", b.sendMacro(2));
    check("macro bounds: C's 100 more refused (511 + 450 + 100 > 1024)",
          !c.send(QString(100, QLatin1Char('E'))));
    a.stop();
    b.stop();
    pumpFor(50);
}

// A key-down refused mid-message ends the message with one notice. The
// refusal is lifted ten units later; a keyer still running would key on.
void runMidMessageRefusal(const char* label, const std::function<void(Fixture&)>& refuse,
                          const std::function<void(Fixture&)>& lift, const QString& noticeKey)
{
    Fixture f(hl2Capabilities(true));
    Sender a(f.radio);
    int notices = 0;
    QObject::connect(&f.radio, &RadioModel::interlockNotificationRequested,
                     [&](const QString&, const QString& key, const QString&) {
                         notices += key.contains(noticeKey) ? 1 : 0;
                     });
    f.radio.transmitModel().setCwSpeed(30);
    const double unitMs = 1200.0 / 30;
    const QString prefix = QString::fromLatin1(label) + QStringLiteral(": ");
    const auto name = [&](const char* what) { return (prefix + QLatin1String(what)).toUtf8(); };
    check(name("accepted").constData(), a.send(QStringLiteral("TEST TEST")));
    check(name("the first key-down arrives").constData(), pumpUntil([&] { return f.downs() >= 1; }, 2000));
    refuse(f);
    const int downsAtRefusal = f.downs();
    pumpFor(static_cast<int>(10 * unitMs));
    lift(f);
    pumpFor(1500);
    check(name("no key-down after the refusal, none once it is lifted").constData(),
          f.downs() == downsAtRefusal);
    check(name("the key ends up").constData(), !f.edges.empty() && !f.edges.back().down);
    std::printf("       %s notices: %d\n", label, notices);
    check(name("one refusal notice").constData(), notices == 1);
    check(name("the sender's Cwx intent ended").constData(), !f.radio.producerRequestHasWork(a.request));
    f.edges.clear();
    check(name("a later send keys again").constData(),
          a.send(QStringLiteral("E")) && pumpUntil([&] { return f.edges.size() >= 2; }, 2000)
              && f.edges.size() == 2);
}

void setMode(Fixture& f, const QString& mode)
{
    SliceDelta delta;
    delta.mode = mode;
    f.radio.emitBackendSliceChangedForTest(0, delta);
}

void setTune(Fixture& f, bool on)
{
    TransmitDelta delta;
    delta.tune = on;
    f.radio.transmitModel().applyChanges(delta);
}

void testMidMessageRefusals()
{
    runMidMessageRefusal("mode change",
        [](Fixture& f) { setMode(f, QStringLiteral("USB")); },
        [](Fixture& f) { setMode(f, QStringLiteral("CW")); },
        QStringLiteral("cw-text-mode:"));
    runMidMessageRefusal("inhibit",
        [](Fixture& f) {
            f.radio.setPanTransmitInhibited(f.radio.slice(0)->panId(), true, QStringLiteral("test inhibit"));
        },
        [](Fixture& f) { f.radio.setPanTransmitInhibited(f.radio.slice(0)->panId(), false); },
        QStringLiteral(":cw-key"));
    runMidMessageRefusal("break-in turned off",
        [](Fixture& f) { f.radio.transmitModel().setCwBreakIn(false); },
        [](Fixture& f) { f.radio.transmitModel().setCwBreakIn(true); },
        QStringLiteral("cw-text-unkeyed"));
    runMidMessageRefusal("TUNE raised",
        [](Fixture& f) { setTune(f, true); },
        [](Fixture& f) { setTune(f, false); },
        QStringLiteral("cw-text-tuning"));
}

// A sender whose producer dies mid-message, with no abortProducerCwx, keys nothing more.
void testInvalidatedSenderStops()
{
    Fixture f(hl2Capabilities(true));
    Sender a(f.radio);
    Sender b(f.radio);
    f.radio.transmitModel().setCwSpeed(20);
    check("invalidated: A plays, B waits", a.send(QStringLiteral("MMMMMMMMMM")) && b.send(QStringLiteral("E")));
    check("invalidated: first down", pumpUntil([&] { return !f.edges.empty(); }, 2000));
    pumpFor(20);
    const size_t before = f.edges.size();
    a.producer.invalidate();
    pumpFor(2000);
    check("invalidated: A's element keys up, then B's E, nothing more of A",
          f.edges.size() == before + 3 && alternates(f.edges));
    check("invalidated: A's Cwx intent ended", !f.radio.producerRequestHasWork(a.request));
}

// Q2: another sender's text waits behind the playing message, never interleaved.
void testQueueBehindPlaying()
{
    Fixture f(hl2Capabilities(true));
    Sender a(f.radio);
    Sender b(f.radio);
    f.radio.transmitModel().setCwSpeed(20);
    const double unitMs = 60.0;
    check("queue: A's TEST accepted", a.send(QStringLiteral("TEST")));
    check("queue: B's E accepted while A plays", b.send(QStringLiteral("E")));
    check("queue: B has no Cwx intent while it waits", !f.radio.producerRequestHasWork(b.request));
    const bool done = pumpUntil([&] { return f.edges.size() >= 14; }, 10000);
    pumpFor(200);
    check("queue: fourteen edges, alternating", done && f.edges.size() == 14 && alternates(f.edges));
    std::vector<Edge> first(f.edges.begin(), f.edges.begin() + std::min<size_t>(12, f.edges.size()));
    check("queue: A's TEST keys whole first", pattern(first, unitMs) == QLatin1String("- . ... -"));
    check("queue: then B's dit", f.edges.size() == 14
              && ms(f.edges[13].at - f.edges[12].at) < 2.0 * unitMs);
    if (f.edges.size() >= 13) {
        const double gap = ms(f.edges[12].at - f.edges[11].at);
        std::printf("       gap before B %.1f ms, expected %.1f ms\n", gap, 7 * unitMs);
        check("queue: a word gap before B's dit (-20 % / +40 %)", gap > 7 * unitMs * 0.80 && gap < 7 * unitMs * 1.40);
    }
}

// The same sender's messages stay together, ahead of a sender that queued
// between them: A plays, B sends E, C sends T, B sends I -> A, E, I, T.
void testSameSenderStaysTogether()
{
    Fixture f(hl2Capabilities(true));
    Sender a(f.radio);
    Sender b(f.radio);
    Sender c(f.radio);
    f.radio.transmitModel().setCwSpeed(20);
    check("order: A's M accepted", a.send(QStringLiteral("M")));
    check("order: B, C, B accepted",
          b.send(QStringLiteral("E")) && c.send(QStringLiteral("T")) && b.send(QStringLiteral("I")));
    const bool done = pumpUntil([&] { return f.edges.size() >= 12; }, 8000);
    pumpFor(200);
    const std::vector<double> downs = downLengths(f.edges);
    check("order: six elements", done && downs.size() == 6);
    if (downs.size() == 6) {
        const auto dah = [](double len) { return len >= 120.0; };
        check("order: A's two dahs, B's three dits, then C's dah",
              dah(downs[0]) && dah(downs[1]) && !dah(downs[2]) && !dah(downs[3]) && !dah(downs[4])
                  && dah(downs[5]));
    }
}

// abortProducerCwx stops only the caller: its waiting text, or its playing
// text (key up now, and the next sender starts).
void testScopedStop()
{
    Fixture f(hl2Capabilities(true));
    Sender a(f.radio);
    Sender b(f.radio);
    f.radio.transmitModel().setCwSpeed(20);
    check("scoped: A plays, B waits", a.send(QStringLiteral("MM")) && b.send(QStringLiteral("EEEE")));
    b.stop();
    const bool done = pumpUntil([&] { return f.edges.size() >= 8; }, 4000);
    pumpFor(500);
    check("scoped: B's stop leaves A to finish, and B never keys", done && f.edges.size() == 8);

    f.edges.clear();
    check("scoped: A plays again, B waits again",
          a.send(QStringLiteral("MMMMMMMMMM")) && b.send(QStringLiteral("E")));
    check("scoped: A's first key-down", pumpUntil([&] { return !f.edges.empty(); }, 2000));
    pumpFor(20);
    const size_t before = f.edges.size();
    a.stop();
    check("scoped: A's stop keys up at once", f.edges.size() == before + 1 && !f.edges.back().down);
    check("scoped: and B's E plays next",
          pumpUntil([&] { return f.edges.size() >= before + 3; }, 2000) && f.edges.size() == before + 3);
}

// The operator's clear (panel ESC, TCI cw_macros_stop) clears everything.
void testClearAll()
{
    Fixture f(hl2Capabilities(true));
    Sender a(f.radio);
    Sender b(f.radio);
    f.radio.transmitModel().setCwSpeed(20);
    check("clear: A plays, B waits", a.send(QStringLiteral("MMMMMMMMMM")) && b.send(QStringLiteral("EEEE")));
    check("clear: A's first key-down", pumpUntil([&] { return !f.edges.empty(); }, 2000));
    pumpFor(20);
    const size_t before = f.edges.size();
    f.radio.cwxModel().clearBuffer();
    check("clear: the key is up as soon as clearBuffer() returns",
          f.edges.size() == before + 1 && !f.edges.back().down);
    pumpFor(800);
    check("clear: nothing keys afterwards, B's waiting text included", f.edges.size() == before + 1);
    check("clear: A's Cwx intent ended", !f.radio.producerRequestHasWork(a.request));
}

// Q3: the first paddle element aborts host text, queued text included.
void testPaddleTakesOver()
{
    Fixture f(hl2Capabilities(true));
    Sender a(f.radio);
    Sender b(f.radio);
    f.radio.transmitModel().setCwSpeed(20);
    check("paddle: A plays, B waits", a.send(QStringLiteral("MMMMMMMMMM")) && b.send(QStringLiteral("EEEE")));
    check("paddle: A's first key-down", pumpUntil([&] { return !f.edges.empty(); }, 2000));
    pumpFor(20);
    const size_t before = f.edges.size();
    f.radio.queueCwKeyEdge(true, QStringLiteral("paddle"), 0, 0, Clock::now());
    pumpUntil([&] { return f.edges.size() >= before + 2; }, 1000);
    check("paddle: the text keys up, then the paddle keys down",
          f.edges.size() == before + 2 && !f.edges[before].down && f.edges[before + 1].down);
    pumpFor(100);
    f.radio.queueCwKeyEdge(false, QStringLiteral("paddle"), 0, 0, Clock::now());
    pumpFor(800);
    check("paddle: the paddle's key-up, then nothing: no text resumes, B's included",
          f.edges.size() == before + 3 && !f.edges.back().down);
    check("paddle: A's Cwx intent ended", !f.radio.producerRequestHasWork(a.request));
}

// Ruling on #7347: a remote client's key edge (TCI keyer) is not the operator's
// hand, so it does not take over from host text. Nor does it key beside it: its
// key-down is refused with a notice, and its key-up unkeys nothing.
void testRemoteKeyDoesNotTakeOver()
{
    Fixture f(hl2Capabilities(true));
    Sender a(f.radio);
    int notices = 0;
    QObject::connect(&f.radio, &RadioModel::interlockNotificationRequested,
                     [&](const QString&, const QString& key, const QString&) {
                         notices += key == QLatin1String("cw-text-remote-key") ? 1 : 0;
                     });
    f.radio.transmitModel().setCwSpeed(20);
    check("remote: A plays", a.send(QStringLiteral("MM")));
    check("remote: A's first key-down", pumpUntil([&] { return !f.edges.empty(); }, 2000));
    pumpFor(20);
    f.radio.sendRemoteCwKey(true, QStringLiteral("tci:keyer"));
    pumpFor(100);
    f.radio.sendRemoteCwKey(false, QStringLiteral("tci:keyer"));
    check("remote: A's text is still playing", f.radio.producerRequestHasWork(a.request));
    check("remote: A's text keys whole and alone",
          pumpUntil([&] { return !f.radio.producerRequestHasWork(a.request); }, 3000)
              && f.edges.size() == 8 && alternates(f.edges));
    std::printf("       %zu edges for MM with a remote key edge inside it\n", f.edges.size());
    check("remote: one refusal notice", notices == 1);
    pumpFor(100);
    f.radio.sendRemoteCwKey(true, QStringLiteral("tci:keyer"));
    pumpFor(60);
    f.radio.sendRemoteCwKey(false, QStringLiteral("tci:keyer"));
    pumpFor(50);
    check("remote: with no host text playing the remote key keys", f.edges.size() == 10 && alternates(f.edges));
    a.stop();
    pumpFor(50);
}

// A waiting message that is refused when its turn comes is dropped with a notice.
void testWaitingRefusedAtItsTurn()
{
    Fixture f(hl2Capabilities(true));
    Sender a(f.radio);
    Sender b(f.radio);
    int dropped = 0;
    QObject::connect(&f.radio, &RadioModel::interlockNotificationRequested,
                     [&](const QString&, const QString& key, const QString&) {
                         dropped += key == QLatin1String("cw-text-queued-dropped") ? 1 : 0;
                     });
    f.radio.transmitModel().setCwSpeed(30);
    check("turn: A plays, B waits", a.send(QStringLiteral("TEST TEST")) && b.send(QStringLiteral("E")));
    check("turn: A's first key-down", pumpUntil([&] { return !f.edges.empty(); }, 2000));
    setMode(f, QStringLiteral("USB"));
    pumpFor(1500);
    check("turn: the key ends up", !f.edges.empty() && !f.edges.back().down);
    check("turn: B's text was dropped with one notice", dropped == 1);
    setMode(f, QStringLiteral("CW"));
    const size_t before = f.edges.size();
    pumpFor(300);
    check("turn: and does not key later", f.edges.size() == before);
}

// #7338: the station key objection is asked once per message, at its Cwx
// admission, as for radio-side CWX; never per element, and again for a
// message that waited for its turn.
void testKeyObjectionOncePerMessage()
{
    Fixture f(hl2Capabilities(true));
    Sender a(f.radio);
    Sender b(f.radio);
    QObject owner;
    QString objection = QStringLiteral("station objects");
    f.radio.addKeyObjection(&owner, [&objection] { return objection; });
    int notices = 0;
    int dropped = 0;
    QObject::connect(&f.radio, &RadioModel::interlockNotificationRequested,
                     [&](const QString&, const QString& key, const QString&) {
                         notices += key.startsWith(QLatin1String("key-objection:")) ? 1 : 0;
                         dropped += key == QLatin1String("cw-text-queued-dropped") ? 1 : 0;
                     });
    f.radio.transmitModel().setCwSpeed(30);
    check("objection: a message is refused", !a.send(QStringLiteral("E")));
    pumpFor(200);
    check("objection: no edge, one notice", f.edges.empty() && notices == 1);
    objection.clear();
    check("objection: lifted, a message is accepted", a.send(QStringLiteral("TEST TEST")));
    check("objection: B waits behind it", b.send(QStringLiteral("E")));
    check("objection: the first key-down arrives", pumpUntil([&] { return !f.edges.empty(); }, 2000));
    objection = QStringLiteral("station objects");
    const bool ran = pumpUntil([&] { return f.edges.size() >= 24; }, 5000);
    check("objection: raised mid-message, every element still keys (not asked per element)",
          ran && f.edges.size() == 24 && alternates(f.edges));
    pumpFor(300);
    check("objection: B is asked at its turn and refused, nothing more keys",
          f.edges.size() == 24 && dropped == 1);
    check("objection: the next message is asked again and refused", !a.send(QStringLiteral("E")));
    pumpFor(200);
    check("objection: no edge for it", f.edges.size() == 24);
}

// A route-less CwxModel::send() has no sender request to key under: refused, as today.
void testRoutelessSendRefused()
{
    Fixture f(hl2Capabilities(true));
    int unsupported = 0;
    QObject::connect(&f.radio, &RadioModel::interlockNotificationRequested,
                     [&](const QString&, const QString& key, const QString&) {
                         unsupported += key == QLatin1String("tx-operation-unsupported") ? 1 : 0;
                     });
    f.radio.cwxModel().send(QStringLiteral("TEST"));
    pumpFor(300);
    check("route-less: refused with the unsupported-operation notice", unsupported == 1);
    check("route-less: no edge", f.edges.empty());
}

void testRadioSideKeyerUnchanged()
{
    RadioCapabilities caps;
    caps.canTransmit = true;
    caps.hasRadioSideCwKeyer = true;
    Fixture f(caps);
    Sender a(f.radio);
    QStringList requested;
    QObject::connect(&f.radio.cwxModel(), &CwxModel::transmissionRequested,
                     [&](const QString& text, int) { requested << text; });
    check("radio-side: hasHostCwTextKeyer() is false", !f.radio.hasHostCwTextKeyer());
    check("radio-side: no host engine is created", !f.radio.hasHostCwTextEngineForTest());
    check("radio-side: accepted", a.send(QStringLiteral("CQ TEST")));
    pumpFor(300);
    check("radio-side: the text goes through CWX", requested == QStringList{QStringLiteral("CQ TEST")});
    check("radio-side: to the radio's text keyer", f.cwText == QStringList{QStringLiteral("CQ TEST")});
    check("radio-side: and no host-timed element", f.edges.empty());
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("host-cw-text-engine"));
    QCoreApplication app(argc, argv);
    if (!profile.isValid()) {
        return 1;
    }
    testHl2Shape();
    testTimingAndSpeedSource();
    testSpeedModifierSegments();
    testCwxIntentLeavesElementKeyUp();
    testStopMidDah();
    testDisconnectMidMessage();
    testRefusals();
    testBreakInOffWithPttHeld();
    testLengthBounds();
    testMacroBoundsAfterPlaceholder();
    testMidMessageRefusals();
    testInvalidatedSenderStops();
    testQueueBehindPlaying();
    testSameSenderStaysTogether();
    testScopedStop();
    testClearAll();
    testPaddleTakesOver();
    testRemoteKeyDoesNotTakeOver();
    testWaitingRefusedAtItsTurn();
    testKeyObjectionOncePerMessage();
    testRoutelessSendRefused();
    testRadioSideKeyerUnchanged();
    std::printf("%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
