// The span group of the opt-in TCI extension set (`aethersdr_rx_span_rates`,
// `aethersdr_rx_span`, RFC #7181): offered only where the backend declares a
// span control, set through RadioModel::requestPanBandwidth() like the
// desktop's span controls, snapped to the sample rates where span is the rate,
// refused while keyed there, answered with the span the radio reports, and
// every move (span, receiver, pan, capability) notified to enabled clients
// only. Two pans and two receivers, so trx 1 is exercised. Socket-free.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/TciClient.h"
#include "core/TciExtensionSet.h"
#include "core/TciProtocol.h"
#include "core/TciServer.h"
#include "core/TciSpanExtension.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/PanDisplayRequest.h"
#include "core/backends/RadioCapabilities.h"
#include "core/backends/SliceDelta.h"
#include "core/backends/TransmitDelta.h"
#include "models/DaxIqModel.h"
#include "models/PanadapterModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <cstdio>
#include <memory>

namespace AetherSDR {

class TciSpanExtensionTest {
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
    static TxCoordinator& coordinator(RadioModel& model) { return model.m_txCoordinator; }
};

namespace {

// Connected; pans "p0".."pN" and their spans are the test's. How a span
// request is answered is chosen per step: at once (snapped to the rates when
// span is the rate, every pan when radio-wide), later (an HL2 rate change
// rebuilds the receive chains first), by re-asserting the span it kept (the
// HL2's link-budget refusal), or never.
class SpanBackend final : public IRadioBackend {
public:
    enum class Answer { Now, Later, Keep, Never };
    RadioCapabilities caps;
    bool connected{true};
    Answer answer{Answer::Now};
    QStringList pans;
    QHash<QString, double> spanMhz;
    QHash<QString, double> centerMhz;
    QList<QPair<QString, double>> requests;  // pan, Hz
    QList<QPair<QString, double>> pending;

    SpanBackend()
    {
        caps.canTransmit = true;
        caps.maxSlices = 4;
        caps.maxPanadapters = 4;
        declareSpan(true);
    }
    void declareSpan(bool on)
    {
        caps.panDisplayControl = PanDisplayControl{{PanDisplayField::Geometry},
            on ? QSet<PanGeometryComponent>{PanGeometryComponent::Center,
                                            PanGeometryComponent::Bandwidth}
               : QSet<PanGeometryComponent>{PanGeometryComponent::Center}};
    }
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connected; }
    void addPan(const QString& id, double center, double span)
    {
        pans << id;
        centerMhz.insert(id, center);
        spanMhz.insert(id, span);
        emit panCenterBandwidthChanged(id, center, span);
    }
    void report(const QString& id) { emit panCenterBandwidthChanged(id, centerMhz.value(id), spanMhz.value(id)); }
    void apply(const QString& panId, double hz)
    {
        const bool radioWide = caps.panSpanModel && caps.panSpanModel->radioWide;
        for (const QString& id : std::as_const(pans)) {
            if (radioWide || id == panId) {
                spanMhz.insert(id, hz / 1.0e6);
                report(id);
            }
        }
    }
    void finish()
    {
        const auto queued = pending;
        pending.clear();
        for (const auto& [panId, hz] : queued) {
            apply(panId, hz);
        }
    }
    void setPanBandwidth(const QString& panId, double hz) override
    {
        requests.append({panId, hz});
        switch (answer) {
        case Answer::Now: apply(panId, hz); break;
        case Answer::Later: pending.append({panId, hz}); break;
        case Answer::Keep: report(panId); break;
        case Answer::Never: break;
        }
    }
    void setTxSlice(int) override {}
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
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
        if (line.contains(QLatin1String("aethersdr_"), Qt::CaseInsensitive)) {
            out << line;
        }
    }
    return out;
}

struct Wire {
    QPointer<TciClient> client{new TciClient};
    QStringList rx;
    QStringList all;  // everything, never cleared
    Wire()
    {
        client->textSink = [this](const QString& text) {
            rx << text;
            all << text;
            return static_cast<qint64>(text.size());
        };
    }
    ~Wire() { delete client.data(); }
    void send(const char* text) { emit client->textMessageReceived(QString::fromLatin1(text)); }
    // The extension lines received since the last take, sorted: lines sent in
    // one notice are in trx order, but nothing below depends on that.
    QStringList take()
    {
        QStringList out = extensionLines(rx);
        rx.clear();
        out.sort();
        return out;
    }
};

QStringList lines(std::initializer_list<const char*> texts)
{
    QStringList out;
    for (const char* text : texts) {
        out << QString::fromLatin1(text);
    }
    out.sort();
    return out;
}

void dump(const char* who, const QStringList& got)
{
    for (const QString& line : got) {
        std::printf("      %s< %s\n", who, qPrintable(line));
    }
}

bool expect(Wire& wire, const QStringList& wanted, const char* who = "a")
{
    const QStringList got = wire.take();
    if (got != wanted) {
        dump(who, got);
    }
    return got == wanted;
}

// The real backends, as the production family switch builds them.
void offeredPerBackend()
{
    const struct {
        const char* family;
        bool offered;
        bool follows;
        bool radioWide;
    } cases[] = {{"hl2", true, true, true}, {"anan", true, true, false},
                 {"flex", true, false, false}, {"icom", true, false, false},
                 {"sim", false, false, false}};
    for (const auto& c : cases) {
        RadioModel model;
        const bool built = model.rebuildBackendForTest(QString::fromLatin1(c.family))
            && model.backend();
        const RadioCapabilities caps = built ? model.backend()->capabilities() : RadioCapabilities{};
        const bool offered = built && TciSpanExtension::offered(true, caps);
        const TciSpanExtension::Model m = TciSpanExtension::model(caps);
        std::printf("      %s: built=%d offered=%d follows=%d radioWide=%d rates=%d\n", c.family,
                    built, offered, m.followsRate, m.radioWide, int(m.rates.size()));
        check(built && offered == c.offered && m.followsRate == c.follows
                  && m.radioWide == c.radioWide,
              "a backend is offered rx_span exactly when it declares a span control, "
              "with its own span model");
        if (QLatin1String(c.family) == QLatin1String("hl2")) {
            check(TciSpanExtension::ratesLine(m, m.rates)
                      == QStringLiteral("aethersdr_rx_span_rates:true,true,48000,96000,192000,384000;"),
                  "the HL2's rates are its four DDC sample rates, radio-wide");
        }
    }
    RadioModel rtl;
    if (rtl.rebuildBackendForTest(QStringLiteral("rtl")) && rtl.backend()) {
        const RadioCapabilities caps = rtl.backend()->capabilities();
        check(TciSpanExtension::offered(true, caps) && !TciSpanExtension::model(caps).followsRate,
              "RTL-SDR is offered a continuous span");
    } else {
        std::printf("      rtl: not built into this tree, not checked\n");
    }
    RadioModel hl2;
    hl2.rebuildBackendForTest(QStringLiteral("hl2"));
    check(hl2.backend() && !TciSpanExtension::offered(false, hl2.backend()->capabilities()),
          "no radio connected, nothing offered");
    RadioCapabilities noRates;
    noRates.panDisplayControl = PanDisplayControl{{PanDisplayField::Geometry},
        {PanGeometryComponent::Center, PanGeometryComponent::Bandwidth}};
    noRates.panSpanModel = PanSpanModel{true, false};
    check(!TciSpanExtension::offered(true, noRates),
          "a span that is the sample rate, with no rate declared, is not offered");
}

void grammar()
{
    const auto parse = [](const char* args) {
        return TciSpanExtension::parseSpan(
            QString::fromLatin1(args).split(QLatin1Char(','), Qt::KeepEmptyParts));
    };
    check(parse("0") && !parse("0")->isSet && parse("1,96000") && parse("1,96000")->isSet
              && parse("1,96000")->hz == 96000 && parse("0,5000000000")->hz == 5000000000LL,
          "<trx> reads and <trx>,<hz> sets");
    check(!parse("") && !parse("-1") && !parse("x") && !parse("0,0") && !parse("0,-48000")
              && !parse("0,abc") && !parse("0,96000,1") && !parse("0,"),
          "anything else does not parse");

    TciSpanExtension::Model rate;
    rate.followsRate = true;
    rate.rates = {48000, 96000, 192000, 384000};
    using TciSpanExtension::snap;
    check(snap(150000, rate, 0, 0) == 192000 && snap(130000, rate, 0, 0) == 96000
              && snap(1, rate, 0, 0) == 48000 && snap(10000000, rate, 0, 0) == 384000,
          "where span is the rate, a set snaps to the nearest rate by ratio");
    check(snap(384000, rate, 48000, 96000) == 96000
              && TciSpanExtension::reachableRates(rate, 48000, 96000) == QVector<int>({48000, 96000}),
          "and only to the rates inside the pan's reported limits");
    TciSpanExtension::Model continuous;
    check(snap(1000000, continuous, 5000, 500000) == 500000 && snap(10, continuous, 5000, 500000) == 5000
              && snap(123456, continuous, 0, 0) == 123456,
          "a continuous span is clamped to known limits, and passed on without them");
    check(TciSpanExtension::ratesLine(continuous, {}) == QStringLiteral("aethersdr_rx_span_rates:false,false;"),
          "a continuous span lists no rates");
}

struct Fixture {
    RadioModel model;
    SpanBackend* backend{nullptr};
    std::unique_ptr<TciServer> server;

    Fixture(bool followsRate, bool radioWide, double span0, double span1)
    {
        auto owner = std::make_unique<SpanBackend>();
        backend = owner.get();
        backend->caps.panSpanModel = PanSpanModel{followsRate, radioWide};
        if (followsRate) {
            backend->caps.sampleRatesHz = {48000, 96000, 192000, 384000};
        }
        model.setBackendForTest(std::move(owner), QStringLiteral("test"));
        server = std::make_unique<TciServer>(&model);
        backend->addPan(QStringLiteral("p0"), 7.1, span0);
        backend->addPan(QStringLiteral("p1"), 14.1, span1);
        addSlice(0, "p0", 7.074);
        addSlice(1, "p1", 14.074);
    }
    void addSlice(int id, const char* pan, double mhz)
    {
        SliceDelta delta;
        delta.panId = QString::fromLatin1(pan);
        delta.frequency = mhz;
        delta.mode = QStringLiteral("USB");
        model.emitBackendSliceChangedForTest(id, delta);
    }
    void moveSlice(int id, const char* pan)
    {
        SliceDelta delta;
        delta.panId = QString::fromLatin1(pan);
        model.emitBackendSliceChangedForTest(id, delta);
    }
    // What RadioModel does with a backend's panBandwidthLimitsChanged
    // (setupBackend(), which the test backend does not go through).
    void limits(double minMhz, double maxMhz)
    {
        for (PanadapterModel* pan : model.panadapters()) {
            pan->setBandwidthLimits(minMhz, maxMhz);
        }
    }
    void republishCapabilities() { emit model.capabilitiesChanged(true, backend->caps); }
    void key(bool on)
    {
        TransmitDelta delta;
        delta.mox = on;
        model.applyBackendTransmitDeltaForTest(delta);
    }
};

// A Hermes-Lite 2's shape: the span is the sample rate, one for the radio.
void rateRadioWide()
{
    Fixture f(true, true, 0.096, 0.096);
    SpanBackend* backend = f.backend;
    TciServer& server = *f.server;
    check(f.model.isConnected() && f.model.slices().size() == 2 && f.model.panadapters().size() == 2,
          "two receivers on two pans of a connected radio");
    backend->declareSpan(false);
    f.republishCapabilities();
    Wire a;  // enabled
    Wire b;  // plain
    Wire c;  // enabled later
    TciSpanExtensionTest::attach(server, a.client, f.model);
    TciSpanExtensionTest::attach(server, b.client, f.model);
    TciSpanExtensionTest::attach(server, c.client, f.model);
    settle(600);
    a.take();
    b.take();
    c.take();

    // Gate: no span control declared, nothing offered.
    check(TciSpanExtensionTest::offeredTokens(server).isEmpty(),
          "a radio without a span control is not offered rx_span");
    a.send("aethersdr_ext_enable:true;");
    check(expect(a, lines({"aethersdr_ext:1;"})), "its enable reply lists no item");
    a.send("aethersdr_rx_span_rates;aethersdr_rx_span:1;aethersdr_rx_span:1,192000;");
    settle();
    check(a.take().isEmpty() && backend->requests.isEmpty(),
          "a read or set for the item not offered gets no reply and writes nothing");

    backend->declareSpan(true);
    f.republishCapabilities();
    const QStringList offeredState = lines({"aethersdr_ext:1,rx_span;",
        "aethersdr_rx_span_rates:true,true,48000,96000,192000,384000;",
        "aethersdr_rx_span:0,96000;", "aethersdr_rx_span:1,96000;"});
    check(expect(a, offeredState), "once declared, the list offers it with the rates and every trx's span");
    settle();
    check(expect(a, {}), "and the notice that follows repeats none of it");
    c.send("aethersdr_ext_enable:true;");
    check(expect(c, offeredState, "c"), "a client enabling later gets the same list and state");

    // Reads, to the requester only.
    a.send("aethersdr_rx_span:1;");
    check(expect(a, lines({"aethersdr_rx_span:1,96000;"})) && expect(c, {}, "c"),
          "a read of trx 1 is answered to the requester only");
    a.send("aethersdr_rx_span_rates;");
    check(expect(a, lines({"aethersdr_rx_span_rates:true,true,48000,96000,192000,384000;"})),
          "the rates read");
    a.send("aethersdr_rx_span:5;aethersdr_rx_span:5,192000;aethersdr_rx_span;"
           "aethersdr_rx_span:0,0;aethersdr_rx_span:0,abc;aethersdr_rx_span:0,1,2;"
           "aethersdr_rx_span_rates:1;");
    settle();
    check(a.take().isEmpty() && backend->requests.isEmpty(),
          "an unknown trx, a bare command and malformed input are ignored");

    // A set while the radio rebuilds: nothing until the radio reports.
    backend->answer = SpanBackend::Answer::Later;
    a.send("aethersdr_rx_span:1,192000;");
    check(backend->requests.size() == 1 && backend->requests.first().first == QStringLiteral("p1")
              && backend->requests.first().second == 192000.0,
          "a set on trx 1 asks the radio, through the model, for trx 1's pan");
    settle();
    f.republishCapabilities();  // an HL2 announces its receiver ceiling mid-change
    settle();
    check(expect(a, {}) && expect(c, {}, "c") && b.take().isEmpty(),
          "while the rate change is in flight, no client hears a span line");
    a.send("aethersdr_rx_span:1;");
    check(expect(a, lines({"aethersdr_rx_span:1,96000;"})),
          "and a read during it answers the span still in force");
    backend->finish();
    settle();
    const QStringList wide = lines({"aethersdr_rx_span:0,192000;", "aethersdr_rx_span:1,192000;"});
    check(expect(a, wide) && expect(c, wide, "c") && b.take().isEmpty(),
          "the reported change reaches every enabled client, for every trx it moved");
    check(f.model.panadapter(f.model.slice(0)->panId())->bandwidthMhz() == 0.192,
          "the model holds the span the radio reported");

    // Only a report after the set can answer it, and not one that only moved
    // the centre (a tune while an HL2 rebuilds).
    backend->answer = SpanBackend::Answer::Never;
    backend->centerMhz.insert(QStringLiteral("p1"), 14.2);
    backend->report(QStringLiteral("p1"));
    a.send("aethersdr_rx_span:1,96000;");
    settle();
    check(expect(a, {}) && expect(c, {}, "c"),
          "a centre report from just before a set does not answer it");
    backend->report(QStringLiteral("p1"));
    settle();
    check(expect(a, lines({"aethersdr_rx_span:1,192000;"})) && expect(c, lines({"aethersdr_rx_span:1,192000;"}), "c"),
          "a re-assert after the set, at the same centre, answers it with the span kept");
    backend->answer = SpanBackend::Answer::Later;
    a.send("aethersdr_rx_span:1,96000;");
    settle();
    backend->centerMhz.insert(QStringLiteral("p1"), 14.3);
    backend->report(QStringLiteral("p1"));
    settle();
    check(expect(a, {}) && expect(c, {}, "c"),
          "a centre move while the set is in flight does not answer it");
    backend->finish();
    settle();
    const QStringList narrow = lines({"aethersdr_rx_span:0,96000;", "aethersdr_rx_span:1,96000;"});
    check(expect(a, narrow) && expect(c, narrow, "c"), "the span the radio then reports does");
    backend->answer = SpanBackend::Answer::Now;
    a.send("aethersdr_rx_span:1,192000;");
    settle();
    check(expect(a, wide) && expect(c, wide, "c"), "and the span is back for what follows");

    // Snaps and quiet no-ops.
    backend->requests.clear();
    a.send("aethersdr_rx_span:0,150000;");
    settle();
    check(expect(a, lines({"aethersdr_rx_span:0,192000;"})) && expect(c, {}, "c")
              && backend->requests.isEmpty(),
          "a set that snaps to the span in force is answered to the sender only");
    a.send("aethersdr_rx_span:0,192000;");
    settle();
    check(expect(a, {}) && expect(c, {}, "c") && backend->requests.isEmpty(),
          "an exact set of the span in force stays quiet");
    a.send("aethersdr_rx_span:0,48001;");
    settle();
    check(backend->requests.size() == 1 && backend->requests.first().second == 48000.0
              && expect(a, lines({"aethersdr_rx_span:0,48000;", "aethersdr_rx_span:1,48000;"})),
          "a set is snapped to the nearest rate before it is asked for");
    c.take();

    // The radio's limits narrow the rates (an HL2 with more receivers open).
    f.limits(0.048, 0.096);
    settle();
    check(expect(a, lines({"aethersdr_rx_span_rates:true,true,48000,96000;"})),
          "narrowed limits re-send the rates a set can reach");
    c.take();
    backend->requests.clear();
    a.send("aethersdr_rx_span:1,384000;");
    settle();
    check(backend->requests.size() == 1 && backend->requests.first().second == 96000.0,
          "a set beyond them asks for the widest reachable rate");
    a.take();
    c.take();

    // The radio keeps its span (HL2 link budget): the set is still answered.
    backend->answer = SpanBackend::Answer::Keep;
    a.send("aethersdr_rx_span:1,48000;");
    settle();
    check(expect(a, lines({"aethersdr_rx_span:1,96000;"})) && expect(c, lines({"aethersdr_rx_span:1,96000;"}), "c"),
          "a set the radio refused is answered with the span it kept");
    backend->answer = SpanBackend::Answer::Never;
    a.send("aethersdr_rx_span:1,48000;");
    settle();
    check(expect(a, {}), "a set the radio has not answered yet waits");
    settle(2100);
    check(expect(a, lines({"aethersdr_rx_span:1,96000;"})),
          "and is answered with the span in force after two seconds");
    c.take();
    backend->answer = SpanBackend::Answer::Now;

    // Keyed: refused where a change rebuilds the receive chains.
    backend->requests.clear();
    f.key(true);
    check(f.model.transmitModel().isMox(), "MOX is reported");
    a.send("aethersdr_rx_span:0,48000;");
    settle();
    check(backend->requests.isEmpty() && expect(a, lines({"aethersdr_rx_span:0,96000;"}))
              && expect(c, {}, "c"),
          "while keyed, a set is refused and answered to the sender with the span in force");
    f.key(false);
    settle();
    a.send("aethersdr_rx_span:0,48000;");
    settle();
    check(backend->requests.size() == 1, "unkeyed, the same set is asked for");
    a.take();
    c.take();

    // A desktop change, from any source, goes to enabled clients only.
    backend->apply(QStringLiteral("p0"), 96000);
    settle();
    const QStringList desk = lines({"aethersdr_rx_span:0,96000;", "aethersdr_rx_span:1,96000;"});
    check(expect(a, desk) && expect(c, desk, "c") && b.take().isEmpty(),
          "a span change from the desktop is notified to enabled clients");

    // Plain and disabled clients' commands are dropped.
    backend->requests.clear();
    b.send("aethersdr_rx_span:0,48000;aethersdr_rx_span:0;aethersdr_rx_span_rates;");
    c.send("aethersdr_ext_enable:false;");
    c.send("aethersdr_rx_span:0,48000;aethersdr_rx_span:0;");
    settle();
    check(backend->requests.isEmpty() && expect(a, {}) && b.take().isEmpty() && expect(c, {}, "c"),
          "a plain or disabled client's command is dropped and writes nothing");
    backend->apply(QStringLiteral("p0"), 48000);
    settle();
    check(expect(a, lines({"aethersdr_rx_span:0,48000;", "aethersdr_rx_span:1,48000;"}))
              && expect(c, {}, "c"),
          "a disabled client hears no notice");

    // Lost capability: the item leaves the list and answers nothing.
    backend->declareSpan(false);
    f.republishCapabilities();
    check(expect(a, lines({"aethersdr_ext:1;"})), "a lost capability re-sends the list without it");
    backend->apply(QStringLiteral("p0"), 96000);
    a.send("aethersdr_rx_span:0;");
    settle();
    check(expect(a, {}), "and the item neither notifies nor answers");
    check(extensionLines(b.all).isEmpty(), "the plain client received no aethersdr_ byte at all");
}

// A Flex's shape: a continuous span per pan. Lines follow receivers as they
// appear and change pan, not only span changes.
void continuousPerPan()
{
    Fixture f(false, false, 0.2, 0.1);
    SpanBackend* backend = f.backend;
    f.limits(0.005, 0.5);
    Wire a;
    Wire b;
    TciSpanExtensionTest::attach(*f.server, a.client, f.model);
    TciSpanExtensionTest::attach(*f.server, b.client, f.model);
    settle(600);
    a.take();
    a.send("aethersdr_ext_enable:true;");
    check(expect(a, lines({"aethersdr_ext:1,rx_span;", "aethersdr_rx_span_rates:false,false;",
                           "aethersdr_rx_span:0,200000;", "aethersdr_rx_span:1,100000;"})),
          "a continuous span lists no rates and each trx's own span");

    a.send("aethersdr_rx_span:1,1000000;");
    settle();
    check(!backend->requests.isEmpty() && backend->requests.last().second == 500000.0
              && expect(a, lines({"aethersdr_rx_span:1,500000;"})),
          "a set on trx 1 is clamped to its pan's limits and changes only that pan");
    f.key(true);
    a.send("aethersdr_rx_span:1,150000;");
    settle();
    check(backend->requests.last().second == 150000.0 && expect(a, lines({"aethersdr_rx_span:1,150000;"})),
          "where span is not the rate, a set is allowed while keyed");
    f.key(false);

    f.moveSlice(1, "p0");
    settle();
    check(expect(a, lines({"aethersdr_rx_span:1,200000;"})),
          "a receiver moved to another pan is sent its new span, unasked");
    f.addSlice(2, "p1", 14.2);
    settle(600);
    check(expect(a, lines({"aethersdr_rx_span:2,150000;"})),
          "a new receiver is sent its span, unasked");
    backend->apply(QStringLiteral("p0"), 300000);
    settle();
    check(expect(a, lines({"aethersdr_rx_span:0,300000;", "aethersdr_rx_span:1,300000;"})),
          "a pan's change is sent for every trx on it, and only those");
    a.send("aethersdr_rx_span:2,10;");
    settle();
    check(backend->requests.last().second == 5000.0 && expect(a, lines({"aethersdr_rx_span:2,5000;"})),
          "a set below the limit is clamped to it");
    check(extensionLines(b.all).isEmpty(), "the plain client received no aethersdr_ byte at all");
}

// Transmit ownership taken, MOX not reported yet (a key-up in progress): a set
// that would rebuild the receive chains is refused as keyed.
void ownershipBeforeMox()
{
    Fixture f(true, true, 0.096, 0.096);
    Wire a;
    TciSpanExtensionTest::attach(*f.server, a.client, f.model);
    settle(600);
    a.take();
    a.send("aethersdr_ext_enable:true;");
    a.take();
    TxCoordinator& coordinator = TciSpanExtensionTest::coordinator(f.model);
    const TxCoordinator::Actor actor = coordinator.registerActor({true, 0});
    const TxCoordinator::Admission held = coordinator.acquire(actor, TxCoordinator::monotonicMs());
    check(held.accepted() && f.model.transmitOwnershipPending() && !f.model.transmitModel().isMox()
              && !f.model.transmitModel().isTransmitting() && !f.model.isRadioTransmitting(),
          "a TxCoordinator operation is held and no MOX is reported");
    a.send("aethersdr_rx_span:0,192000;");
    settle();
    check(f.backend->requests.isEmpty() && expect(a, lines({"aethersdr_rx_span:0,96000;"})),
          "while transmit ownership is held, a set is refused and answered with the span in force");
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-tci-ext-span-test"));
    DaxIqModel::setPactlRunnerForTest([](const QStringList&) { return quint32{0}; });
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    check(settingsProfile.isValid(), "isolated settings profile");
    offeredPerBackend();
    grammar();
    rateRadioWide();
    continuousPerPan();
    ownershipBeforeMox();

    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
