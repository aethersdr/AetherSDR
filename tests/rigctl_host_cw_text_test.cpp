// rigctl send_morse / stop_morse on a radio with no radio-side text buffer
// (#7105). The HL2 fixture takes its capabilities from a real, unconnected
// Hl2Backend; only canTransmit is set, as a connected HL2 that granted TX
// declares it. The text is timed by the real CwxLocalKeyer worker and every edge
// is recorded where RadioModel hands it to IRadioBackend::setCwKeying().
// Socket-free: an injected stub backend; nothing is opened and nothing is keyed.

#include "TestSettingsProfile.h"
#include "core/RigctlProtocol.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/SliceDelta.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "models/HostCwTextKeyer.h"
#include "models/CwxModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
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

    // Break-in on by default: the HL2 then raises MOX itself. The break-in-off
    // cases (TransmitModel's default) pass false.
    Fixture(const RadioCapabilities& caps, const QString& family, const QString& mode,
            bool txSlice = true, bool breakIn = true)
    {
        radio.transmitModel().setCwBreakIn(breakIn);
        auto owned = std::make_unique<KeyingStub>();
        backend = owned.get();
        backend->caps = caps;
        backend->edges = &edges;
        backend->cwText = &cwText;
        radio.setBackendForTest(std::move(owned), family);
        SliceDelta slice;
        slice.frequency = 7.020;
        slice.mode = mode;
        slice.panId = QStringLiteral("0x40000000");   // the model maps it to its own pan id
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

QString cat(RigctlProtocol& port, const QString& line)
{
    const QString reply = port.handleLine(line).trimmed();
    QCoreApplication::processEvents();
    return reply;
}

// Runs the event loop (where the keyer's queued edges are forwarded) until
// `done` holds or `ms` passes.
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

// Element pattern of the recorded keying: '.'/'-' for a down shorter/longer
// than two units, ' ' between characters (an up of two units or more).
QString pattern(const std::vector<Edge>& edges, double unitMs)
{
    QString out;
    for (size_t i = 0; i + 1 < edges.size(); ++i) {
        const double len = ms(edges[i + 1].at - edges[i].at);
        if (edges[i].down) {
            out += len < 2.0 * unitMs ? QLatin1Char('.') : QLatin1Char('-');
        } else if (len >= 2.0 * unitMs) {
            out += QLatin1Char(' ');
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

void testHl2KeysTextInMorseTiming()
{
    Fixture f(hl2Capabilities(true), QStringLiteral("hl2"), QStringLiteral("CW"));
    RigctlProtocol port(&f.radio);
    port.setSliceIndex(0);

    check("HL2 fixture: real Hl2Backend declares no radio-side buffer but host CW keying",
          !f.backend->caps.hasRadioSideCwKeyer && f.backend->caps.hostCwKeyingControl.has_value());
    check("HL2: hasHostCwTextKeyer() is true", f.radio.hasHostCwTextKeyer());

    check("L KEYSPD 30 accepted", cat(port, QStringLiteral("L KEYSPD 30")) == QLatin1String("RPRT 0"));
    check("the model's CW speed is 30 WPM", f.radio.transmitModel().cwSpeed() == 30);
    // ITU timing: one unit is 1.2 s / WPM.
    const double unitMs = 1200.0 / f.radio.transmitModel().cwSpeed();

    const QString reply = cat(port, QStringLiteral("+b TEST"));
    check("+b TEST answers RPRT 0", reply.endsWith(QLatin1String("RPRT 0")));

    // T E S T = 6 elements, 12 edges.
    const bool done = pumpUntil([&] { return f.edges.size() >= 12; }, 5000);
    pumpFor(static_cast<int>(10 * unitMs));
    check("twelve edges reach setCwKeying", done && f.edges.size() == 12);
    check("edges alternate down/up, starting down", alternates(f.edges));
    check("the elements spell T E S T", pattern(f.edges, unitMs) == QLatin1String("- . ... -"));
    if (f.edges.size() == 12) {
        // T(3) 3 E(1) 3 S(1+1+1+1+1) 3 T(3) = 21 units from first down to last up.
        const double total = ms(f.edges.back().at - f.edges.front().at);
        std::printf("       total %.1f ms, expected %.1f ms at %d WPM\n",
                    total, 21 * unitMs, f.radio.transmitModel().cwSpeed());
        check("the message lasts 21 units at the configured speed (within 15 %)",
              total > 21 * unitMs * 0.85 && total < 21 * unitMs * 1.15);
    }

    // The two-line form: a bare `b`, the text on the next line.
    f.edges.clear();
    check("bare b arms the next line", cat(port, QStringLiteral("b")).isEmpty());
    check("the next line is sent: RPRT 0", cat(port, QStringLiteral("E")).endsWith(QLatin1String("RPRT 0")));
    check("and keys one dit", pumpUntil([&] { return f.edges.size() >= 2; }, 2000)
                                  && f.edges.size() == 2 && f.edges[0].down && !f.edges[1].down);
}

void testStopMorseKeysUpAtOnce()
{
    Fixture f(hl2Capabilities(true), QStringLiteral("hl2"), QStringLiteral("CW"));
    RigctlProtocol port(&f.radio);
    port.setSliceIndex(0);
    (void)cat(port, QStringLiteral("L KEYSPD 20"));

    // M: two dahs, so the key is down for 180 ms at a time.
    check("send_morse accepted",
          cat(port, QStringLiteral("\\send_morse MMMMMMMMMM")).endsWith(QLatin1String("RPRT 0")));
    check("the first key-down arrives", pumpUntil([&] { return !f.edges.empty(); }, 2000));
    pumpFor(20);
    check("precondition: the key is down mid-element", !f.edges.empty() && f.edges.back().down);

    const size_t before = f.edges.size();
    check("stop_morse answers RPRT 0",
          cat(port, QStringLiteral("\\stop_morse")).endsWith(QLatin1String("RPRT 0")));
    check("the key is up as soon as the stop is processed",
          f.edges.size() == before + 1 && !f.edges.back().down);
    pumpFor(800);
    check("and no further edge follows", f.edges.size() == before + 1);

    // A later send starts a fresh run.
    f.edges.clear();
    check("send after stop accepted", cat(port, QStringLiteral("b E")).endsWith(QLatin1String("RPRT 0")));
    check("and keys again", pumpUntil([&] { return f.edges.size() >= 2; }, 2000) && f.edges.size() == 2);
}

void testRefusals()
{
    {
        Fixture f(hl2Capabilities(true), QStringLiteral("hl2"), QStringLiteral("USB"));
        RigctlProtocol port(&f.radio);
        port.setSliceIndex(0);
        check("not CW: send_morse refused with RIG_ERJCTED",
              cat(port, QStringLiteral("b TEST")) == QLatin1String("RPRT -9"));
        pumpFor(300);
        check("not CW: no edge", f.edges.empty());
    }
    {
        // An unconnected Hl2Backend reports canTransmit=false: receive-only.
        Fixture f(hl2Capabilities(false), QStringLiteral("hl2"), QStringLiteral("CW"));
        RigctlProtocol port(&f.radio);
        port.setSliceIndex(0);
        check("receive-only: send_morse refused with RIG_ENAVAIL",
              cat(port, QStringLiteral("b TEST")) == QLatin1String("RPRT -11"));
        check("receive-only: stop_morse refused with RIG_ENAVAIL",
              cat(port, QStringLiteral("\\stop_morse")) == QLatin1String("RPRT -11"));
        pumpFor(300);
        check("receive-only: no edge", f.edges.empty());
    }
    {
        Fixture f(hl2Capabilities(true), QStringLiteral("hl2"), QStringLiteral("CW"));
        f.radio.setPanTransmitInhibited(f.radio.slice(0)->panId(), true, QStringLiteral("test inhibit"));
        RigctlProtocol port(&f.radio);
        port.setSliceIndex(0);
        check("inhibited: send_morse refused with RIG_ERJCTED",
              cat(port, QStringLiteral("b TEST")) == QLatin1String("RPRT -9"));
        pumpFor(300);
        check("inhibited: no edge", f.edges.empty());
    }
    {
        Fixture f(hl2Capabilities(true), QStringLiteral("hl2"), QStringLiteral("CW"), /*txSlice=*/false);
        RigctlProtocol port(&f.radio);
        port.setSliceIndex(0);
        check("no TX slice: send_morse refused with RIG_ERJCTED",
              cat(port, QStringLiteral("b TEST")) == QLatin1String("RPRT -9"));
        pumpFor(300);
        check("no TX slice: no edge", f.edges.empty());
    }
    {
        // A radio that transmits but leaves hostCwKeyingControl absent (the
        // default setCwKeying is a no-op) keeps the old answer.
        RadioCapabilities caps;
        caps.canTransmit = true;
        check("default capabilities: hostCwKeyingControl is absent",
              !caps.hostCwKeyingControl.has_value());
        Fixture f(caps, QStringLiteral("test"), QStringLiteral("CW"));
        RigctlProtocol port(&f.radio);
        port.setSliceIndex(0);
        check("no host CW keying: hasHostCwTextKeyer() is false", !f.radio.hasHostCwTextKeyer());
        check("no host CW keying: send_morse refused with RIG_ENAVAIL",
              cat(port, QStringLiteral("b TEST")) == QLatin1String("RPRT -11"));
    }
}

// Break-in off (TransmitModel's default): the HL2 keys CW only under an MOX/PTT
// already held, so without one the text is refused rather than answered RPRT 0
// with nothing on air. With `T 1` first it keys.
void testBreakInOff()
{
    {
        Fixture f(hl2Capabilities(true), QStringLiteral("hl2"), QStringLiteral("CW"),
                  /*txSlice=*/true, /*breakIn=*/false);
        RigctlProtocol port(&f.radio);
        port.setSliceIndex(0);
        check("break-in off: precondition, break-in is off and nothing is keyed",
              !f.radio.transmitModel().cwBreakIn() && !f.radio.transmitModel().isTransmitting());
        check("break-in off, not keyed: preflight refuses",
              f.radio.hostCwTextRefusal() == RadioModel::HostCwTextRefusal::BreakInOffUnkeyed);
        check("break-in off, not keyed: send_morse refused with RIG_ERJCTED",
              cat(port, QStringLiteral("b TEST")) == QLatin1String("RPRT -9"));
        pumpFor(300);
        check("break-in off, not keyed: no edge", f.edges.empty());
    }
    {
        Fixture f(hl2Capabilities(true), QStringLiteral("hl2"), QStringLiteral("CW"),
                  /*txSlice=*/true, /*breakIn=*/false);
        RigctlProtocol port(&f.radio);
        port.setSliceIndex(0);
        check("break-in off: T 1 accepted", cat(port, QStringLiteral("T 1")) == QLatin1String("RPRT 0"));
        check("break-in off: MOX/PTT is held", f.radio.transmitModel().isTransmitting());
        check("break-in off, PTT held: send_morse RPRT 0",
              cat(port, QStringLiteral("b E")).endsWith(QLatin1String("RPRT 0")));
        check("break-in off, PTT held: keys one dit",
              pumpUntil([&] { return f.edges.size() >= 2; }, 2000)
                  && f.edges.size() == 2 && f.edges[0].down && !f.edges[1].down);
        (void)cat(port, QStringLiteral("T 0"));
    }
}

// Length bounds (HostCwTextKeyer, Hamlib's MAXARGSZ 511 and HAMLIB_FIFO_SIZE
// 1024): a longer message, or one that would take the pending text past 1024
// characters, is refused with no edge. Counted both once the keyer holds the
// text and while a send() is still waiting in the event queue.
void testLengthBounds()
{
    const int maxMessage = HostCwTextKeyer::kMaxMessageChars;
    const QString longest(maxMessage, QLatin1Char('E'));
    {
        Fixture f(hl2Capabilities(true), QStringLiteral("hl2"), QStringLiteral("CW"));
        RigctlProtocol port(&f.radio);
        port.setSliceIndex(0);
        check("bounds: the message cap is 511 and the pending cap 1024",
              maxMessage == 511 && HostCwTextKeyer::kMaxPendingChars == 1024);
        check("bounds: 512 characters refused with RIG_ERJCTED",
              cat(port, QStringLiteral("b ") + longest + QLatin1Char('E')) == QLatin1String("RPRT -9"));
        pumpFor(300);
        check("bounds: no edge for the refused message", f.edges.empty());

        check("bounds: 511 characters accepted",
              cat(port, QStringLiteral("b ") + longest).endsWith(QLatin1String("RPRT 0")));
        check("bounds: a second 511 accepted (1022 pending)",
              cat(port, QStringLiteral("b ") + longest).endsWith(QLatin1String("RPRT 0")));
        check("bounds: 3 more refused once the keyer holds 1022 (1025 > 1024)",
              cat(port, QStringLiteral("b EEE")) == QLatin1String("RPRT -9"));
        check("bounds: 2 more still fit (1024)",
              cat(port, QStringLiteral("b EE")).endsWith(QLatin1String("RPRT 0")));
        check("bounds: stop_morse", cat(port, QStringLiteral("\\stop_morse")).endsWith(QLatin1String("RPRT 0")));
        f.edges.clear();
        check("bounds: after stop_morse nothing is pending and a send keys",
              cat(port, QStringLiteral("b E")).endsWith(QLatin1String("RPRT 0"))
                  && pumpUntil([&] { return f.edges.size() >= 2; }, 2000) && f.edges.size() == 2);
    }
    {
        // Pipelined: three lines handled with no event-loop turn between, so
        // no send() has run yet; the queued text alone takes the bound.
        Fixture f(hl2Capabilities(true), QStringLiteral("hl2"), QStringLiteral("CW"));
        RigctlProtocol port(&f.radio);
        port.setSliceIndex(0);
        const bool first = port.handleLine(QStringLiteral("b ") + longest).trimmed() == QLatin1String("RPRT 0");
        const bool second = port.handleLine(QStringLiteral("b ") + longest).trimmed() == QLatin1String("RPRT 0");
        const QString third = port.handleLine(QStringLiteral("b EEE")).trimmed();
        check("bounds, pipelined: two 511s accepted before any is delivered", first && second);
        check("bounds, pipelined: 3 more refused with RIG_ERJCTED", third == QLatin1String("RPRT -9"));
        (void)cat(port, QStringLiteral("\\stop_morse"));
    }
}

// A key-down refused mid-message ends the message (the keyer stops, as
// stop_morse does). The refusal is lifted ten units later, after the next
// element (T then E: the next key-down is six units in): a keyer still
// running would key the rest of the text from then on.
void runMidMessageRefusal(const char* label, const std::function<void(Fixture&)>& refuse,
                          const std::function<void(Fixture&)>& lift, const QString& noticeKey)
{
    Fixture f(hl2Capabilities(true), QStringLiteral("hl2"), QStringLiteral("CW"));
    RigctlProtocol port(&f.radio);
    port.setSliceIndex(0);
    int notices = 0;
    QObject::connect(&f.radio, &RadioModel::interlockNotificationRequested,
                     [&](const QString&, const QString& key, const QString&) {
                         notices += key.contains(noticeKey) ? 1 : 0;
                     });
    (void)cat(port, QStringLiteral("L KEYSPD 30"));
    const double unitMs = 1200.0 / f.radio.transmitModel().cwSpeed();
    const QString prefix = QString::fromLatin1(label) + QStringLiteral(": ");
    const auto name = [&](const char* what) { return (prefix + QLatin1String(what)).toUtf8(); };

    check(name("send_morse accepted").constData(),
          cat(port, QStringLiteral("b TEST TEST")).endsWith(QLatin1String("RPRT 0")));
    check(name("the first key-down arrives").constData(),
          pumpUntil([&] { return f.downs() >= 1; }, 2000));
    refuse(f);
    const int downsAtRefusal = f.downs();
    pumpFor(static_cast<int>(10 * unitMs));
    lift(f);
    pumpFor(2000);
    check(name("no key-down after the refusal").constData(), f.downs() == downsAtRefusal);
    check(name("and the key ends up").constData(), !f.edges.empty() && !f.edges.back().down);
    check(name("the keyer stopped: nothing keys once the refusal is lifted").constData(),
          f.downs() == downsAtRefusal);
    std::printf("       %s notices: %d\n", label, notices);
    check(name("one refusal notice").constData(), notices == 1);
    // The message was ended, not the keyer: a new send keys.
    f.edges.clear();
    (void)cat(port, QStringLiteral("b E"));
    check(name("a later send keys again").constData(),
          pumpUntil([&] { return f.edges.size() >= 2; }, 2000) && f.edges.size() == 2);
}

void setMode(Fixture& f, const QString& mode)
{
    SliceDelta delta;
    delta.mode = mode;
    f.radio.emitBackendSliceChangedForTest(0, delta);
}

void testMidMessageRefusalsEndTheMessage()
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
        QStringLiteral(":cw-key"));   // the key-down refusal, not the pan notice
    runMidMessageRefusal("break-in turned off",
        [](Fixture& f) { f.radio.transmitModel().setCwBreakIn(false); },
        [](Fixture& f) { f.radio.transmitModel().setCwBreakIn(true); },
        QStringLiteral("cw-text-unkeyed"));
}

void testRadioSideKeyerStillUsesCwx()
{
    RadioCapabilities caps;
    caps.canTransmit = true;
    caps.hasRadioSideCwKeyer = true;
    Fixture f(caps, QStringLiteral("test"), QStringLiteral("CW"));
    RigctlProtocol port(&f.radio);
    port.setSliceIndex(0);
    QStringList requested;
    QObject::connect(&f.radio.cwxModel(), &CwxModel::transmissionRequested,
                     [&](const QString& text, int) { requested << text; });

    check("radio-side keyer: hasHostCwTextKeyer() is false", !f.radio.hasHostCwTextKeyer());
    check("radio-side keyer: send_morse RPRT 0",
          cat(port, QStringLiteral("b CQ TEST")).endsWith(QLatin1String("RPRT 0")));
    pumpFor(300);
    check("the text goes through CWX", requested == QStringList{QStringLiteral("CQ TEST")});
    check("and to the radio's text keyer", f.cwText == QStringList{QStringLiteral("CQ TEST")});
    check("and no host-timed element is keyed", f.edges.empty());
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("rigctl-host-cw-text"));
    QCoreApplication app(argc, argv);
    if (!profile.isValid()) {
        return 1;
    }
    testHl2KeysTextInMorseTiming();
    testStopMorseKeysUpAtOnce();
    testRefusals();
    testBreakInOff();
    testLengthBounds();
    testMidMessageRefusalsEndTheMessage();
    testRadioSideKeyerStillUsesCwx();
    std::printf("%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
