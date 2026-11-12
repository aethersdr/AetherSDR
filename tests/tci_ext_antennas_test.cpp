// The antenna groups of the opt-in TCI extension set (`aethersdr_rx_antenna`,
// `aethersdr_tx_antenna` and their lists, RFC #7181): offered while a receiver
// publishes a list (TX: on a radio that can transmit), set through
// SliceModel::setRxAntenna()/setTxAntenna() like the RX applet's menus, the TX
// antenna refused while transmitting, and every change, lists included, sent
// to enabled clients only; an RX set asks first for a KiwiSDR antenna to be
// released, and a keyed refusal warns once per keyed period. Two slices.
// Socket-free; nothing keys.

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
           "aethersdr_rx_antenna:0,bad,name;aethersdr_tx_antenna:0,ANT1;");
    settle();
    check(a.takeExtension().isEmpty() && backend->rxRequests.isEmpty()
              && backend->txRequests.isEmpty(),
          "an unknown trx, a malformed line, an unlisted name or the antenna in use: no reply, "
          "nothing written");
    check(kiwiReleases.isEmpty(), "and no KiwiSDR release for any of them");

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
              && backend->rxRequests.size() == 1 && a.takeExtension().isEmpty(),
          "an RX set naming the antenna in use still releases a KiwiSDR antenna, nothing else");
    c.take();

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

    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
