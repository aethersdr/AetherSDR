// The transmit-ceiling item of the opt-in TCI extension set
// (`aethersdr_tx_power_max`): TransmitModel::maxPowerLevel() in whole watts,
// offered while the connected radio can transmit and has reported a ceiling,
// read only, one line for the TX trx. Sent on enable, and again only when the
// value or its trx moves (a TX move, a reconnect's reclaim rebinding the trx);
// a gate edge re-announces the set. Socket-free.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/TciClient.h"
#include "core/TciExtensionSet.h"
#include "core/TciProtocol.h"
#include "core/TciServer.h"
#include "core/TciTxPowerExtension.h"
#include "core/backends/IRadioBackend.h"
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
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QElapsedTimer>
#include <QList>

#include <cstdio>
#include <functional>
#include <memory>

namespace AetherSDR {

class TciTxPowerExtensionTest {
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
    // A reconnect's reclaim end state (tci_server_review_test's pattern): the
    // map was cleared at disconnect and the slices bind again in `order`.
    static void rebind(TciServer& server, const QList<int>& order)
    {
        server.m_trxMap.clear();
        for (const int sliceId : order) {
            server.m_trxMap.acquire(sliceId);
        }
    }
};

namespace {

// Connected, transmit capable; the ceiling is the test's.
class PowerBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    bool connected{true};

    PowerBackend() { caps.canTransmit = true; }
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connected; }
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

void settle(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// Spins until `done` holds or the deadline passes; a loaded runner only
// takes longer.
bool waitFor(const std::function<bool()>& done, int deadlineMs = 5000)
{
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
        if (clock.elapsed() > deadlineMs) {
            return false;
        }
        settle(20);
    }
    return true;
}

// Absence cannot be polled: wait past the 100 ms notice limit and its post.
constexpr int kQuietMs = 300;

void dump(const char* who, const QStringList& got)
{
    for (const QString& line : got) {
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
        QStringList out = rx;
        rx.clear();
        return out;
    }
    QStringList takeExtension()
    {
        QStringList out;
        for (const QString& line : take()) {
            if (line.startsWith(QLatin1String("aethersdr_"))) {
                out << line;
            }
        }
        return out;
    }
};

QStringList ofItem(const QStringList& got)
{
    QStringList out;
    for (const QString& line : got) {
        if (line.startsWith(QLatin1String("aethersdr_tx_power_max:"))) {
            out << line;
        }
    }
    return out;
}

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

QStringList one(const char* text)
{
    return QStringList{QString::fromLatin1(text)};
}

void wireGrammar()
{
    using namespace TciTxPowerExtension;
    check(line(0, 5) == QStringLiteral("aethersdr_tx_power_max:0,5;")
              && line(1, 100) == QStringLiteral("aethersdr_tx_power_max:1,100;")
              && line(0, 500) == QStringLiteral("aethersdr_tx_power_max:0,500;"),
          "the line: <trx>,<watts>, watts an integer");
    const auto parse = [](const char* args) {
        const QString text = QString::fromLatin1(args);
        return parseRead(text.isEmpty() ? QStringList{}
                                        : text.split(QLatin1Char(','), Qt::KeepEmptyParts));
    };
    check(parse("") == -1 && parse("0") == 0 && parse("2") == 2 && !parse("-1")
              && !parse("x") && !parse("0,5") && !parse("0,"),
          "a read is bare or names a trx; a set does not parse");
}

void gate()
{
    using namespace TciTxPowerExtension;
    check(offered(true, true, true, 5), "connected, transmits, ceiling reported: offered");
    check(!offered(false, true, true, 5), "not connected: not offered");
    check(!offered(true, false, true, 5), "a receive-only radio: not offered");
    check(!offered(true, true, false, 100),
          "no ceiling reported: the model's compiled-in 100 is not offered");
    check(!offered(true, true, true, 0), "a 0 W ceiling: not offered");
}

SliceDelta tuned(double mhz, bool tx)
{
    SliceDelta d;
    d.frequency = mhz;
    d.txSlice = tx;
    return d;
}

struct Bench {
    RadioModel model;
    PowerBackend* backend{nullptr};
    SliceModel* s0{nullptr};
    SliceModel* s1{nullptr};
};

// Two fixture slices, slice 0 transmitting at 7.074 MHz, connected, with the
// given capabilities. Not connected-edged: TransmitModel holds no ceiling yet.
bool makeBench(Bench& bench, const RadioCapabilities& caps)
{
    auto owner = std::make_unique<PowerBackend>();
    bench.backend = owner.get();
    bench.backend->caps = caps;
    bench.backend->connected = false;  // slice fixtures are applied while disconnected
    bench.model.setBackendForTest(std::move(owner), QStringLiteral("test"));
    QString error;
    const bool fixtures = bench.model.automationApplySliceFixture(0, QStringLiteral("A"), &error)
        && bench.model.automationApplySliceFixture(1, QStringLiteral("B"), &error);
    bench.s0 = bench.model.slice(0);
    bench.s1 = bench.model.slice(1);
    if (!fixtures || !bench.s0 || !bench.s1) {
        std::printf("      fixture: %s\n", qPrintable(error));
        return false;
    }
    bench.s0->applyChanges(tuned(7.074, true));
    bench.s1->applyChanges(tuned(14.074, false));
    bench.backend->connected = true;
    return bench.model.isConnected() && bench.model.txSlice() == bench.s0;
}

// The real Hermes-Lite 2 capabilities, through RadioModel's own connect edge
// (refreshTxPowerLimit), give the value: 5, whole watts. RTL is not offered.
void hl2ThroughTheModel()
{
    RadioModel hl2;
    if (!hl2.rebuildBackendForTest(QStringLiteral("hl2")) || !hl2.backend()) {
        check(false, "the hl2 backend builds");
        return;
    }
    Bench bench;
    if (!makeBench(bench, hl2.backend()->capabilities())) {
        check(false, "hl2 bench");
        return;
    }
    emit bench.model.connectionStateChanged(true);
    const TransmitModel& tx = bench.model.transmitModel();
    std::printf("      hl2: have=%d maxPowerLevel=%d\n", tx.haveMaxPowerLevel(), tx.maxPowerLevel());
    TciServer server(&bench.model);
    server.wireSlice(bench.s0->sliceId(), bench.s0);
    server.wireSlice(bench.s1->sliceId(), bench.s1);
    Wire a;
    TciTxPowerExtensionTest::attach(server, a.client, bench.model);
    settle(kQuietMs);
    a.take();
    a.send("aethersdr_ext_enable:true;");
    const QStringList got = a.take();
    check(lastAdvertisement(got).contains(QLatin1String(",tx_power_max"))
              && ofItem(got) == one("aethersdr_tx_power_max:0,5;"),
          "a Hermes-Lite 2 that transmits reports 5, from TransmitModel::maxPowerLevel()");
    if (ofItem(got).size() != 1) {
        dump("a", got);
    }

    RadioModel rtl;
    if (rtl.rebuildBackendForTest(QStringLiteral("rtl")) && rtl.backend()) {
        check(!rtl.backend()->capabilities().canTransmit
                  && !TciTxPowerExtension::offered(true, rtl.backend()->capabilities().canTransmit,
                                                   true, 5),
              "RTL-SDR (receive only) is not offered, whatever the model holds");
    } else {
        std::printf("      rtl: not built into this tree, not checked\n");
    }
}

void serverBehaviour()
{
    RadioCapabilities caps;
    caps.canTransmit = true;  // and no txPowerBands: a Flex-shaped radio
    Bench bench;
    check(makeBench(bench, caps), "two fixture slices, connected, slice 0 transmits");
    if (!bench.s0) {
        return;
    }
    RadioModel& model = bench.model;
    SliceModel* s0 = bench.s0;
    SliceModel* s1 = bench.s1;
    TransmitModel& tx = model.transmitModel();

    TciServer server(&model);
    server.wireSlice(s0->sliceId(), s0);
    server.wireSlice(s1->sliceId(), s1);
    Wire a;  // enabled
    Wire b;  // plain
    TciTxPowerExtensionTest::attach(server, a.client, model);
    TciTxPowerExtensionTest::attach(server, b.client, model);
    emit model.capabilitiesChanged(true, caps);
    settle(600);
    a.take();
    b.take();

    check(!tx.haveMaxPowerLevel()
              && !TciTxPowerExtensionTest::offeredTokens(server)
                      .contains(QStringLiteral("tx_power_max")),
          "no ceiling reported yet: not offered, the default 100 is not a ceiling");
    a.send("aethersdr_ext_enable:true;");
    QStringList got = a.take();
    check(!lastAdvertisement(got).isEmpty()
              && !lastAdvertisement(got).contains(QLatin1String("tx_power_max"))
              && ofItem(got).isEmpty(),
          "the enable does not list it");

    // A first report equal to the default moves only the latch.
    tx.setMaxPowerLevel(100);
    check(waitFor([&] { return ofItem(a.rx).size() == 1; }), "a first report arrives");
    got = a.takeExtension();
    check(lastAdvertisement(got).contains(QLatin1String(",tx_power_max"))
              && ofItem(got) == one("aethersdr_tx_power_max:0,100;"),
          "a first report of 100 W offers the item, with one line for the TX trx");
    if (ofItem(got).size() != 1) {
        dump("a", got);
    }

    // Flex: the radio's max_power_level, through TransmitModel::applyChanges().
    TransmitDelta flex;
    flex.maxPowerLevel = 500;
    tx.applyChanges(flex);
    check(waitFor([&] { return !ofItem(a.rx).isEmpty(); }), "a new ceiling arrives");
    got = a.takeExtension();
    check(got == one("aethersdr_tx_power_max:0,500;"),
          "a new max_power_level (a Flex with an amplifier) sends one line, nothing else");
    if (got.size() != 1) {
        dump("a", got);
    }

    s0->applyChanges(tuned(7.200, true));
    s0->applyChanges(tuned(14.074, true));
    s1->applyChanges(tuned(21.074, false));
    settle(kQuietMs);
    check(a.takeExtension().isEmpty(), "a retune that leaves the ceiling alone sends nothing");

    a.send("aethersdr_tx_power_max;");
    check(a.take() == one("aethersdr_tx_power_max:0,500;"), "a bare read answers the sender");
    a.send("aethersdr_tx_power_max:0;");
    check(a.take() == one("aethersdr_tx_power_max:0,500;"), "a read naming the TX trx too");
    a.send("aethersdr_tx_power_max:1;aethersdr_tx_power_max:0,5;");
    settle(kQuietMs);
    check(a.takeExtension().isEmpty(), "a read of another trx, or a set, gets no answer");

    // TX moves to slice 1.
    s0->applyChanges(tuned(14.074, false));
    s1->applyChanges(tuned(21.074, true));
    check(waitFor([&] { return !ofItem(a.rx).isEmpty(); }), "the TX move is heard");
    settle(kQuietMs);
    got = a.takeExtension();
    check(ofItem(got) == one("aethersdr_tx_power_max:1,500;"),
          "a TX move sends one line for the new trx");
    if (ofItem(got).size() != 1) {
        dump("a", got);
    }

    // With no TX slice the line keeps naming the last TX trx, as tx_filter_band does.
    s1->applyChanges(tuned(21.074, false));
    settle(700);
    got = a.takeExtension();
    check(model.txSlice() == nullptr && ofItem(got).isEmpty() && lastAdvertisement(got).isEmpty(),
          "no TX slice for a moment: the item stays, nothing is re-announced");
    s1->applyChanges(tuned(21.074, true));
    settle(700);
    got = a.takeExtension();
    check(ofItem(got).isEmpty() && lastAdvertisement(got).isEmpty(),
          "the TX slice back on the same trx sends nothing");

    caps.canTransmit = false;
    bench.backend->caps = caps;
    emit model.capabilitiesChanged(true, caps);
    check(waitFor([&] { return !lastAdvertisement(a.rx).isEmpty(); }), "the list is re-announced");
    settle(kQuietMs);
    got = a.takeExtension();
    // Every announce of this edge, not only the last: none may still list it.
    bool listed = false;
    for (const QString& line : got) {
        listed = listed || (line.startsWith(QLatin1String("aethersdr_ext:"))
                            && line.contains(QLatin1String("tx_power_max")));
    }
    check(!listed, "no announce on the capability edge lists the withdrawn item");
    check(!lastAdvertisement(got).contains(QLatin1String("tx_power_max")) && ofItem(got).isEmpty(),
          "a radio that stops transmitting withdraws the item");
    caps.canTransmit = true;
    bench.backend->caps = caps;
    emit model.capabilitiesChanged(true, caps);
    check(waitFor([&] { return !ofItem(a.rx).isEmpty(); }), "the item returns");
    settle(kQuietMs);
    got = a.takeExtension();
    check(lastAdvertisement(got).contains(QLatin1String(",tx_power_max"))
              && ofItem(got) == one("aethersdr_tx_power_max:1,500;"),
          "a radio that can transmit again restores the item with one line");
    if (ofItem(got).size() != 1) {
        dump("a", got);
    }

    // A reconnect: the disconnect resets the model's ceiling, the reconnect
    // reports one again, and the reclaim rebinds the slices in the order they
    // come back, slice 1 first, on the occupancy edge alone.
    bench.backend->connected = false;
    tx.resetState();
    emit model.capabilitiesChanged(false, caps);
    check(waitFor([&] { return !lastAdvertisement(a.rx).isEmpty(); }), "disconnect re-announces");
    got = a.takeExtension();
    check(!lastAdvertisement(got).contains(QLatin1String("tx_power_max")) && ofItem(got).isEmpty(),
          "a disconnect withdraws the item");
    bench.backend->connected = true;
    emit model.capabilitiesChanged(true, caps);
    settle(kQuietMs);
    got = a.takeExtension();
    check(ofItem(got).isEmpty()
              && !TciTxPowerExtensionTest::offeredTokens(server)
                      .contains(QStringLiteral("tx_power_max")),
          "reconnected, no ceiling yet: still withdrawn");
    tx.setMaxPowerLevel(500);
    check(waitFor([&] { return !ofItem(a.rx).isEmpty(); }), "the reconnect's ceiling is heard");
    settle(kQuietMs);
    got = a.takeExtension();
    check(lastAdvertisement(got).contains(QLatin1String(",tx_power_max"))
              && ofItem(got) == one("aethersdr_tx_power_max:1,500;"),
          "the ceiling reported after a reconnect restores the item");
    if (ofItem(got).size() != 1) {
        dump("a", got);
    }
    TciTxPowerExtensionTest::rebind(server, {1, 0});
    emit model.slotOccupancyChanged(1);
    check(waitFor([&] { return !ofItem(a.rx).isEmpty(); }), "the reclaim is heard");
    settle(kQuietMs);
    got = a.takeExtension();
    check(ofItem(got) == one("aethersdr_tx_power_max:0,500;"),
          "a reclaim that rebinds the TX slice to trx 0 sends the line for trx 0");
    if (ofItem(got).size() != 1) {
        dump("a", got);
    }

    check(b.takeExtension().isEmpty(), "a client that never enabled the set heard none of it");
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-tci-ext-tx-power-max-test"));
    DaxIqModel::setPactlRunnerForTest([](const QStringList&) { return quint32{0}; });
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    check(settingsProfile.isValid(), "isolated settings profile");
    wireGrammar();
    gate();
    hl2ThroughTheModel();
    serverBehaviour();

    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
