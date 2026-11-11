// The TX passband group of the opt-in TCI extension set
// (`aethersdr_tx_filter_band`, RFC #7181): offered only where the backend
// declares TX cutoff controls, set through TransmitModel::setTxFilter() like
// the desktop control, clamps answered with the applied value, and changes
// from any source notified to enabled clients only. Socket-free.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/TciClient.h"
#include "core/TciExtensionSet.h"
#include "core/TciProtocol.h"
#include "core/TciServer.h"
#include "core/TciTxPassbandExtension.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/RadioCapabilities.h"
#include "core/backends/SliceDelta.h"
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

#include <climits>
#include <cstdio>
#include <memory>
#include <variant>

namespace AetherSDR {

class TciTxPassbandExtensionTest {
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

// Connected, with the TX cutoff capability the test sets; records what the
// model path hands it.
class FilterBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    QList<TxPassband> filterRequests;
    int directFilterCalls{0};
    bool connected{true};

    FilterBackend() { caps.canTransmit = true; }
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connected; }
    ReceiveDispatch requestTransmitControl(const TransmitControlRequest& request,
                                           TunePowerContext) override
    {
        if (request.field == TransmitControlRequest::Field::Filter) {
            filterRequests.append(std::get<TxPassband>(request.value));
        }
        return ReceiveDispatch::LocalOnly;
    }
    void setTxFilter(int, int) override { ++directFilterCalls; }
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
};

QStringList one(const char* line)
{
    return QStringList{QString::fromLatin1(line)};
}

bool passbandIs(const TransmitModel& tx, int low, int high)
{
    return tx.txFilterLow() == low && tx.txFilterHigh() == high;
}

// The real backends, as the production family switch builds them.
void offeredPerBackend()
{
    const struct {
        const char* family;
        bool expected;
    } cases[] = {{"hl2", true}, {"flex", true}, {"sim", false}, {"icom", false}};
    for (const auto& c : cases) {
        RadioModel model;
        const bool built = model.rebuildBackendForTest(QString::fromLatin1(c.family))
            && model.backend();
        const bool offered = built
            && TciTxPassbandExtension::offered(true, model.backend()->capabilities());
        std::printf("      %s: built=%d offered=%d\n", c.family, built, offered);
        check(built && offered == c.expected,
              "a backend is offered the TX passband exactly when it declares TX cutoffs");
    }
    RadioModel rtl;
    if (rtl.rebuildBackendForTest(QStringLiteral("rtl")) && rtl.backend()) {
        check(!TciTxPassbandExtension::offered(true, rtl.backend()->capabilities()),
              "RTL-SDR (receive only) is not offered the TX passband");
    } else {
        std::printf("      rtl: not built into this tree, not checked\n");
    }
    RadioCapabilities caps;
    caps.hasTxFilterControls = true;
    check(!TciTxPassbandExtension::offered(false, caps), "no radio connected, nothing offered");
}

void normalisation()
{
    RadioCapabilities continuous;
    const auto n = [&](int low, int high, const RadioCapabilities& caps) {
        const auto p = TciTxPassbandExtension::normalise(low, high, caps);
        return QStringLiteral("%1,%2").arg(p.lowHz).arg(p.highHz);
    };
    check(n(2900, -100, continuous) == QStringLiteral("100,2900"),
          "a reversed, negative pair is taken absolute and swapped");
    check(n(INT_MIN, 500, continuous) == QStringLiteral("500,10000")
              && n(INT_MAX, INT_MAX, continuous) == QStringLiteral("10000,10000"),
          "INT_MIN and INT_MAX are bounded to the model's range before abs");
    RadioCapabilities edges;
    edges.txFilterLowEdgesHz = {100, 200, 300, 500};
    edges.txFilterHighEdgesHz = {2500, 2700, 2800, 2900};
    check(n(240, 2650, edges) == QStringLiteral("200,2700")
              && n(0, 9000, edges) == QStringLiteral("100,2900"),
          "a radio with edge lists gets the nearest listed edges");
    const auto parse = [](const char* args) {
        return TciTxPassbandExtension::parse(
            QString::fromLatin1(args).split(QLatin1Char(','), Qt::SkipEmptyParts));
    };
    check(!parse("") && !parse("0,100") && !parse("0,x,100") && !parse("-1")
              && !parse("0,1,2,3") && parse("0") && !parse("0")->isSet
              && parse("0,1,2")->isSet,
          "only <trx> and <trx>,<low>,<high> parse");
}

void serverBehaviour()
{
    RadioModel model;
    auto owner = std::make_unique<FilterBackend>();
    FilterBackend* backend = owner.get();
    backend->caps.hasTxFilterControls = false;
    model.setBackendForTest(std::move(owner), QStringLiteral("test"));
    TransmitModel& tx = model.transmitModel();
    tx.setTxFilter(100, 2900);
    TciServer server(&model);
    Wire a;  // enabled
    Wire b;  // plain
    Wire c;  // enabled
    TciTxPassbandExtensionTest::attach(server, a.client, model);
    TciTxPassbandExtensionTest::attach(server, b.client, model);
    TciTxPassbandExtensionTest::attach(server, c.client, model);
    settle();
    backend->filterRequests.clear();

    // Gate: a connected radio without TX cutoffs offers nothing.
    check(model.isConnected() && TciTxPassbandExtensionTest::offeredTokens(server).isEmpty(),
          "a radio without TX cutoffs is not offered tx_filter_band");
    a.send("aethersdr_ext_enable:true;");
    check(a.take() == one("aethersdr_ext:1;"), "its enable reply lists no item");
    a.send("aethersdr_tx_filter_band:0;aethersdr_tx_filter_band:0,300,2400;");
    settle();
    check(a.take().isEmpty() && passbandIs(tx, 100, 2900) && backend->filterRequests.isEmpty(),
          "a read or set for the item not offered gets no reply and writes nothing");

    backend->caps.hasTxFilterControls = true;
    emit model.capabilitiesChanged(true, backend->caps);
    check(a.take() == QStringList({QStringLiteral("aethersdr_ext:1,tx_filter_band;"),
                                   QStringLiteral("aethersdr_tx_filter_band:0,100,2900;")}),
          "once declared, the list offers it with its state line");
    c.send("aethersdr_ext_enable:true;");
    check(c.take() == QStringList({QStringLiteral("aethersdr_ext:1,tx_filter_band;"),
                                   QStringLiteral("aethersdr_tx_filter_band:0,100,2900;")}),
          "a client enabling later gets the same list and state");
    check(b.take().isEmpty(), "the plain client heard none of it");

    // Read: to the requester only.
    a.send("aethersdr_tx_filter_band:0;");
    check(a.take() == one("aethersdr_tx_filter_band:0,100,2900;") && c.take().isEmpty(),
          "a read is answered to the requester only");
    a.send("aethersdr_tx_filter_band:1;aethersdr_tx_filter_band:1,300,2400;"
           "aethersdr_tx_filter_band;aethersdr_tx_filter_band:0,300;"
           "aethersdr_tx_filter_band:0,abc,2400;");
    settle();
    check(a.take().isEmpty() && passbandIs(tx, 100, 2900) && backend->filterRequests.isEmpty(),
          "another trx, a bare command and malformed sets are ignored");

    // Set: the model path, then one notice to every enabled client.
    a.send("aethersdr_tx_filter_band:0,300,2400;");
    check(passbandIs(tx, 300, 2400), "a set changes TransmitModel's TX passband");
    check(backend->filterRequests.size() == 1 && backend->filterRequests.first().lowHz == 300
              && backend->filterRequests.first().highHz == 2400
              && backend->directFilterCalls == 0,
          "the set reaches the backend as the desktop's TransmitControlRequest");
    settle();
    check(a.take() == one("aethersdr_tx_filter_band:0,300,2400;")
              && c.take() == one("aethersdr_tx_filter_band:0,300,2400;") && b.take().isEmpty(),
          "the change is notified once to each enabled client, not to the plain one");

    // Clamps reply with what was applied.
    a.send("aethersdr_tx_filter_band:0,1000,1020;");
    settle();
    check(passbandIs(tx, 1000, 1050)
              && a.take() == one("aethersdr_tx_filter_band:0,1000,1050;")
              && c.take() == one("aethersdr_tx_filter_band:0,1000,1050;"),
          "a set narrower than the minimum width replies the widened passband");
    a.send("aethersdr_tx_filter_band:0,-2147483648,500;");
    settle();
    check(passbandIs(tx, 500, TransmitModel::kTxFilterMaxHz)
              && a.take() == one("aethersdr_tx_filter_band:0,500,10000;"),
          "INT_MIN,500 replies 500,10000");
    c.take();
    a.send("aethersdr_tx_filter_band:0,2147483647,2147483647;");
    settle();
    check(passbandIs(tx, 9950, 10000)
              && a.take() == one("aethersdr_tx_filter_band:0,9950,10000;"),
          "a set past the maximum edge replies the clamped passband");
    c.take();
    a.send("aethersdr_tx_filter_band:0,12000,12000;");
    settle();
    check(a.take() == one("aethersdr_tx_filter_band:0,9950,10000;") && c.take().isEmpty(),
          "a clamp that changes nothing is still answered to the requester");
    a.send("aethersdr_tx_filter_band:0,9950,10000;");
    settle();
    check(a.take().isEmpty() && c.take().isEmpty(), "an exact set that changes nothing stays quiet");

    backend->caps.txFilterLowEdgesHz = {100, 200, 300, 500};
    backend->caps.txFilterHighEdgesHz = {2500, 2700, 2800, 2900};
    a.send("aethersdr_tx_filter_band:0,240,2650;");
    settle();
    check(passbandIs(tx, 200, 2700) && a.take() == one("aethersdr_tx_filter_band:0,200,2700;"),
          "a radio with edge lists is set to, and replies, the nearest edges");
    backend->caps.txFilterLowEdgesHz.clear();
    backend->caps.txFilterHighEdgesHz.clear();
    c.take();

    // A desktop change (ClientEq drag: low, then high) is notified as one pair.
    tx.setTxFilterLow(150);
    tx.setTxFilterHigh(2600);
    settle();
    check(a.take() == one("aethersdr_tx_filter_band:0,150,2600;")
              && c.take() == one("aethersdr_tx_filter_band:0,150,2600;") && b.take().isEmpty(),
          "a desktop change reaches enabled clients as one pair, and not the plain client");
    check(b.rx.isEmpty(), "the plain client received nothing at all");

    // A plain or disabled client's command is dropped.
    const int requests = backend->filterRequests.size();
    b.send("aethersdr_tx_filter_band:0,400,2000;aethersdr_tx_filter_band:0;");
    c.send("aethersdr_ext_enable:false;");
    c.send("aethersdr_tx_filter_band:0,400,2000;aethersdr_tx_filter_band:0;");
    settle();
    check(passbandIs(tx, 150, 2600) && backend->filterRequests.size() == requests
              && a.take().isEmpty() && b.take().isEmpty() && c.take().isEmpty(),
          "a plain or disabled client's command is dropped and writes nothing");
    tx.setTxFilter(200, 2800);
    settle();
    check(a.take() == one("aethersdr_tx_filter_band:0,200,2800;") && c.take().isEmpty(),
          "a disabled client hears no notice");

    // Lost capability: no notice, and the item leaves the list.
    backend->caps.hasTxFilterControls = false;
    emit model.capabilitiesChanged(true, backend->caps);
    check(a.take() == one("aethersdr_ext:1;"), "a lost capability re-sends the list without it");
    tx.setTxFilter(300, 2700);
    a.send("aethersdr_tx_filter_band:0;");
    settle();
    check(a.take().isEmpty(), "and the item neither notifies nor answers");
    backend->connected = false;
    backend->caps.hasTxFilterControls = true;
    check(TciTxPassbandExtensionTest::offeredTokens(server).isEmpty(),
          "a disconnected radio is not offered it");
}

// Two receivers: moving TX re-sends the state line with the new trx, because a
// set is only taken for the trx that transmits and the line is how a client
// learns it.
void txMove()
{
    RadioModel model;
    auto owner = std::make_unique<FilterBackend>();
    FilterBackend* backend = owner.get();
    backend->caps.hasTxFilterControls = true;
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
    SliceDelta on;
    on.txSlice = true;
    SliceDelta off;
    off.txSlice = false;
    s0->applyChanges(on);
    TransmitModel& tx = model.transmitModel();
    tx.setTxFilter(100, 2900);
    TciServer server(&model);
    server.wireSlice(s0->sliceId(), s0);
    server.wireSlice(s1->sliceId(), s1);
    Wire a;  // enabled
    Wire b;  // plain
    TciTxPassbandExtensionTest::attach(server, a.client, model);
    TciTxPassbandExtensionTest::attach(server, b.client, model);
    settle(600);  // past wireSlice()'s 400 ms seeds
    a.take();
    b.take();
    a.send("aethersdr_ext_enable:true;");
    check(a.take() == QStringList({QStringLiteral("aethersdr_ext:1,tx_filter_band;"),
                                   QStringLiteral("aethersdr_tx_filter_band:0,100,2900;")}),
          "with TX on trx 0 the enable reply names trx 0");

    // The radio reports the move: B gains TX, then A loses it.
    s1->applyChanges(on);
    s0->applyChanges(off);
    settle(600);  // past kSplitSettleMs (450 ms)
    const QStringList heard = extensionLines(a.take());
    check(heard == one("aethersdr_tx_filter_band:1,100,2900;"),
          "moving TX to trx 1 sends the enabled client the line for trx 1, unasked");
    if (heard != one("aethersdr_tx_filter_band:1,100,2900;")) {
        for (const QString& line : heard) {
            std::printf("      a< %s\n", qPrintable(line));
        }
    }
    check(extensionLines(b.take()).isEmpty(), "and the plain client hears no aethersdr_ line");

    const int requests = backend->filterRequests.size();
    a.send("aethersdr_tx_filter_band:0,300,2400;");
    settle();
    check(a.take().isEmpty() && passbandIs(tx, 100, 2900)
              && backend->filterRequests.size() == requests,
          "a set on the old trx 0 is dropped");
    a.send("aethersdr_tx_filter_band:1,300,2400;");
    settle();
    check(passbandIs(tx, 300, 2400) && backend->filterRequests.size() == requests + 1
              && extensionLines(a.take()) == one("aethersdr_tx_filter_band:1,300,2400;")
              && extensionLines(b.take()).isEmpty(),
          "a set on the new trx 1 is applied and notified with trx 1");
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-tci-ext-tx-passband-test"));
    DaxIqModel::setPactlRunnerForTest([](const QStringList&) { return quint32{0}; });
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    check(settingsProfile.isValid(), "isolated settings profile");
    offeredPerBackend();
    normalisation();
    serverBehaviour();
    txMove();

    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
