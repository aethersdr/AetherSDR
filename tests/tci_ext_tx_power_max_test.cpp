// The transmit-ceiling item of the opt-in TCI extension set
// (`aethersdr_tx_power_max`): offered while the connected radio can transmit
// and its ceiling at the TX frequency is above 0 W, read only, one line for
// the trx of the TX slice. Sent on enable, and again only when the value or
// its trx moves: a retune inside one txPowerBands range sends nothing, a
// boundary crossing, a TX move or a capability change sends the new line, and
// a ceiling that goes away takes the item out of the list. Socket-free.

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
#include "models/DaxIqModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtNumeric>

#include <cstdio>
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

void settle(int ms = 160)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
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
    check(line(0, 5.0) == QStringLiteral("aethersdr_tx_power_max:0,5.0;")
              && line(1, 100.0) == QStringLiteral("aethersdr_tx_power_max:1,100.0;")
              && line(0, 12.34) == QStringLiteral("aethersdr_tx_power_max:0,12.3;")
              && line(0, 0.25) == QStringLiteral("aethersdr_tx_power_max:0,0.3;"),
          "the line: <trx>,<watts>, watts with one decimal place");
    check(tenths(5.0) == 50 && tenths(0.04) == 0 && tenths(-1.0) == 0 && tenths(qQNaN()) == 0,
          "tenths of a watt; nothing below 0.05 W, negative or not a number");
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
    RadioCapabilities caps;
    caps.canTransmit = true;
    caps.txPowerMaxWatts = 100.0;
    caps.txPowerBands = {TxPowerBand{1.8e6, 30.0e6, 100.0}, TxPowerBand{50.0e6, 54.0e6, 50.0},
                         TxPowerBand{144.0e6, 148.0e6, 0.0}};
    const RadioCapabilities cached = limits(caps);
    check(wattsAt(cached, 14.074e6) == 100.0 && wattsAt(cached, 50.313e6) == 50.0
              && wattsAt(cached, 144.3e6) == 0.0 && wattsAt(cached, 40.0e6) == 100.0,
          "per-band ceilings: the band holding the frequency, txPowerMaxWatts outside every band");
    check(offered(true, true, true, wattsAt(cached, 50.313e6))
              && !offered(true, true, true, wattsAt(cached, 144.3e6)),
          "a band whose ceiling is 0 W is not offered");
    caps.canTransmit = false;
    check(wattsAt(caps, 14.074e6) == 0.0 && !offered(true, false, true, 100.0),
          "a receive-only radio is not offered, whatever its bands say");
    check(!offered(false, true, true, 5.0) && !offered(true, true, false, 5.0),
          "no radio, or no TX slice: not offered");
}

// The real backends as the production family switch builds them.
void gatePerBackend()
{
    RadioModel hl2;
    if (hl2.rebuildBackendForTest(QStringLiteral("hl2")) && hl2.backend()) {
        const RadioCapabilities caps = hl2.backend()->capabilities();
        const double watts = TciTxPowerExtension::wattsAt(caps, 7.074e6);
        std::printf("      hl2: canTransmit=%d watts=%.2f\n", caps.canTransmit, watts);
        check(caps.canTransmit && TciTxPowerExtension::offered(true, caps.canTransmit, true, watts)
                  && TciTxPowerExtension::line(0, watts)
                         == QStringLiteral("aethersdr_tx_power_max:0,5.0;"),
              "a Hermes-Lite 2 that transmits reports 5.0 W");
    } else {
        check(false, "the hl2 backend builds");
    }
    RadioModel rtl;
    if (rtl.rebuildBackendForTest(QStringLiteral("rtl")) && rtl.backend()) {
        const RadioCapabilities caps = rtl.backend()->capabilities();
        check(!TciTxPowerExtension::offered(true, caps.canTransmit, true,
                                            TciTxPowerExtension::wattsAt(caps, 7.074e6)),
              "RTL-SDR (receive only) is not offered");
    } else {
        std::printf("      rtl: not built into this tree, not checked\n");
    }
}

SliceDelta tuned(double mhz, bool tx)
{
    SliceDelta d;
    d.frequency = mhz;
    d.txSlice = tx;
    return d;
}

void serverBehaviour()
{
    RadioModel model;
    auto owner = std::make_unique<PowerBackend>();
    PowerBackend* backend = owner.get();
    backend->caps.txPowerMaxWatts = 100.0;
    backend->caps.txPowerBands = {TxPowerBand{1.8e6, 30.0e6, 100.0},
                                  TxPowerBand{50.0e6, 54.0e6, 50.0},
                                  TxPowerBand{144.0e6, 148.0e6, 0.0}};
    backend->connected = false;  // slice fixtures are applied while disconnected
    model.setBackendForTest(std::move(owner), QStringLiteral("test"));
    QString error;
    const bool fixtures = model.automationApplySliceFixture(0, QStringLiteral("A"), &error)
        && model.automationApplySliceFixture(1, QStringLiteral("B"), &error);
    SliceModel* s0 = model.slice(0);
    SliceModel* s1 = model.slice(1);
    check(fixtures && s0 && s1, "two fixture slices");
    if (!fixtures || !s0 || !s1) {
        std::printf("      fixture: %s\n", qPrintable(error));
        return;
    }
    s0->applyChanges(tuned(14.074, true));
    s1->applyChanges(tuned(7.074, false));
    backend->connected = true;
    check(model.isConnected() && model.txSlice() == s0, "connected, slice 0 transmits");

    TciServer server(&model);
    server.wireSlice(s0->sliceId(), s0);
    server.wireSlice(s1->sliceId(), s1);
    Wire a;  // enabled
    Wire b;  // plain
    TciTxPowerExtensionTest::attach(server, a.client, model);
    TciTxPowerExtensionTest::attach(server, b.client, model);
    settle(600);
    a.take();
    b.take();

    check(TciTxPowerExtensionTest::offeredTokens(server).contains(QStringLiteral("tx_power_max")),
          "offered on a connected transmitter with a ceiling at the TX frequency");
    a.send("aethersdr_ext_enable:true;");
    QStringList got = a.take();
    check(lastAdvertisement(got).contains(QLatin1String(",tx_power_max"))
              && ofItem(got) == one("aethersdr_tx_power_max:0,100.0;"),
          "the enable lists tx_power_max and sends one line, for the TX slice's trx");
    if (ofItem(got).size() != 1) {
        dump("a", got);
    }

    s0->applyChanges(tuned(14.250, true));
    s0->applyChanges(tuned(21.074, true));
    settle();
    check(a.takeExtension().isEmpty(), "a retune inside one band sends nothing");

    s0->applyChanges(tuned(50.313, true));
    settle();
    got = a.takeExtension();
    check(got == one("aethersdr_tx_power_max:0,50.0;"),
          "crossing into another band sends its ceiling");
    if (got.size() != 1) {
        dump("a", got);
    }

    s1->applyChanges(tuned(52.0, false));
    settle();
    check(a.takeExtension().isEmpty(), "a receive-only slice's retune sends nothing");

    a.send("aethersdr_tx_power_max;");
    check(a.take() == one("aethersdr_tx_power_max:0,50.0;"), "a bare read answers the sender");
    a.send("aethersdr_tx_power_max:0;");
    check(a.take() == one("aethersdr_tx_power_max:0,50.0;"), "a read naming the TX trx too");
    a.send("aethersdr_tx_power_max:1;aethersdr_tx_power_max:0,5;");
    settle();
    check(a.takeExtension().isEmpty(), "a read of another trx, or a set, gets no answer");

    // TX moves to slice 1 (52 MHz): same ceiling, another trx.
    s0->applyChanges(tuned(50.313, false));
    s1->applyChanges(tuned(52.0, true));
    settle(700);  // past the TX-move settle
    got = a.takeExtension();
    check(ofItem(got) == one("aethersdr_tx_power_max:1,50.0;"),
          "a TX move sends the line for the new trx");
    if (ofItem(got).size() != 1) {
        dump("a", got);
    }

    // A band with a 0 W ceiling takes the item out of the list.
    s1->applyChanges(tuned(144.3, true));
    settle();
    got = a.takeExtension();
    check(!lastAdvertisement(got).isEmpty()
              && !lastAdvertisement(got).contains(QLatin1String("tx_power_max"))
              && ofItem(got).isEmpty(),
          "a band whose ceiling is 0 W withdraws the item");
    a.send("aethersdr_tx_power_max;");
    check(a.take().isEmpty(), "a read of a withdrawn item is ignored");
    s1->applyChanges(tuned(7.074, true));
    settle();
    got = a.takeExtension();
    check(lastAdvertisement(got).contains(QLatin1String("tx_power_max"))
              && ofItem(got) == one("aethersdr_tx_power_max:1,100.0;"),
          "back in a band with a ceiling, the item returns with its line");
    if (ofItem(got).size() != 1) {
        dump("a", got);
    }

    // A capability change: the backend's ceiling moves.
    backend->caps.txPowerBands = {};
    backend->caps.txPowerMaxWatts = 10.0;
    emit model.capabilitiesChanged(true, backend->caps);
    settle();
    got = a.takeExtension();
    check(ofItem(got) == one("aethersdr_tx_power_max:1,10.0;"),
          "a capability change sends the new ceiling");
    if (ofItem(got).size() != 1) {
        dump("a", got);
    }

    backend->caps.canTransmit = false;
    emit model.capabilitiesChanged(true, backend->caps);
    settle();
    got = a.takeExtension();
    check(!lastAdvertisement(got).isEmpty()
              && !lastAdvertisement(got).contains(QLatin1String("tx_power_max"))
              && ofItem(got).isEmpty(),
          "a radio that stops transmitting withdraws the item");

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
    gatePerBackend();
    serverBehaviour();

    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
