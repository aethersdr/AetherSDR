// The antenna groups of the opt-in TCI extension set (`aethersdr_rx_antenna`,
// `aethersdr_tx_antenna` and their lists, RFC #7181): offered while a receiver
// publishes a list (TX: on a radio that can transmit), set through
// SliceModel::setRxAntenna()/setTxAntenna() like the RX applet's menus, the TX
// antenna refused while transmitting, and every change, lists included, sent
// to enabled clients only; an RX set asks first for a KiwiSDR antenna to be
// released, and a keyed refusal warns once per keyed period, re-armed also
// when a TCI key ends on its own; a list change reaches every enabled client
// even when only some of them were behind on the item list; a TX set is
// accepted again after an over whose stop the radio never confirms, once fresh
// RX is seen. Socket-free; the over keys only this test's backend.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/TciAntennaExtension.h"
#include "core/TciClient.h"
#include "core/TciExtensionSet.h"
#include "core/TciProtocol.h"
#include "core/TciServer.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/ModelControlRequest.h"
#include "core/backends/RadioCapabilities.h"
#include "core/backends/SliceDelta.h"
#include "core/backends/TransmitDelta.h"
#include "models/DaxIqModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <cstdio>
#include <functional>
#include <memory>
#include <variant>

namespace AetherSDR {

class TciAntennaExtensionTest {
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
    // A TCI key accepted, before MOX is visible on the model.
    static void holdTciPtt(TciServer& server, TciClient* client)
    {
        server.m_tciPttClient = client;
    }
    // The same, with the key-up requested and not yet confirmed, so the
    // client's own trx:false releases it (TciServer::abortTciPtt).
    static void requestTciPtt(TciServer& server, TciClient* client)
    {
        server.m_tciPttClient = client;
        server.m_tciPttRequestedOn = true;
        server.m_tciPttConfirmedOn = false;
    }
    static bool tciPttHeld(const TciServer& server) { return !server.m_tciPttClient.isNull(); }
};

namespace {

// Connected; records the antenna requests the model path hands it. `onTxAntenna`
// lets a case answer the way a backend that refuses does (republish the actual).
class AntennaBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    QList<QPair<int, QString>> rxRequests;
    QList<QPair<int, QString>> txRequests;
    std::function<void(int, const QString&)> onTxAntenna;
    bool connected{true};

    AntennaBackend() { caps.canTransmit = true; }
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connected; }
    ReceiveDispatch requestSliceRxAntenna(int sliceId, const QString& antenna) override
    {
        rxRequests.append({sliceId, antenna});
        return ReceiveDispatch::LocalOnly;
    }
    ReceiveDispatch requestSliceControl(int sliceId, const SliceControlRequest& request) override
    {
        if (request.field == SliceControlRequest::Field::TxAntenna) {
            const QString antenna = std::get<QString>(request.value);
            txRequests.append({sliceId, antenna});
            if (onTxAntenna) {
                onTxAntenna(sliceId, antenna);
            }
            return ReceiveDispatch::Dispatched;
        }
        return IRadioBackend::requestSliceControl(sliceId, request);
    }
    void setTxSlice(int) override {}
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    // Each key edge; with holdKeyUp, the key-up's delivery is kept unfinished.
    QList<bool> keyEdges;
    bool holdKeyUp{false};
    TxCoordinator::Completion heldKeyUp;
    void setKeying(bool on, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion& completion) override
    {
        keyEdges.append(on);
        if (!on && holdKeyUp) {
            heldKeyUp = completion;
        }
    }
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

// The server guarantees which lines a client gets, not the order across
// receivers and groups, so most comparisons are of the set of lines.
QSet<QString> lineSet(const QStringList& lines)
{
    return QSet<QString>(lines.cbegin(), lines.cend());
}

QSet<QString> lines(std::initializer_list<const char*> texts)
{
    QSet<QString> out;
    for (const char* text : texts) {
        out.insert(QString::fromLatin1(text));
    }
    return out;
}

void dump(const char* who, const QStringList& got)
{
    for (const QString& line : got) {
        std::printf("      %s< %s\n", who, qPrintable(line));
    }
}

struct Wire {
    QPointer<TciClient> client{new TciClient};
    QStringList rx;
    QStringList all;  // never taken
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
    QStringList take()
    {
        QStringList out = rx;
        rx.clear();
        return out;
    }
    QStringList takeExtension() { return extensionLines(take()); }
};

QString lastAdvertisement(const QStringList& got)
{
    QString last;
    for (const QString& line : got) {
        if (line.startsWith(QLatin1String("aethersdr_ext:"))) {
            last = line;
        }
    }
    return last;
}

void wireGrammar()
{
    using namespace TciAntennaExtension;
    check(wireNames({QStringLiteral(" ANT1 "), QStringLiteral("ANT2"), QStringLiteral("ANT1"),
                     QString(), QStringLiteral("a,b"), QStringLiteral("c:d"),
                     QStringLiteral("e;f"), QStringLiteral("RX_A")})
              == QStringList({QStringLiteral("ANT1"), QStringLiteral("ANT2"),
                              QStringLiteral("RX_A")}),
          "a list is trimmed, deduplicated, and loses names with a reserved character");
    const QStringList wire{QStringLiteral("ANT"), QStringLiteral("RF1")};
    check(listedName(wire, QStringLiteral("rf1")) == QStringLiteral("RF1")
              && listedName(wire, QStringLiteral("ANT2")).isEmpty(),
          "a set names a listed antenna in any case, and gets the listed spelling");
    const auto parse = [](const char* args, bool allowSet) {
        return TciAntennaExtension::parse(
            QString::fromLatin1(args).split(QLatin1Char(','), Qt::KeepEmptyParts), allowSet);
    };
    check(parse("0", true) && !parse("0", true)->isSet && parse("1,ANT", true)->isSet
              && parse("1,ANT", true)->name == QStringLiteral("ANT")
              && !parse("1,ANT", false) && !parse("-1", true) && !parse("x", true)
              && !parse("0,", true) && !parse("0,A,B", true) && !parse("", true),
          "only <trx> and, for a selection, <trx>,<name> parse");
    check(listLine(kTxListCommand, 1, wire) == QStringLiteral("aethersdr_tx_antennas:1,ANT,RF1;")
              && selectionLine(kRxCommand, 0, QStringLiteral("ANT"))
                     == QStringLiteral("aethersdr_rx_antenna:0,ANT;"),
          "the list and selection lines");
}

// The TX gate reads canTransmit, as the RX applet's TX menu does; the real
// backends as the production family switch builds them.
void transmitGatePerBackend()
{
    const struct {
        const char* family;
        bool expected;
    } cases[] = {{"hl2", true}, {"flex", true}};
    for (const auto& c : cases) {
        RadioModel model;
        const bool built = model.rebuildBackendForTest(QString::fromLatin1(c.family))
            && model.backend();
        const bool offered = built
            && TciAntennaExtension::txOffered(true, model.backend()->capabilities().canTransmit,
                                              true);
        std::printf("      %s: built=%d tx offered=%d\n", c.family, built, offered);
        check(built && offered == c.expected, "a radio that transmits is offered the TX antenna");
    }
    RadioModel rtl;
    if (rtl.rebuildBackendForTest(QStringLiteral("rtl")) && rtl.backend()) {
        check(!TciAntennaExtension::txOffered(true, rtl.backend()->capabilities().canTransmit,
                                              true),
              "RTL-SDR (receive only) is not offered the TX antenna");
    } else {
        std::printf("      rtl: not built into this tree, not checked\n");
    }
    check(!TciAntennaExtension::rxOffered(false, true) && !TciAntennaExtension::rxOffered(true, false)
              && !TciAntennaExtension::txOffered(true, false, true),
          "no radio, no list or no transmitter: not offered");
}

SliceDelta antennas(const QStringList& rxList, const QString& rx, const QStringList& txList,
                    const QString& tx)
{
    SliceDelta d;
    d.rxAntennaList = rxList;
    d.rxAntenna = rx;
    d.txAntennaList = txList;
    d.txAntenna = tx;
    return d;
}

void serverBehaviour()
{
    RadioModel model;
    auto owner = std::make_unique<AntennaBackend>();
    AntennaBackend* backend = owner.get();
    backend->connected = false;  // slice fixtures are applied while disconnected
    model.setBackendForTest(std::move(owner), QStringLiteral("test"));
    QString error;
    const bool fixtures = model.automationApplySliceFixture(0, QStringLiteral("A"), &error)
        && model.automationApplySliceFixture(1, QStringLiteral("B"), &error);
    backend->connected = true;
    SliceModel* s0 = model.slice(0);
    SliceModel* s1 = model.slice(1);
    check(fixtures && s0 && s1 && model.isConnected(), "two fixture slices on a connected radio");
    if (!fixtures || !s0 || !s1) {
        std::printf("      fixture: %s\n", qPrintable(error));
        return;
    }
    // No list yet on either slice.
    s0->applyChanges(antennas({}, QStringLiteral("ANT1"), {}, QStringLiteral("ANT1")));
    s1->applyChanges(antennas({}, QStringLiteral("ANT1"), {}, QStringLiteral("ANT1")));
    int warnings = 0;
    QObject::connect(&model, &RadioModel::configurationWarning, [&warnings](const QString& why) {
        ++warnings;
        std::printf("      notice: %s\n", qPrintable(why));
    });

    TciServer server(&model);
    server.wireSlice(s0->sliceId(), s0);
    server.wireSlice(s1->sliceId(), s1);
    Wire a;  // enabled
    Wire b;  // plain
    Wire c;  // enabled later
    TciAntennaExtensionTest::attach(server, a.client, model);
    TciAntennaExtensionTest::attach(server, b.client, model);
    TciAntennaExtensionTest::attach(server, c.client, model);
    // What MainWindow's KiwiSDR clear would see: the slice, and whether the
    // set had already reached the slice or the backend when it ran.
    struct KiwiRelease {
        int sliceId;
        QString rxAntenna;
        int rxRequests;
    };
    QList<KiwiRelease> kiwiReleases;
    QObject::connect(&server, &TciServer::rxAntennaSetByTci, [&](int sliceId) {
        SliceModel* slice = model.slice(sliceId);
        kiwiReleases.append({sliceId, slice ? slice->rxAntenna() : QString(),
                             static_cast<int>(backend->rxRequests.size())});
    });
    settle(600);  // past wireSlice()'s 400 ms seeds
    a.take();
    b.take();
    c.take();

    // Gate: no receiver publishes a list, so nothing is offered.
    check(TciAntennaExtensionTest::offeredTokens(server).isEmpty(),
          "a radio with no antenna list is offered neither item");
    a.send("aethersdr_ext_enable:true;");
    check(a.take() == QStringList({QStringLiteral("aethersdr_ext:1;")}),
          "its enable reply lists no item");
    a.send("aethersdr_rx_antennas:0;aethersdr_rx_antenna:0,ANT2;aethersdr_tx_antenna:0,ANT2;");
    settle();
    check(a.takeExtension().isEmpty() && backend->rxRequests.isEmpty()
              && backend->txRequests.isEmpty() && s0->rxAntenna() == QStringLiteral("ANT1"),
          "a read or set for an item not offered gets no reply and writes nothing");

    // Trx 0 publishes its lists, with no capabilitiesChanged (as Flex and the
    // HL2 do): the item list is re-sent with the state.
    s0->applyChanges(antennas({QStringLiteral("ANT1"), QStringLiteral("ANT2"),
                               QStringLiteral("RX_A"), QStringLiteral("bad,name")},
                              QStringLiteral("ANT1"),
                              {QStringLiteral("ANT1"), QStringLiteral("ANT2")},
                              QStringLiteral("ANT1")));
    settle();
    QStringList got = a.takeExtension();
    check(lastAdvertisement(got) == QStringLiteral("aethersdr_ext:1,rx_antenna,tx_antenna;"),
          "a list appearing re-sends the item list, without a capability change");
    check(lineSet(got)
              == lines({"aethersdr_ext:1,rx_antenna;", "aethersdr_ext:1,rx_antenna,tx_antenna;",
                        "aethersdr_rx_antennas:0,ANT1,ANT2,RX_A;", "aethersdr_rx_antenna:0,ANT1;",
                        "aethersdr_tx_antennas:0,ANT1,ANT2;", "aethersdr_tx_antenna:0,ANT1;"}),
          "with the lines for trx 0 only; the name with a comma is left out");
    if (lastAdvertisement(got) != QStringLiteral("aethersdr_ext:1,rx_antenna,tx_antenna;")) {
        dump("a", got);
    }

    // Trx 1 publishes: the gate does not move, so only its lines go out.
    s1->applyChanges(antennas({QStringLiteral("ANT1"), QStringLiteral("ANT2")},
                              QStringLiteral("ANT2"),
                              {QStringLiteral("ANT1"), QStringLiteral("ANT2")},
                              QStringLiteral("ANT1")));
    settle();
    got = a.takeExtension();
    check(lineSet(got)
              == lines({"aethersdr_rx_antennas:1,ANT1,ANT2;", "aethersdr_rx_antenna:1,ANT2;",
                        "aethersdr_tx_antennas:1,ANT1,ANT2;", "aethersdr_tx_antenna:1,ANT1;"}),
          "a second receiver's lists are sent for trx 1, with no new item list");
    if (got.size() != 4) {
        dump("a", got);
    }

    c.send("aethersdr_ext_enable:true;");
    got = c.take();
    check(!got.isEmpty() && got.first() == QStringLiteral("aethersdr_ext:1,rx_antenna,tx_antenna;")
              && lineSet(got.mid(1))
                     == lines({"aethersdr_rx_antennas:0,ANT1,ANT2,RX_A;",
                               "aethersdr_rx_antenna:0,ANT1;",
                               "aethersdr_rx_antennas:1,ANT1,ANT2;", "aethersdr_rx_antenna:1,ANT2;",
                               "aethersdr_tx_antennas:0,ANT1,ANT2;", "aethersdr_tx_antenna:0,ANT1;",
                               "aethersdr_tx_antennas:1,ANT1,ANT2;",
                               "aethersdr_tx_antenna:1,ANT1;"})
              && got.size() == 9,
          "a client enabling later gets the item list, then every receiver's state once");
    check(extensionLines(b.take()).isEmpty(), "the plain client heard none of it");

    // Reads go to the requester only; malformed and unknown trx are ignored.
    a.send("aethersdr_rx_antennas:0;aethersdr_tx_antenna:1;");
    check(a.take() == QStringList({QStringLiteral("aethersdr_rx_antennas:0,ANT1,ANT2,RX_A;"),
                                   QStringLiteral("aethersdr_tx_antenna:1,ANT1;")})
              && c.take().isEmpty(),
          "a read is answered to the requester only");
    a.send("aethersdr_rx_antennas:5;aethersdr_rx_antenna:5,ANT1;aethersdr_rx_antennas:0,ANT1;"
           "aethersdr_rx_antenna;aethersdr_rx_antenna:x,ANT1;aethersdr_tx_antenna:0,ANT9;"
           "aethersdr_rx_antenna:0,bad,name;");
    settle();
    check(a.takeExtension().isEmpty() && backend->rxRequests.isEmpty()
              && backend->txRequests.isEmpty(),
          "an unknown trx, a malformed line or an unlisted name: no reply, nothing written");
    check(kiwiReleases.isEmpty(), "and no KiwiSDR release for any of them");
    // A set naming the antenna already in use is answered with the selection
    // line, to the sender only, and writes nothing (ruling on #7197's N3).
    a.send("aethersdr_tx_antenna:0,ant1;");
    settle();
    check(a.takeExtension() == QStringList({QStringLiteral("aethersdr_tx_antenna:0,ANT1;")})
              && c.takeExtension().isEmpty() && backend->txRequests.isEmpty() && warnings == 0,
          "a TX set naming the antenna in use is answered to the sender, nothing written");

    // RX set: through SliceModel to the backend, then the notice to enabled clients.
    a.send("aethersdr_rx_antenna:0,rx_a;");
    settle();
    check(s0->rxAntenna() == QStringLiteral("RX_A") && backend->rxRequests.size() == 1
              && backend->rxRequests.first() == qMakePair(0, QStringLiteral("RX_A")),
          "an RX set reaches the backend as the menu's receive-antenna request, listed spelling");
    check(kiwiReleases.size() == 1 && kiwiReleases.first().sliceId == 0
              && kiwiReleases.first().rxAntenna == QStringLiteral("ANT1")
              && kiwiReleases.first().rxRequests == 0,
          "it first asks for the slice's KiwiSDR antenna to be released, before the set");
    got = a.takeExtension();
    check(got == QStringList({QStringLiteral("aethersdr_rx_antenna:0,RX_A;")})
              && c.takeExtension() == got && extensionLines(b.take()).isEmpty(),
          "it is notified once to each enabled client, not to the plain one");
    // The port in use: a KiwiSDR antenna leaves rxAntenna() on the real port,
    // so naming it must still release the Kiwi, and write nothing else.
    a.send("aethersdr_rx_antenna:0,RX_A;");
    settle();
    check(kiwiReleases.size() == 2 && kiwiReleases.last().sliceId == 0
              && backend->rxRequests.size() == 1
              && a.takeExtension() == QStringList({QStringLiteral("aethersdr_rx_antenna:0,RX_A;")})
              && c.takeExtension().isEmpty(),
          "an RX set naming the antenna in use still releases a KiwiSDR antenna, writes nothing "
          "else, and is answered to the sender");

    // The RX antenna may change while transmitting, as in the GUI.
    model.transmitModel().setTransmitting(true);
    a.send("aethersdr_rx_antenna:1,ANT1;");
    settle();
    check(s1->rxAntenna() == QStringLiteral("ANT1") && backend->rxRequests.size() == 2
              && a.takeExtension() == QStringList({QStringLiteral("aethersdr_rx_antenna:1,ANT1;")}),
          "an RX set is allowed while transmitting");
    c.take();

    // TX set while transmitting: refused before dispatch, the antenna in use
    // answered to the sender only, and the reason shown in the app.
    a.send("aethersdr_tx_antenna:0,ANT2;");
    settle();
    check(backend->txRequests.isEmpty() && s0->txAntenna() == QStringLiteral("ANT1")
              && a.take() == QStringList({QStringLiteral("aethersdr_tx_antenna:0,ANT1;")})
              && c.take().isEmpty() && warnings == 1,
          "a TX set while transmitting is refused, answered to the sender, and noticed");
    a.send("aethersdr_tx_antenna:0,ANT2;aethersdr_tx_antenna:0,ANT2;");
    settle();
    check(backend->txRequests.isEmpty()
              && a.take() == QStringList({QStringLiteral("aethersdr_tx_antenna:0,ANT1;"),
                                          QStringLiteral("aethersdr_tx_antenna:0,ANT1;")})
              && c.take().isEmpty() && warnings == 1,
          "repeated refusals in the same keyed period are each answered, and noticed once");
    a.send("aethersdr_tx_antenna:0,ANT1;");
    settle();
    check(backend->txRequests.isEmpty()
              && a.take() == QStringList({QStringLiteral("aethersdr_tx_antenna:0,ANT1;")})
              && c.take().isEmpty() && warnings == 1,
          "a keyed TX set naming the antenna in use is answered, and is no refusal");
    model.transmitModel().setTransmitting(false);
    TransmitDelta tune;
    tune.tune = true;
    model.transmitModel().applyChanges(tune);
    a.send("aethersdr_tx_antenna:0,ANT2;");
    settle();
    check(model.transmitModel().isTuning() && backend->txRequests.isEmpty()
              && a.takeExtension() == QStringList({QStringLiteral("aethersdr_tx_antenna:0,ANT1;")})
              && warnings == 2,
          "and while tuning: unkeying re-arms the notice");
    tune.tune = false;
    model.transmitModel().applyChanges(tune);

    // A TCI key held before MOX is visible is keyed too.
    TciAntennaExtensionTest::holdTciPtt(server, b.client);
    a.send("aethersdr_tx_antenna:0,ANT2;");
    settle();
    check(!model.transmitModel().isMox() && backend->txRequests.isEmpty()
              && a.takeExtension() == QStringList({QStringLiteral("aethersdr_tx_antenna:0,ANT1;")})
              && warnings == 3,
          "a held TCI PTT before MOX refuses a TX set");
    TciAntennaExtensionTest::holdTciPtt(server, nullptr);
    b.take();
    c.take();

    // A TCI key that ends on its own, with no TransmitModel or radio edge
    // after it, ends the keyed period too: the next one warns again.
    TciAntennaExtensionTest::requestTciPtt(server, b.client);
    a.send("aethersdr_tx_antenna:0,ANT2;");
    settle();
    check(a.takeExtension() == QStringList({QStringLiteral("aethersdr_tx_antenna:0,ANT1;")})
              && warnings == 3,
          "a TX set refused under the same TCI key is not noticed again");
    b.send("trx:0,false;");
    settle();
    check(!TciAntennaExtensionTest::tciPttHeld(server), "the TCI client releases its key");
    model.transmitModel().setTransmitting(true);
    a.send("aethersdr_tx_antenna:0,ANT2;");
    settle();
    check(backend->txRequests.isEmpty()
              && a.takeExtension() == QStringList({QStringLiteral("aethersdr_tx_antenna:0,ANT1;")})
              && warnings == 4,
          "a TCI key ending on its own re-arms the notice for the next keyed period");
    model.transmitModel().setTransmitting(false);
    a.take();
    b.take();
    c.take();

    // TX set, not keyed: the menu's path to the backend.
    a.send("aethersdr_tx_antenna:1,ant2;");
    settle();
    check(backend->txRequests.size() == 1
              && backend->txRequests.first() == qMakePair(1, QStringLiteral("ANT2"))
              && s1->txAntenna() == QStringLiteral("ANT2"),
          "a TX set reaches the backend as the menu's TX antenna control request");
    check(a.takeExtension() == QStringList({QStringLiteral("aethersdr_tx_antenna:1,ANT2;")})
              && c.takeExtension() == QStringList({QStringLiteral("aethersdr_tx_antenna:1,ANT2;")})
              && extensionLines(b.take()).isEmpty(),
          "and is notified to enabled clients");

    // A backend that refuses republishes the antenna in use; that is the answer.
    backend->onTxAntenna = [s0](int, const QString&) {
        SliceDelta d;
        d.txAntenna = QStringLiteral("ANT1");
        s0->applyChanges(d);
    };
    a.send("aethersdr_tx_antenna:0,ANT2;");
    settle();
    got = a.takeExtension();
    check(backend->txRequests.size() == 2 && s0->txAntenna() == QStringLiteral("ANT1")
              && got == QStringList({QStringLiteral("aethersdr_tx_antenna:0,ANT1;")}),
          "a backend refusal reads back the antenna actually in use");
    if (got != QStringList({QStringLiteral("aethersdr_tx_antenna:0,ANT1;")})) {
        dump("a", got);
    }
    backend->onTxAntenna = {};
    c.take();

    // A desktop change and a list change reach enabled clients, and not the
    // plain one. The list moves without capabilitiesChanged.
    s0->setRxAntenna(QStringLiteral("ANT2"));
    SliceDelta shorter;
    shorter.txAntennaList = QStringList{QStringLiteral("ANT1")};
    s1->applyChanges(shorter);
    settle();
    check(lineSet(a.takeExtension())
              == lines({"aethersdr_rx_antenna:0,ANT2;", "aethersdr_tx_antennas:1,ANT1;"})
              && lineSet(c.takeExtension())
                     == lines({"aethersdr_rx_antenna:0,ANT2;", "aethersdr_tx_antennas:1,ANT1;"})
              && extensionLines(b.take()).isEmpty(),
          "a desktop selection and a shorter list are notified, with no item list");

    // A plain or disabled client's command is dropped.
    const int rxBefore = backend->rxRequests.size();
    const int txBefore = backend->txRequests.size();
    const int releasesBefore = kiwiReleases.size();
    b.send("aethersdr_rx_antenna:0,ANT1;aethersdr_tx_antenna:0,ANT2;aethersdr_rx_antennas:0;");
    c.send("aethersdr_ext_enable:false;");
    c.send("aethersdr_rx_antenna:0,ANT1;aethersdr_tx_antenna:0,ANT2;aethersdr_rx_antennas:0;");
    settle();
    check(backend->rxRequests.size() == rxBefore && backend->txRequests.size() == txBefore
              && a.takeExtension().isEmpty() && b.take().isEmpty() && c.take().isEmpty(),
          "a plain or disabled client's command is dropped and writes nothing");
    check(kiwiReleases.size() == releasesBefore, "and releases no KiwiSDR antenna");
    s1->setRxAntenna(QStringLiteral("ANT2"));
    settle();
    check(a.takeExtension() == QStringList({QStringLiteral("aethersdr_rx_antenna:1,ANT2;")})
              && c.take().isEmpty(),
          "a disabled client hears no notice");

    // TX moves to trx 1: its TX antenna line is re-sent unasked, as the
    // antenna that will now transmit, and the plain client hears nothing.
    SliceDelta txOn;
    txOn.txSlice = true;
    SliceDelta txOff;
    txOff.txSlice = false;
    s0->applyChanges(txOn);
    settle(600);  // past kSplitSettleMs
    a.take();
    b.take();
    s1->applyChanges(txOn);
    s0->applyChanges(txOff);
    settle(600);
    got = a.takeExtension();
    check(got == QStringList({QStringLiteral("aethersdr_tx_antenna:1,ANT2;")})
              && extensionLines(b.take()).isEmpty(),
          "moving TX to trx 1 re-sends trx 1's TX antenna to the enabled client, unasked");
    if (got != QStringList({QStringLiteral("aethersdr_tx_antenna:1,ANT2;")})) {
        dump("a", got);
    }

    // The radio stops transmitting capability: tx_antenna leaves the list.
    backend->caps.canTransmit = false;
    emit model.capabilitiesChanged(true, backend->caps);
    got = a.takeExtension();
    check(!got.isEmpty() && got.first() == QStringLiteral("aethersdr_ext:1,rx_antenna;")
              && got.filter(QStringLiteral("aethersdr_tx_")).isEmpty(),
          "a radio that cannot transmit re-sends the list without tx_antenna");
    if (got.isEmpty() || got.first() != QStringLiteral("aethersdr_ext:1,rx_antenna;")) {
        dump("a", got);
    }
    a.send("aethersdr_tx_antennas:0;aethersdr_tx_antenna:0,ANT2;");
    settle();
    check(a.takeExtension().isEmpty() && backend->txRequests.size() == txBefore,
          "and the TX item neither answers nor writes");

    // Every list empties, again without capabilitiesChanged: rx_antenna leaves.
    SliceDelta none;
    none.rxAntennaList = QStringList{};
    s0->applyChanges(none);
    s1->applyChanges(none);
    settle();
    check(lastAdvertisement(a.take()) == QStringLiteral("aethersdr_ext:1;"),
          "the last list emptying re-sends the item list without rx_antenna");
    check(!b.all.isEmpty() && extensionLines(b.all).isEmpty(),
          "the plain client received no aethersdr_ line at any point");
}

// A Flex keeps a finished over's TX ownership until qualified stop evidence,
// which may never come (NF0T on #7197, FLEX-8400): every TX set after an over
// was refused. Desktop MOX keys and unkeys here, interlock status as a Flex
// sends it; the over stays unconfirmed throughout, as there.
void txSetAfterAnOver()
{
    RadioModel model;
    auto owner = std::make_unique<AntennaBackend>();
    AntennaBackend* backend = owner.get();
    backend->connected = false;
    model.setBackendForTest(std::move(owner), QStringLiteral("test"));
    QString error;
    const bool fixture = model.automationApplySliceFixture(0, QStringLiteral("A"), &error);
    backend->connected = true;
    SliceModel* s0 = model.slice(0);
    check(fixture && s0, "over: one fixture slice");
    if (!fixture || !s0) {
        std::printf("      fixture: %s\n", qPrintable(error));
        return;
    }
    SliceDelta tx = antennas({QStringLiteral("ANT1"), QStringLiteral("ANT2")}, QStringLiteral("ANT1"),
                             {QStringLiteral("ANT1"), QStringLiteral("ANT2")}, QStringLiteral("ANT1"));
    tx.txSlice = true;
    tx.mode = QStringLiteral("USB");
    s0->applyChanges(tx);
    TciServer server(&model);
    server.wireSlice(s0->sliceId(), s0);
    Wire a;
    TciAntennaExtensionTest::attach(server, a.client, model);
    int warnings = 0;
    QObject::connect(&model, &RadioModel::configurationWarning,
                     [&warnings](const QString&) { ++warnings; });
    settle(600);
    a.take();
    a.send("aethersdr_ext_enable:true;");
    a.take();
    const auto interlock = [&model](const char* state) {
        model.handleStatusForTest(QStringLiteral("interlock"),
                                  {{QStringLiteral("state"), QString::fromLatin1(state)}});
    };
    const auto setTo = [&a](const char* antenna) {
        a.send(QStringLiteral("aethersdr_tx_antenna:0,%1;").arg(QLatin1String(antenna)).toLatin1().constData());
        settle();
        return a.takeExtension();
    };
    const QStringList stays1{QStringLiteral("aethersdr_tx_antenna:0,ANT1;")};

    interlock("READY");
    model.setTransmit(true);
    interlock("TRANSMITTING");
    check(backend->keyEdges == QList<bool>({true}) && model.transmitModel().isTransmitting(),
          "over: desktop MOX keys the backend");
    check(setTo("ANT2") == stays1 && backend->txRequests.isEmpty() && warnings == 1,
          "over: a TX set while keyed is refused, and noticed");
    model.setTransmit(false);
    interlock("UNKEY_REQUESTED");
    check(backend->keyEdges == QList<bool>({true, false}) && model.transmitOwnershipPending()
              && !model.transmitModel().isTransmitting() && !model.isRadioTransmitting(),
          "over: after the key-up the over's TX ownership is retained, unconfirmed");
    check(setTo("ANT2") == stays1 && backend->txRequests.isEmpty() && warnings == 1,
          "over: a TX set before fresh settled RX is refused, in the same keyed period");
    interlock("READY");
    const QStringList moved = setTo("ANT2");
    check(model.transmitOwnershipPending() && backend->txRequests.size() == 1
              && backend->txRequests.first() == qMakePair(0, QStringLiteral("ANT2"))
              && s0->txAntenna() == QStringLiteral("ANT2")
              && moved == QStringList({QStringLiteral("aethersdr_tx_antenna:0,ANT2;")}),
          "over: with fresh RX after the over, a TX set is accepted; ownership is still retained");
    if (backend->txRequests.size() != 1) {
        dump("a", moved);
    }

    // The key-up still being delivered is not idle, fresh RX or not.
    backend->holdKeyUp = true;
    model.setTransmit(true);
    interlock("TRANSMITTING");
    check(setTo("ANT1").size() == 1 && backend->txRequests.size() == 1 && warnings == 2,
          "over: the next over refuses a TX set again, and the notice re-armed on settled RX");
    model.setTransmit(false);
    interlock("READY");
    check(setTo("ANT1").size() == 1 && backend->txRequests.size() == 1 && warnings == 2,
          "over: a TX set while the key-up is still being delivered is refused");
    backend->holdKeyUp = false;
    backend->heldKeyUp.finish();
    backend->heldKeyUp = {};
    settle();

    // The delivery finishing is the edge that ends that keyed period: with no
    // accepted set in between, the next over's refusal is noticed again.
    model.setTransmit(true);
    interlock("TRANSMITTING");
    check(setTo("ANT1").size() == 1 && backend->txRequests.size() == 1 && warnings == 3,
          "over: after a delayed key-up delivery, the next over's refusal is noticed again");
    model.setTransmit(false);
    interlock("READY");
    setTo("ANT1");
    check(model.transmitOwnershipPending() && backend->txRequests.size() == 2
              && s0->txAntenna() == QStringLiteral("ANT1"),
          "over: once it is delivered, the TX set is accepted");
}

// A backend with no interlock status (the HL2): the over's TX ownership is
// still never confirmed, and the backend's own RX edge, once the key-up is
// delivered, is the settled RX that lets a TX set through again.
void txSetAfterAnOverWithoutInterlock()
{
    RadioModel model;
    auto owner = std::make_unique<AntennaBackend>();
    AntennaBackend* backend = owner.get();
    backend->connected = false;
    model.setBackendForTest(std::move(owner), QStringLiteral("test"));
    QString error;
    const bool fixture = model.automationApplySliceFixture(0, QStringLiteral("A"), &error);
    backend->connected = true;
    SliceModel* s0 = model.slice(0);
    check(fixture && s0, "no interlock: one fixture slice");
    if (!fixture || !s0) {
        std::printf("      fixture: %s\n", qPrintable(error));
        return;
    }
    SliceDelta tx = antennas({QStringLiteral("ANT1"), QStringLiteral("ANT2")}, QStringLiteral("ANT1"),
                             {QStringLiteral("ANT1"), QStringLiteral("ANT2")}, QStringLiteral("ANT1"));
    tx.txSlice = true;
    tx.mode = QStringLiteral("USB");
    s0->applyChanges(tx);
    TciServer server(&model);
    server.wireSlice(s0->sliceId(), s0);
    Wire a;
    TciAntennaExtensionTest::attach(server, a.client, model);
    int warnings = 0;
    QObject::connect(&model, &RadioModel::configurationWarning,
                     [&warnings](const QString&) { ++warnings; });
    settle(600);
    a.take();
    a.send("aethersdr_ext_enable:true;");
    a.take();
    const auto setTo = [&a](const char* antenna) {
        a.send(QStringLiteral("aethersdr_tx_antenna:0,%1;").arg(QLatin1String(antenna)).toLatin1().constData());
        settle();
        return a.takeExtension();
    };
    const QStringList stays1{QStringLiteral("aethersdr_tx_antenna:0,ANT1;")};

    model.setTransmit(true);
    check(backend->keyEdges == QList<bool>({true}) && model.isRadioTransmitting(),
          "no interlock: desktop MOX keys the backend, which publishes its TX edge");
    check(setTo("ANT2") == stays1 && backend->txRequests.isEmpty() && warnings == 1,
          "no interlock: a TX set while keyed is refused, and noticed");
    model.setTransmit(false);
    check(backend->keyEdges == QList<bool>({true, false}) && model.transmitOwnershipPending()
              && !model.isRadioTransmitting() && !model.transmitModel().isTransmitting(),
          "no interlock: after the key-up the over's TX ownership is retained, unconfirmed");
    const QStringList moved = setTo("ANT2");
    check(backend->txRequests.size() == 1 && s0->txAntenna() == QStringLiteral("ANT2")
              && moved == QStringList({QStringLiteral("aethersdr_tx_antenna:0,ANT2;")})
              && model.transmitOwnershipPending(),
          "no interlock: after the backend's RX edge, a TX set is accepted; ownership retained");
    if (backend->txRequests.size() != 1) {
        dump("a", moved);
    }

    // The RX edge alone is not idle while the key-up is still being delivered.
    backend->holdKeyUp = true;
    model.setTransmit(true);
    model.setTransmit(false);
    check(!model.isRadioTransmitting() && setTo("ANT1").size() == 1
              && backend->txRequests.size() == 1 && warnings == 2,
          "no interlock: a TX set while the key-up is still being delivered is refused");
    backend->holdKeyUp = false;
    backend->heldKeyUp.finish();
    backend->heldKeyUp = {};
    settle();
    setTo("ANT1");
    check(backend->txRequests.size() == 2 && s0->txAntenna() == QStringLiteral("ANT1"),
          "no interlock: once it is delivered, the TX set is accepted");
}

// One enabled client is behind on the item list, the other already holds it:
// the resend reaches only the first, so a list change is still noticed to both.
void mixedResend()
{
    RadioModel model;
    auto owner = std::make_unique<AntennaBackend>();
    AntennaBackend* backend = owner.get();
    backend->connected = false;
    model.setBackendForTest(std::move(owner), QStringLiteral("test"));
    QString error;
    const bool fixture = model.automationApplySliceFixture(0, QStringLiteral("A"), &error);
    backend->connected = true;
    SliceModel* s0 = model.slice(0);
    check(fixture && s0, "mixed: one fixture slice");
    if (!fixture || !s0) {
        std::printf("      fixture: %s\n", qPrintable(error));
        return;
    }
    s0->applyChanges(antennas({QStringLiteral("ANT1"), QStringLiteral("ANT2")},
                              QStringLiteral("ANT1"),
                              {QStringLiteral("ANT1"), QStringLiteral("ANT2")},
                              QStringLiteral("ANT1")));
    TciServer server(&model);
    server.wireSlice(s0->sliceId(), s0);
    Wire a;
    Wire c;
    TciAntennaExtensionTest::attach(server, a.client, model);
    TciAntennaExtensionTest::attach(server, c.client, model);
    settle(600);
    a.take();
    c.take();
    a.send("aethersdr_ext_enable:true;");
    check(lastAdvertisement(a.take()) == QStringLiteral("aethersdr_ext:1,rx_antenna,tx_antenna;"),
          "mixed: the first client enables with both items");
    // The TX gate moves with no capabilitiesChanged yet; the second client
    // enables in between and holds the newer list.
    backend->caps.canTransmit = false;
    c.send("aethersdr_ext_enable:true;");
    check(lastAdvertisement(c.take()) == QStringLiteral("aethersdr_ext:1,rx_antenna;"),
          "mixed: the second client enables with rx_antenna only");
    s0->applyChanges(antennas({QStringLiteral("ANT1"), QStringLiteral("ANT2"),
                               QStringLiteral("RX_A")},
                              QStringLiteral("ANT1"),
                              {QStringLiteral("ANT1"), QStringLiteral("ANT2")},
                              QStringLiteral("ANT1")));
    settle();
    const QStringList gotA = a.takeExtension();
    const QStringList gotC = c.takeExtension();
    check(lastAdvertisement(gotA) == QStringLiteral("aethersdr_ext:1,rx_antenna;")
              && gotA.contains(QStringLiteral("aethersdr_rx_antennas:0,ANT1,ANT2,RX_A;")),
          "mixed: the client behind gets the item list with the new list");
    // Its selection line may come along (the slice re-publishes it); the list
    // line is the one a skipped notice would lose.
    const bool currentNoticed = lastAdvertisement(gotC).isEmpty()
        && gotC.count(QStringLiteral("aethersdr_rx_antennas:0,ANT1,ANT2,RX_A;")) == 1;
    check(currentNoticed, "mixed: the client already current gets the new list as a notice");
    if (!currentNoticed) {
        dump("c", gotC);
    }
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-tci-ext-antennas-test"));
    DaxIqModel::setPactlRunnerForTest([](const QStringList&) { return quint32{0}; });
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    check(settingsProfile.isValid(), "isolated settings profile");
    wireGrammar();
    transmitGatePerBackend();
    serverBehaviour();
    mixedResend();
    txSetAfterAnOver();
    txSetAfterAnOverWithoutInterlock();

    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
