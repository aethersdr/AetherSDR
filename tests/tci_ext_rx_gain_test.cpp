// The receive gain and overload groups of the opt-in TCI extension set
// (`rx_rf_gain`, `rx_auto_rf_gain`, `rx_overload`, RFC #7181): offered only
// where the backend declares the control and has published what it needs, set
// through RadioModel like the desktop controls, re-sent when a receiver or its
// range moves, and notified to enabled clients only. Socket-free; nothing keys.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/TciClient.h"
#include "core/TciExtensionSet.h"
#include "core/TciProtocol.h"
#include "core/TciRxGainExtension.h"
#include "core/TciServer.h"
#include "core/backends/AutoRfGainControl.h"
#include "core/backends/FrontEndOverload.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/PanDisplayRequest.h"
#include "core/backends/RadioCapabilities.h"
#include "core/backends/SliceDelta.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "models/DaxIqModel.h"
#include "models/PanadapterModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>

namespace AetherSDR {

namespace hl2 {
// The real HL2 backend, connected without a socket.
struct Hl2FrontEndOverloadTestAccess {
    static void setConnected(Hl2Backend& b, bool on) { b.m_connected = on; }
};
} // namespace hl2

class TciRxGainExtensionTest {
public:
    static void attach(TciServer& server, TciClient* client, RadioModel& model)
    {
        TciServer::ClientState state;
        state.socket = client;
        state.protocol = new TciProtocol(&model, &server.m_routingState, &server.m_trxMap);
        server.m_clients.append(state);
        QObject::connect(client, &TciClient::textMessageReceived,
                         &server, &TciServer::onTextMessage);
    }
    static QStringList offeredTokens(TciServer& server)
    {
        return server.m_extensions.offeredTokens();
    }
};

namespace {

// A radio whose RF gain lives behind the seam, as the HL2's LNA does. It
// records each write and echoes it to the written pan or, with radioWide, to
// every pan (one shared front end). setBackendForTest() wires no pan-gain or
// arm-settled relay, so the fixture supplies both as `echo` and `settled`.
class GainBackend final : public IRadioBackend, public IAutoRfGainControl {
public:
    bool connected{false};
    bool declaresRfGain{true};
    bool observesOverload{true};
    bool hasAuto{true};
    bool radioWide{false};
    bool refuseArm{false};
    bool armed{false};
    int armCalls{0};
    QStringList panIds;
    QList<QPair<QString, int>> writes;
    std::function<void(const QString&, int)> echo;
    std::function<void(bool)> settled;

    RadioCapabilities capabilities() const override
    {
        RadioCapabilities caps;
        if (declaresRfGain) {
            caps.panDisplayControl = PanDisplayControl{{PanDisplayField::RfGain}, {}};
        }
        if (observesOverload) {
            caps.frontEndOverloadObservation = RadioCapabilities::FrontEndOverloadObservation{};
        }
        return caps;
    }
    bool isConnected() const override { return connected; }
    IAutoRfGainControl* autoRfGainControl() override { return hasAuto ? this : nullptr; }
    void setPanRfGain(const QString& panId, int gainDb) override
    {
        writes.append({panId, gainDb});
        for (const QString& id : radioWide ? panIds : QStringList{panId}) {
            echo(id, gainDb);
        }
    }

    void setArmed(bool on) override
    {
        ++armCalls;
        armed = on && !refuseArm;
        settled(armed);
    }
    bool isArmed() const override { return armed; }
    void setFloorDb(int) override {}
    int floorDb() const override { return 0; }
    int maxFloorDb() const override { return 0; }
    bool setLaw(const QString&) override { return false; }
    QString law() const override { return {}; }
    QStringList laws() const override { return {}; }

    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override { connected = false; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

} // namespace
} // namespace AetherSDR

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

// Past the posted send and the 100 ms notice window.
void settle(int ms = 160)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

QStringList extensionLines(const QStringList& lines)
{
    QStringList out;
    for (const QString& line : lines) {
        if (line.startsWith(QLatin1String("aethersdr_"))) {
            out << line;
        }
    }
    return out;
}

// The server sorts nothing it does not promise to: compare as sets of lines.
bool sameLines(QStringList a, QStringList b)
{
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return a == b;
}

void dump(const char* who, const QStringList& lines)
{
    for (const QString& line : lines) {
        std::printf("      %s< %s\n", who, qPrintable(line));
    }
}

struct Wire {
    QPointer<TciClient> client{new TciClient};
    QStringList rx;
    Wire()
    {
        client->textSink = [this](const QString& text) {
            rx << text;
            return static_cast<qint64>(text.size());
        };
    }
    ~Wire() { delete client.data(); }
    void send(const char* text) { emit client->textMessageReceived(QString::fromLatin1(text)); }
    QStringList take()
    {
        QStringList out = extensionLines(rx);
        rx.clear();
        return out;
    }
};

QStringList lines(std::initializer_list<const char*> list)
{
    QStringList out;
    for (const char* line : list) {
        out << QString::fromLatin1(line);
    }
    return out;
}

// Two receivers, each on its own pan, in the backend's own pan namespace.
struct Fixture {
    RadioModel model;
    GainBackend* backend{nullptr};
    const QString pan0{QStringLiteral("stub-pan-0")};
    const QString pan1{QStringLiteral("stub-pan-1")};

    QHash<QString, PanadapterModel*> pans;  // backend pan id -> model

    Fixture()
    {
        auto owned = std::make_unique<GainBackend>();
        backend = owned.get();
        backend->panIds = {pan0, pan1};
        backend->echo = [this](const QString& id, int gain) { publishGain(id, gain); };
        backend->settled = [this](bool armed) { emit model.autoRfGainArmSettled(armed); };
        model.setBackendForTest(std::move(owned), QStringLiteral("rx-gain-test"));
        for (const QString& id : backend->panIds) {
            const QList<PanadapterModel*> before = model.panadapters();
            emit backend->panCenterBandwidthChanged(id, id == pan0 ? 14.1 : 7.1, 0.192);
            for (PanadapterModel* pan : model.panadapters()) {
                if (!before.contains(pan)) {
                    pans.insert(id, pan);
                }
            }
        }
        if (pans.size() != 2) {
            std::fprintf(stderr, "FATAL: the two pans were not materialised\n");
            std::exit(2);
        }
        addSlice(0, pan0, 14.074);
        backend->connected = true;
    }
    // What RadioModel relays from IRadioBackend::panRfGainInfoChanged and
    // panRfGainChanged in production.
    void publishRange(const QString& id, int low, int high, int step,
                      const QString& unit = QStringLiteral(" dB"))
    {
        pans.value(id)->setRfGainInfo(low, high, step, unit);
    }
    void publishGain(const QString& id, int gain) { pans.value(id)->setRfGain(gain); }
    void addSlice(int sliceId, const QString& panId, double mhz)
    {
        SliceDelta slice;
        slice.frequency = mhz;
        slice.panId = panId;
        model.emitBackendSliceChangedForTest(sliceId, slice);
    }
    PanadapterModel* panOf(int sliceId)
    {
        SliceModel* slice = model.slice(sliceId);
        return slice ? model.panadapter(slice->panId()) : nullptr;
    }
};

void offeredPerBackend()
{
    const struct {
        const char* family;
        bool rfGain;
        bool autoGain;
        bool overload;
    } cases[] = {{"hl2", true, true, true},
                 {"flex", true, false, false},
                 {"icom", true, false, false},
                 {"sim", false, false, false}};
    for (const auto& c : cases) {
        RadioModel model;
        IRadioBackend* backend = model.rebuildBackendForTest(QString::fromLatin1(c.family))
            ? model.backend() : nullptr;
        const RadioCapabilities caps = backend ? backend->capabilities() : RadioCapabilities{};
        const bool rfGain = TciRxGainExtension::declaresPanRfGain(caps);
        const bool autoGain = backend && backend->autoRfGainControl() != nullptr;
        const bool overload = TciRxGainExtension::overloadOffered(true, caps);
        std::printf("      %s: built=%d rf_gain=%d auto=%d overload=%d\n", c.family,
                    backend != nullptr, rfGain, autoGain, overload);
        check(backend && rfGain == c.rfGain && autoGain == c.autoGain && overload == c.overload,
              "each real backend declares exactly the receive controls it has");
    }
    RadioModel rtl;
    if (rtl.rebuildBackendForTest(QStringLiteral("rtl")) && rtl.backend()) {
        const RadioCapabilities caps = rtl.backend()->capabilities();
        check(TciRxGainExtension::declaresPanRfGain(caps)
                  && !rtl.backend()->autoRfGainControl()
                  && !TciRxGainExtension::overloadOffered(true, caps),
              "RTL-SDR declares RF gain, and no automatic gain or overload reading");
    } else {
        std::printf("      rtl: not built into this tree, not checked\n");
    }
    RadioCapabilities caps;
    caps.panDisplayControl = PanDisplayControl{{PanDisplayField::RfGain}, {}};
    caps.frontEndOverloadObservation = RadioCapabilities::FrontEndOverloadObservation{};
    check(!TciRxGainExtension::gainOffered(false, caps, true)
              && !TciRxGainExtension::overloadOffered(false, caps)
              && !TciRxGainExtension::gainOffered(true, caps, false),
          "nothing is offered without a radio, and RF gain not without a published range");
}

void grammar()
{
    using namespace TciRxGainExtension;
    const auto split = [](const char* args) {
        return QString::fromLatin1(args).split(QLatin1Char(','), Qt::KeepEmptyParts);
    };
    check(parseGain(split("0")) && !parseGain(split("0"))->isSet
              && parseGain(split("1,-12"))->isSet && parseGain(split("1,-12"))->db == -12
              && !parseGain(QStringList{}) && !parseGain(split("-1")) && !parseGain(split("0,x"))
              && !parseGain(split("0,1,2")) && !parseGain(split("0,")),
          "only <trx> and <trx>,<db> parse");
    check(parseTrx(split("3")) == 3 && !parseTrx(split("3,1")) && !parseTrx(QStringList{})
              && !parseTrx(split("x")),
          "the range read takes exactly <trx>");
    const GainRange hl2{-12, 48, 1};
    const GainRange flex{-8, 32, 8};
    check(snap(INT_MIN, hl2) == -12 && snap(INT_MAX, hl2) == 48 && snap(20, hl2) == 20,
          "a set is clamped to the range, INT_MIN and INT_MAX included");
    check(snap(13, flex) == 16 && snap(11, flex) == 8 && snap(-3, flex) == 0
              && snap(31, GainRange{0, 31, 4}) == 28,
          "and snapped to the nearest step from the low end, never past the high end");
    const QSet<QString> names{levelName(FrontEndLevel::Unobserved), levelName(FrontEndLevel::Clean),
                              levelName(FrontEndLevel::Marginal), levelName(FrontEndLevel::Hot),
                              levelName(FrontEndLevel::AtFloor)};
    check(names.size() == 5 && names.contains(QStringLiteral("at_floor"))
              && names.contains(QStringLiteral("unobserved")),
          "the five levels have five distinct wire names");
    FrontEndOverload state;
    state.level = FrontEndLevel::AtFloor;
    state.autoArmed = true;
    state.autoOffsetDb = 12;
    state.reason = QStringLiteral("free text, with a comma; and a semicolon");
    check(overloadLine(state) == QStringLiteral("aethersdr_rx_overload:at_floor,true,12;"),
          "the overload line carries level, armed and offset, not the reason");
    state.autoOffsetDb = -3;
    check(overloadLine(state).endsWith(QStringLiteral(",0;")), "a negative offset is sent as 0");
}

void rfGain()
{
    Fixture f;
    TciServer server(&f.model);
    f.backend->observesOverload = false;
    f.backend->hasAuto = false;
    emit f.model.capabilitiesChanged(true, f.backend->capabilities());
    Wire a;  // enabled
    Wire b;  // plain
    Wire c;  // enabled
    TciRxGainExtensionTest::attach(server, a.client, f.model);
    TciRxGainExtensionTest::attach(server, b.client, f.model);
    TciRxGainExtensionTest::attach(server, c.client, f.model);
    settle(600);
    a.take();
    b.take();

    // Before the backend publishes a range the pan holds PanadapterModel's
    // defaults, which are no radio's statement: nothing is offered.
    check(f.panOf(0) && !f.panOf(0)->hasRfGainRange()
              && !TciRxGainExtensionTest::offeredTokens(server).contains(QStringLiteral("rx_rf_gain")),
          "a pan with no published range is not offered rx_rf_gain");
    a.send("aethersdr_ext_enable:true;");
    check(a.take() == lines({"aethersdr_ext:1;"}), "its enable reply lists no item");
    a.send("aethersdr_rx_rf_gain:0,10;aethersdr_rx_rf_gain:0;aethersdr_rx_rf_gain_range:0;");
    settle();
    check(a.take().isEmpty() && f.backend->writes.isEmpty(),
          "a read or set for the item not offered gets no reply and writes nothing");

    // The range lands without capabilitiesChanged: the list is re-sent on its own.
    f.publishRange(f.pan0, -12, 48, 1);
    f.publishGain(f.pan0, 20);
    settle();
    QStringList heard = a.take();
    check(heard == lines({"aethersdr_ext:1,rx_rf_gain;", "aethersdr_rx_rf_gain_range:0,-12,48,1;",
                          "aethersdr_rx_rf_gain:0,20;"}),
          "a published range offers the item, with its state lines, to the enabled client");
    if (heard.size() != 3) {
        dump("a", heard);
    }
    c.send("aethersdr_ext_enable:true;");
    check(c.take() == lines({"aethersdr_ext:1,rx_rf_gain;", "aethersdr_rx_rf_gain_range:0,-12,48,1;",
                             "aethersdr_rx_rf_gain:0,20;"}),
          "a client enabling later gets the same list and state");

    // A second receiver appears, on its own pan, after both enabled.
    f.addSlice(1, f.pan1, 7.074);
    f.publishRange(f.pan1, -12, 48, 1);
    f.publishGain(f.pan1, 10);
    settle(600);
    heard = a.take();
    check(f.model.slice(1) && heard == lines({"aethersdr_rx_rf_gain_range:1,-12,48,1;",
                                              "aethersdr_rx_rf_gain:1,10;"}),
          "a receiver that appears is described with trx 1 and no repeat of the list");
    if (heard.size() != 2) {
        dump("a", heard);
    }
    c.take();
    check(b.take().isEmpty(), "the plain client heard no aethersdr_ line");

    // Reads go to the requester only.
    a.send("aethersdr_rx_rf_gain_range:1;aethersdr_rx_rf_gain:1;");
    check(a.take() == lines({"aethersdr_rx_rf_gain_range:1,-12,48,1;", "aethersdr_rx_rf_gain:1,10;"})
              && c.take().isEmpty(),
          "a read is answered to the requester only");
    a.send("aethersdr_rx_rf_gain:2;aethersdr_rx_rf_gain:2,5;aethersdr_rx_rf_gain;"
           "aethersdr_rx_rf_gain:1,x;aethersdr_rx_rf_gain:1,5,6;aethersdr_rx_rf_gain_range:1,5;"
           "aethersdr_rx_rf_gain_range;");
    settle();
    check(a.take().isEmpty() && f.backend->writes.isEmpty(),
          "an unknown trx, a bare command and malformed arguments are ignored");

    // Set on trx 1: RadioModel's pan path, then the backend's echo to all enabled.
    a.send("aethersdr_rx_rf_gain:1,30;");
    check(f.backend->writes.size() == 1 && f.backend->writes.first().first == f.pan1
              && f.backend->writes.first().second == 30,
          "a set on trx 1 reaches the backend for trx 1's pan, through setPanRfGainFor");
    settle();
    check(a.take() == lines({"aethersdr_rx_rf_gain:1,30;"})
              && c.take() == lines({"aethersdr_rx_rf_gain:1,30;"}) && b.take().isEmpty(),
          "the change is notified once to each enabled client, not to the plain one");
    check(f.panOf(0)->rfGain() == 20, "trx 0's pan is untouched");

    a.send("aethersdr_rx_rf_gain:1,2147483647;");
    settle();
    check(f.backend->writes.last().second == 48 && a.take() == lines({"aethersdr_rx_rf_gain:1,48;"}),
          "a set above the range is applied, and answered, as the maximum");
    c.take();
    a.send("aethersdr_rx_rf_gain:1,100;");
    settle();
    check(a.take() == lines({"aethersdr_rx_rf_gain:1,48;"}) && c.take().isEmpty(),
          "a clamp onto the current value is answered to the sender only");
    a.send("aethersdr_rx_rf_gain:1,48;");
    settle();
    check(a.take().isEmpty() && c.take().isEmpty(), "an exact set that changes nothing stays quiet");

    // A range with a coarser step is re-sent, and sets snap to it.
    f.publishRange(f.pan0, -8, 32, 8);
    f.publishGain(f.pan0, 0);
    settle();
    check(a.take() == lines({"aethersdr_rx_rf_gain_range:0,-8,32,8;", "aethersdr_rx_rf_gain:0,0;"}),
          "a new range is re-sent with the gain");
    c.take();
    a.send("aethersdr_rx_rf_gain:0,13;");
    settle();
    check(f.backend->writes.last().second == 16 && a.take() == lines({"aethersdr_rx_rf_gain:0,16;"}),
          "a set between steps goes to the nearest step");
    c.take();

    // A desktop change (the RF Gain control) is notified the same way.
    f.model.setPanRfGainFor(f.panOf(0)->panId(), 24);
    settle();
    check(a.take() == lines({"aethersdr_rx_rf_gain:0,24;"})
              && c.take() == lines({"aethersdr_rx_rf_gain:0,24;"}) && b.take().isEmpty(),
          "a desktop change reaches enabled clients and not the plain client");

    // One front end for the whole radio: a write to one pan moves both.
    f.publishRange(f.pan0, -12, 48, 1);
    settle();
    a.take();
    c.take();
    f.backend->radioWide = true;
    a.send("aethersdr_rx_rf_gain:0,12;");
    settle();
    heard = a.take();
    check(sameLines(heard, lines({"aethersdr_rx_rf_gain:0,12;", "aethersdr_rx_rf_gain:1,12;"})),
          "on a radio-wide front end, a set is reported for every trx");
    if (heard.size() != 2) {
        dump("a", heard);
    }
    c.take();
    f.backend->radioWide = false;

    // Receiver 1 moves onto pan 0: its lines describe pan 0 now.
    f.addSlice(1, f.pan0, 14.2);
    settle(600);
    heard = a.take();
    check(f.panOf(1) == f.panOf(0)
              && heard == lines({"aethersdr_rx_rf_gain_range:1,-12,48,1;", "aethersdr_rx_rf_gain:1,12;"}),
          "a receiver that moves to another pan is described again");
    if (heard.size() != 2) {
        dump("a", heard);
    }
    c.take();
    a.send("aethersdr_rx_rf_gain:1,6;");
    settle();
    heard = a.take();
    check(f.backend->writes.last().first == f.pan0
              && sameLines(heard, lines({"aethersdr_rx_rf_gain:0,6;", "aethersdr_rx_rf_gain:1,6;"})),
          "two receivers on one pan both report its change");
    c.take();

    // A plain or disabled client's command is dropped.
    const qsizetype writes = f.backend->writes.size();
    b.send("aethersdr_rx_rf_gain:0,1;aethersdr_rx_rf_gain:0;");
    c.send("aethersdr_ext_enable:false;");
    c.send("aethersdr_rx_rf_gain:0,1;aethersdr_rx_rf_gain:0;");
    settle();
    check(f.backend->writes.size() == writes && a.take().isEmpty() && b.take().isEmpty()
              && c.take().isEmpty(),
          "a plain or disabled client's command is dropped and writes nothing");

    // A range in another unit is not a dB range.
    f.publishRange(f.pan0, 0, 100, 1, QStringLiteral("%"));
    settle();
    check(a.take() == lines({"aethersdr_ext:1;"}),
          "a range in percent withdraws the item, with no gain line");
    f.publishRange(f.pan0, -12, 48, 1);
    settle();
    a.take();

    // Lost capability: the list is re-sent without it, and nothing answers.
    f.backend->declaresRfGain = false;
    emit f.model.capabilitiesChanged(true, f.backend->capabilities());
    check(a.take() == lines({"aethersdr_ext:1;"}), "a lost capability re-sends the list without it");
    a.send("aethersdr_rx_rf_gain:0;aethersdr_rx_rf_gain:0,3;");
    f.publishGain(f.pan0, 9);
    settle();
    check(a.take().isEmpty() && f.backend->writes.size() == writes,
          "and the item neither notifies nor answers");
    check(b.rx.isEmpty() || extensionLines(b.rx).isEmpty(), "the plain client received no aethersdr_ byte");
}

void autoGain()
{
    Fixture f;
    f.backend->declaresRfGain = false;
    f.backend->observesOverload = false;
    f.backend->hasAuto = false;
    TciServer server(&f.model);
    Wire a;
    Wire b;
    Wire c;
    TciRxGainExtensionTest::attach(server, a.client, f.model);
    TciRxGainExtensionTest::attach(server, b.client, f.model);
    TciRxGainExtensionTest::attach(server, c.client, f.model);
    settle(600);
    b.take();
    a.send("aethersdr_ext_enable:true;");
    c.send("aethersdr_ext_enable:true;");
    a.take();
    c.take();
    a.send("aethersdr_rx_auto_rf_gain;aethersdr_rx_auto_rf_gain:true;");
    settle();
    check(a.take().isEmpty() && f.backend->armCalls == 0,
          "without an automatic gain control nothing is offered or armed");

    f.backend->hasAuto = true;
    emit f.model.capabilitiesChanged(true, f.backend->capabilities());
    check(a.take() == lines({"aethersdr_ext:1,rx_auto_rf_gain;", "aethersdr_rx_auto_rf_gain:false;"}),
          "a radio with automatic gain offers it, with its state");
    c.take();
    a.send("aethersdr_rx_auto_rf_gain;");
    check(a.take() == lines({"aethersdr_rx_auto_rf_gain:false;"}) && c.take().isEmpty(),
          "a read is answered to the requester only");

    a.send("aethersdr_rx_auto_rf_gain:1;");
    check(f.backend->armCalls == 1 && f.backend->armed, "a set arms through the model's control");
    settle();
    check(a.take() == lines({"aethersdr_rx_auto_rf_gain:true;"})
              && c.take() == lines({"aethersdr_rx_auto_rf_gain:true;"}) && b.take().isEmpty(),
          "the settled arm is notified to every enabled client, not the plain one");
    a.send("aethersdr_rx_auto_rf_gain:true;");
    settle();
    check(f.backend->armCalls == 1 && a.take().isEmpty(), "asking for the current state is quiet");
    a.send("aethersdr_rx_auto_rf_gain:maybe;aethersdr_rx_auto_rf_gain:true,1;"
           "aethersdr_rx_auto_rf_gain:;");
    b.send("aethersdr_rx_auto_rf_gain:false;");
    settle();
    check(f.backend->armCalls == 1 && f.backend->armed && a.take().isEmpty(),
          "malformed sets and a plain client's set change nothing");

    a.send("aethersdr_rx_auto_rf_gain:false;");
    settle();
    a.take();
    c.take();
    f.backend->refuseArm = true;
    a.send("aethersdr_rx_auto_rf_gain:true;");
    settle();
    check(!f.backend->armed && a.take() == lines({"aethersdr_rx_auto_rf_gain:false;"}),
          "a refused arm is answered false");
    c.take();

    // Armed elsewhere (the Auto box, a connect-time restore): notified the same way.
    f.backend->refuseArm = false;
    f.model.autoRfGain()->setArmed(true);
    settle();
    check(a.take() == lines({"aethersdr_rx_auto_rf_gain:true;"})
              && c.take() == lines({"aethersdr_rx_auto_rf_gain:true;"}),
          "an arm from the desktop reaches enabled clients");
    check(extensionLines(b.rx).isEmpty(), "the plain client received no aethersdr_ byte");
}

// The gate, on an injected backend: offered exactly while it declares the
// observation record, with the model's reading (unobserved before any).
void overloadGate()
{
    Fixture f;
    f.backend->declaresRfGain = false;
    f.backend->hasAuto = false;
    f.backend->observesOverload = false;
    TciServer server(&f.model);
    Wire a;
    TciRxGainExtensionTest::attach(server, a.client, f.model);
    settle(600);
    a.send("aethersdr_ext_enable:true;");
    a.take();
    a.send("aethersdr_rx_overload;");
    check(a.take().isEmpty(), "a radio that does not observe its converter is not offered rx_overload");
    f.backend->observesOverload = true;
    emit f.model.capabilitiesChanged(true, f.backend->capabilities());
    check(a.take() == lines({"aethersdr_ext:1,rx_overload;", "aethersdr_rx_overload:unobserved,false,0;"}),
          "a radio that declares it is offered it, unobserved before any reading");
    f.backend->observesOverload = false;
    emit f.model.capabilitiesChanged(true, f.backend->capabilities());
    check(a.take() == lines({"aethersdr_ext:1;"}), "withdrawing the record withdraws the item");
}

// The real HL2 backend under RadioModel, connected without a socket: its own
// signals reach the clients through production's relays.
void hl2Backend()
{
    using Access = hl2::Hl2FrontEndOverloadTestAccess;
    RadioModel model;
    if (!model.rebuildBackendForTest(QStringLiteral("hl2"))) {
        std::printf("      hl2: no backend in this build, not checked\n");
        return;
    }
    auto* hl2 = static_cast<hl2::Hl2Backend*>(model.backend());
    Access::setConnected(*hl2, true);
    emit model.capabilitiesChanged(true, hl2->capabilities());
    TciServer server(&model);
    Wire a;
    Wire b;
    TciRxGainExtensionTest::attach(server, a.client, model);
    TciRxGainExtensionTest::attach(server, b.client, model);
    settle(600);
    b.take();
    a.send("aethersdr_ext_enable:true;");
    QStringList heard = a.take();
    check(model.isConnected()
              && heard == lines({"aethersdr_ext:1,rx_auto_rf_gain,rx_overload;",
                                 "aethersdr_rx_auto_rf_gain:false;",
                                 "aethersdr_rx_overload:unobserved,false,0;"}),
          "the HL2 is offered automatic gain and overload (no pan yet, so no RF gain)");
    if (heard.size() != 3) {
        dump("a", heard);
    }

    FrontEndOverload hot;
    hot.level = FrontEndLevel::Hot;
    hot.autoArmed = true;
    hot.autoOffsetDb = 6;
    hot.reason = QStringLiteral("holding, 6 dB; below");
    FrontEndOverload marginal;
    marginal.level = FrontEndLevel::Marginal;
    FrontEndOverload clean;
    clean.level = FrontEndLevel::Clean;
    emit hl2->frontEndOverloadChanged(marginal);
    emit hl2->frontEndOverloadChanged(hot);
    settle();
    check(a.take() == lines({"aethersdr_rx_overload:hot,true,6;"}),
          "changes in one turn go out as one line with the latest reading, without the reason");
    a.send("aethersdr_rx_overload;aethersdr_rx_overload:1;");
    check(a.take() == lines({"aethersdr_rx_overload:hot,true,6;"}),
          "a read is answered, a read with arguments is not");
    emit hl2->frontEndOverloadChanged(clean);
    QCoreApplication::processEvents();
    emit hl2->frontEndOverloadChanged(marginal);
    QCoreApplication::processEvents();
    emit hl2->frontEndOverloadChanged(hot);
    settle();
    check(a.take() == lines({"aethersdr_rx_overload:clean,false,0;", "aethersdr_rx_overload:hot,true,6;"}),
          "inside the 100 ms window only the leading and the latest reading go out");

    // An arm through TCI reaches the HL2's own control; whatever it settles
    // on (armed, or refused above its baseline ceiling) is what is answered.
    a.send("aethersdr_rx_auto_rf_gain:true;");
    settle();
    const bool armed = model.autoRfGain() && model.autoRfGain()->isArmed();
    heard.clear();
    for (const QString& line : a.take()) {
        if (line.startsWith(QLatin1String("aethersdr_rx_auto_rf_gain:"))) {
            heard << line;
        }
    }
    std::printf("      hl2 arm settled: armed=%d\n", armed);
    check(heard == QStringList{TciRxGainExtension::autoLine(armed)},
          "an HL2 arm request is answered with what the backend settled on");
    if (heard.size() != 1) {
        dump("a", heard);
    }
    check(extensionLines(b.rx).isEmpty(), "the plain client received no aethersdr_ byte");

    Access::setConnected(*hl2, false);
    emit model.capabilitiesChanged(false, hl2->capabilities());
    check(a.take() == lines({"aethersdr_ext:1;"}), "a disconnect withdraws both items");
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-tci-ext-rx-gain-test"));
    DaxIqModel::setPactlRunnerForTest([](const QStringList&) { return quint32{0}; });
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    check(settingsProfile.isValid(), "isolated settings profile");
    offeredPerBackend();
    grammar();
    rfGain();
    autoGain();
    overloadGate();
    hl2Backend();

    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
