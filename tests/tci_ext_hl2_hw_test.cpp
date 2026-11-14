// The HL2 hardware group of the opt-in TCI extension set (`aethersdr_hl2_*`,
// RFC #7181): offered only where the backend declares the `hl2` extension
// namespace, the filter board set through hw.set like the Radio Setup page and
// refused while transmitting, the IO board read answered to the asker alone,
// and the transmit timing readings taken from the health snapshot. Changes go
// to enabled clients only. Socket-free; nothing keys.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/TciClient.h"
#include "core/TciExtensionSet.h"
#include "core/TciHl2Extension.h"
#include "core/TciProtocol.h"
#include "core/TciServer.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/RadioCapabilities.h"
#include "core/backends/TransmitDelta.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "models/DaxIqModel.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <utility>

namespace AetherSDR {

class TciHl2ExtensionTest {
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
    static void checkReadings(TciServer& server) { server.checkHl2Readings(); }
    // A TCI key held, as trx:...,true; leaves it, without a socket.
    static void holdTciPtt(TciServer& server, TciClient* client)
    {
        server.m_tciPttClient = client;
    }
};

// RadioModel befriends this name; the identity is what a set is stored against.
struct RadioModelWakeTestAccess {
    static void identity(RadioModel& radio, const QString& serial)
    { radio.m_lastInfo.serial = serial; }
};

namespace hl2 {
// The real transmit tail, armed and ended by the backend's own functions.
struct Hl2HwSetTailTestAccess {
    static void setConnected(Hl2Backend& b) { b.m_connected = true; }
    static void beginTail(Hl2Backend& b)
    {
        b.m_txTailHoldMs = 60 * 60 * 1000;   // parked; the test ends it
        b.beginTxTail();
    }
    static void endTail(Hl2Backend& b)
    {
        b.m_txTailTimer->stop();
        b.finishTxTail();
    }
    static bool tailPending(const Hl2Backend& b) { return b.m_txTailPending; }
    static void setKeyed(Hl2Backend& b, bool v) { b.m_keyed = v; }
};
} // namespace hl2

namespace {

// Answers the `hl2` verbs the way Hl2Backend does: hw.get and hw.set
// synchronously (hw.set partial, with an `hl2`/`hw` status on a change),
// ioboard.status later or with an error, one at a time.
class Hl2FakeBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    bool connected{true};
    int filterBoard{0};
    bool n2adrHpf{false};
    int codec{0};
    QList<QVariantMap> hwSets;
    QList<quint64> ioRequests;
    bool ioPending{false};
    QVariantMap health;

    Hl2FakeBackend()
    {
        caps.canTransmit = true;
        caps.extensionNamespaces << QStringLiteral("hl2");
    }
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connected; }
    HealthSnapshot healthSnapshot() const override
    {
        HealthSnapshot h;
        for (auto it = health.cbegin(); it != health.cend(); ++it) {
            h.order << it.key();
            h.values.insert(it.key(), it.value());
        }
        return h;
    }
    QVariantMap hw() const
    {
        return {{QStringLiteral("codec"), codec},
                {QStringLiteral("filterBoard"), filterBoard},
                {QStringLiteral("n2adrHpf"), n2adrHpf}};
    }
    void invokeExtension(const QString& ns, const QString& verb, quint64 id,
                         const QVariant& arg) override
    {
        if (ns != QLatin1String("hl2")) {
            return;
        }
        if (verb == QLatin1String("hw.get")) {
            if (id != 0) {
                emit extensionResult(id, hw());
            }
        } else if (verb == QLatin1String("hw.set")) {
            const QVariantMap in = arg.toMap();
            hwSets << in;
            const QVariantMap before = hw();
            filterBoard = in.value(QStringLiteral("filterBoard"), filterBoard).toInt();
            n2adrHpf = in.value(QStringLiteral("n2adrHpf"), n2adrHpf).toBool();
            codec = in.value(QStringLiteral("codec"), codec).toInt();
            if (hw() != before) {
                emit extensionStatus(QStringLiteral("hl2"), QStringLiteral("hw"), hw());
            }
            if (id != 0) {
                emit extensionResult(id, hw());
            }
        } else if (verb == QLatin1String("ioboard.status") && id != 0) {
            if (!connected) {
                emit extensionError(id, QStringLiteral("Not connected to a radio."));
            } else if (ioPending) {
                emit extensionError(id, QStringLiteral("An IO board read is already pending."));
            } else {
                ioPending = true;
                ioRequests << id;
            }
        }
    }
    void answerIo(bool board)
    {
        if (ioRequests.isEmpty()) {
            return;
        }
        const quint64 id = ioRequests.takeLast();
        ioPending = false;
        if (!board) {
            emit extensionError(id, QStringLiteral("No IO board answered."));
            return;
        }
        emit extensionResult(id, QVariantMap{
            {QStringLiteral("inputPins"), 0x05},
            {QStringLiteral("receiving"), true},
            {QStringLiteral("in1"), false},
            {QStringLiteral("in2"), true},
            {QStringLiteral("in3"), false},
            {QStringLiteral("in4"), false},
            {QStringLiteral("in5"), false},
            {QStringLiteral("antennaTuner"), 0xEE},
            {QStringLiteral("fault"), 0},
            {QStringLiteral("firmwareMajor"), 2},
        });
    }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
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
    QStringList everything;  // every line ever received
    // The extension lines received since the last take. The standard lines
    // (trx:, tune:, ...) that the transmit state changes below produce are
    // not this group's and are left out.
    QStringList take()
    {
        QStringList out;
        for (const QString& line : std::as_const(rx)) {
            everything << line;
            if (line.startsWith(QLatin1String("aethersdr_"))) {
                out << line;
            }
        }
        rx.clear();
        return out;
    }
};

QStringList one(const char* line)
{
    return QStringList{QString::fromLatin1(line)};
}

void dump(const char* who, const QStringList& lines)
{
    for (const QString& line : lines) {
        std::printf("      %s< %s\n", who, qPrintable(line));
    }
}

// The real backends, as the production family switch builds them.
void offeredPerBackend()
{
    const struct {
        const char* family;
        bool expected;
    } cases[] = {{"hl2", true}, {"flex", false}, {"sim", false}, {"icom", false}};
    for (const auto& c : cases) {
        RadioModel model;
        const bool built = model.rebuildBackendForTest(QString::fromLatin1(c.family))
            && model.backend();
        const bool declares = built && model.backendDeclaresExtension(QStringLiteral("hl2"));
        std::printf("      %s: built=%d declares hl2=%d\n", c.family, built, declares);
        check(built && declares == c.expected,
              "a backend declares the hl2 namespace exactly when it is an HL2 backend");
    }
}

void grammar()
{
    using namespace TciHl2Extension;
    const auto parse = [](const char* args) {
        return parseFilterBoard(
            QString::fromLatin1(args).split(QLatin1Char(','), Qt::SkipEmptyParts));
    };
    check(parse("") && !parse("")->isSet && parse("N2ADR_TXONLY,1")
              && parse("N2ADR_TXONLY,1")->board == 2 && parse("N2ADR_TXONLY,1")->hpf
              && parse("none,false") && parse("none,false")->board == 0
              && !parse("none") && !parse("n2adr,true") && !parse("none,maybe")
              && !parse("none,true,1") && !parse("1,true"),
          "only a read or <board>,<bool> with a named board parses");
    check(boardToken(1) == QStringLiteral("n2adr_rxtx") && boardToken(3).isEmpty()
              && boardToken(-1).isEmpty(),
          "board numbers map to their names, and nothing else does");

    QVariantMap h;
    check(txTimingLine(h) == QStringLiteral("aethersdr_hl2_tx_timing:-1,-1,-1;")
              && extTrLine(h) == QStringLiteral("aethersdr_hl2_exttr:-1;"),
          "readings not read yet are -1");
    h.insert(QStringLiteral("pttHangTimeMs"), 31);
    h.insert(QStringLiteral("txBufferLatencyMs"), 10);
    check(txTimingLine(h) == QStringLiteral("aethersdr_hl2_tx_timing:31,false,10;"),
          "a PTT hang of 31 reads auto_unkey false");
    h.insert(QStringLiteral("pttHangTimeMs"), 4);
    check(txTimingLine(h) == QStringLiteral("aethersdr_hl2_tx_timing:4,true,10;"),
          "any other hang reads auto_unkey true");
    h.insert(QStringLiteral("paExtTr"), true);
    check(extTrLine(h) == QStringLiteral("aethersdr_hl2_exttr:-1;"),
          "external T/R without a reading age is not reported");
    h.insert(QStringLiteral("telemetryAgeMs"), qlonglong{800});
    check(extTrLine(h) == QStringLiteral("aethersdr_hl2_exttr:true;"),
          "a fresh external T/R reading is reported");
    h.insert(QStringLiteral("telemetryAgeMs"), qlonglong{kExtTrFreshMs + 1});
    check(extTrLine(h) == QStringLiteral("aethersdr_hl2_exttr:-1;"),
          "a stale one is not: it is a transmit state");
}

void serverBehaviour()
{
    RadioModel model;
    auto owner = std::make_unique<Hl2FakeBackend>();
    Hl2FakeBackend* backend = owner.get();
    backend->caps.extensionNamespaces.clear();
    model.setBackendForTest(std::move(owner), QStringLiteral("test"));
    int warnings = 0;
    QObject::connect(&model, &RadioModel::configurationWarning, &model,
                     [&warnings](const QString&) { ++warnings; });
    TciServer server(&model);
    Wire a;  // enabled
    Wire b;  // plain
    Wire c;  // enabled
    TciHl2ExtensionTest::attach(server, a.client, model);
    TciHl2ExtensionTest::attach(server, b.client, model);
    TciHl2ExtensionTest::attach(server, c.client, model);
    settle();

    // Gate: a connected radio without the namespace offers nothing.
    check(model.isConnected() && TciHl2ExtensionTest::offeredTokens(server).isEmpty(),
          "a radio that does not declare hl2 is not offered the group");
    a.send("aethersdr_ext_enable:true;");
    check(a.take() == one("aethersdr_ext:1;"), "its enable reply lists no item");
    a.send("aethersdr_hl2_filter_board;aethersdr_hl2_filter_board:n2adr_rxtx,false;"
           "aethersdr_hl2_ioboard_status;aethersdr_hl2_tx_timing;aethersdr_hl2_exttr;");
    settle();
    check(a.take().isEmpty() && backend->hwSets.isEmpty() && backend->ioRequests.isEmpty(),
          "a read or set for the group not offered gets no reply and writes nothing");

    backend->caps.extensionNamespaces << QStringLiteral("hl2");
    backend->health.insert(QStringLiteral("pttHangTimeMs"), 4);
    backend->health.insert(QStringLiteral("txBufferLatencyMs"), 10);
    emit model.capabilitiesChanged(true, backend->caps);
    const QStringList state{QStringLiteral("aethersdr_ext:1,hl2;"),
                            QStringLiteral("aethersdr_hl2_filter_board:none,false;"),
                            QStringLiteral("aethersdr_hl2_tx_timing:4,true,10;"),
                            QStringLiteral("aethersdr_hl2_exttr:-1;")};
    QStringList got = a.take();
    check(got == state, "once declared, the list offers hl2 with its three state lines");
    if (got != state) {
        dump("a", got);
    }
    c.send("aethersdr_ext_enable:true;");
    check(c.take() == state, "a client enabling later gets the same list and state");
    check(b.take().isEmpty(), "the plain client heard none of it");

    // Reads: to the requester only.
    a.send("aethersdr_hl2_filter_board;aethersdr_hl2_tx_timing;aethersdr_hl2_exttr;");
    check(a.take() == QStringList({QStringLiteral("aethersdr_hl2_filter_board:none,false;"),
                                   QStringLiteral("aethersdr_hl2_tx_timing:4,true,10;"),
                                   QStringLiteral("aethersdr_hl2_exttr:-1;")})
              && c.take().isEmpty(),
          "reads are answered to the requester only");
    a.send("aethersdr_hl2_tx_timing:31,false,10;aethersdr_hl2_exttr:true;"
           "aethersdr_hl2_filter_board:n2adr;aethersdr_hl2_filter_board:bogus,true;"
           "aethersdr_hl2_filter_board:none,maybe;aethersdr_hl2_ioboard_status:1;");
    settle();
    check(a.take().isEmpty() && backend->hwSets.isEmpty() && backend->ioRequests.isEmpty(),
          "sets of read-only items and malformed sets are ignored");

    // No radio identity: refused, as the setup page dims the controls.
    a.send("aethersdr_hl2_filter_board:n2adr_rxtx,false;");
    settle();
    check(backend->hwSets.isEmpty() && warnings == 1
              && a.take() == one("aethersdr_hl2_filter_board:none,false;") && c.take().isEmpty(),
          "without a radio identity a set is refused, answered to the sender and noticed");

    RadioInfo info;
    info.family = QStringLiteral("test");
    info.serial = QStringLiteral("00:00:00:00:00:01");
    info.address = QHostAddress(QStringLiteral("127.0.0.1"));
    model.connectToRadio(info);
    settle();
    a.take();
    c.take();
    check(!model.settingsScope().radioId().isEmpty() && model.isConnected(),
          "the radio has an identity");

    // Set: hw.set with only the board (HPF not applicable), then one notice.
    a.send("aethersdr_hl2_filter_board:N2ADR_RXTX,false;");
    check(backend->hwSets.size() == 1
              && backend->hwSets.first()
                     == QVariantMap({{QStringLiteral("filterBoard"), 1},
                                     {QStringLiteral("refuseWhileTransmitting"), true}}),
          "a set reaches the backend as the setup page's partial hw.set, asking for the "
          "transmit refusal");
    settle();
    check(a.take() == one("aethersdr_hl2_filter_board:n2adr_rxtx,false;")
              && c.take() == one("aethersdr_hl2_filter_board:n2adr_rxtx,false;")
              && b.take().isEmpty(),
          "the change is notified once to each enabled client, not to the plain one");

    // HPF: applies to the transmit-only board only.
    a.send("aethersdr_hl2_filter_board:n2adr_rxtx,true;");
    settle();
    check(backend->hwSets.size() == 1
              && a.take() == one("aethersdr_hl2_filter_board:n2adr_rxtx,false;")
              && c.take().isEmpty(),
          "an HPF on the RX/TX board is not applied: the sender gets what stands");
    a.send("aethersdr_hl2_filter_board:n2adr_txonly,1;");
    settle();
    check(backend->hwSets.size() == 2
              && backend->hwSets.last()
                     == QVariantMap({{QStringLiteral("filterBoard"), 2},
                                     {QStringLiteral("n2adrHpf"), true},
                                     {QStringLiteral("refuseWhileTransmitting"), true}})
              && a.take() == one("aethersdr_hl2_filter_board:n2adr_txonly,true;")
              && c.take() == one("aethersdr_hl2_filter_board:n2adr_txonly,true;"),
          "on the TX-only board the HPF is set with it and notified");
    a.send("aethersdr_hl2_filter_board:n2adr_txonly,true;");
    settle();
    check(backend->hwSets.size() == 2 && a.take().isEmpty() && c.take().isEmpty(),
          "a set that changes nothing stays quiet");

    // A toggling client: the first set is applied at once, the rest of the
    // window is coalesced into one hw.set with the newest.
    a.send("aethersdr_hl2_filter_board:none,false;aethersdr_hl2_filter_board:n2adr_rxtx,false;"
           "aethersdr_hl2_filter_board:none,false;aethersdr_hl2_filter_board:n2adr_rxtx,false;");
    check(backend->hwSets.size() == 3 && backend->filterBoard == 0,
          "the first set of a burst is applied at once");
    settle();
    check(backend->hwSets.size() == 4 && backend->filterBoard == 1,
          "the rest of the window is one hw.set, with the newest board");
    settle(250);  // the window the coalesced set opened
    a.take();
    c.take();
    a.send("aethersdr_hl2_filter_board:none,false;aethersdr_hl2_filter_board:n2adr_rxtx,false;"
           "aethersdr_hl2_filter_board:none,false;");
    settle(250);
    check(backend->hwSets.size() == 5 && backend->filterBoard == 0,
          "a held set followed by one equal to what stands is dropped, not applied");
    a.take();
    c.take();
    a.send("aethersdr_hl2_filter_board:n2adr_txonly,true;");
    settle(250);
    check(backend->hwSets.size() == 6 && backend->filterBoard == 2 && backend->n2adrHpf,
          "the board is back where the keyed checks expect it");
    a.take();
    c.take();

    // Keyed: refused before any hw.set, current value to the sender, noticed.
    const qsizetype setsBeforeKey = backend->hwSets.size();
    model.transmitModel().setTransmitting(true);
    a.send("aethersdr_hl2_filter_board:none,false;");
    settle();
    check(backend->hwSets.size() == setsBeforeKey && backend->filterBoard == 2 && warnings == 2
              && a.take() == one("aethersdr_hl2_filter_board:n2adr_txonly,true;")
              && c.take().isEmpty(),
          "a set while transmitting is refused, answered to the sender, and noticed");
    a.send("aethersdr_hl2_filter_board:none,false;aethersdr_hl2_filter_board:n2adr_rxtx,false;");
    c.send("aethersdr_hl2_filter_board:none,false;");
    settle();
    check(backend->hwSets.size() == setsBeforeKey && warnings == 2
              && a.take() == QStringList({QStringLiteral("aethersdr_hl2_filter_board:n2adr_txonly,true;"),
                                          QStringLiteral("aethersdr_hl2_filter_board:n2adr_txonly,true;")})
              && c.take() == one("aethersdr_hl2_filter_board:n2adr_txonly,true;"),
          "repeated refusals in one transmit state are each answered, and noticed once");
    model.transmitModel().setTransmitting(false);
    TransmitDelta tune;
    tune.tune = true;
    model.transmitModel().applyChanges(tune);
    a.send("aethersdr_hl2_filter_board:none,false;");
    settle();
    check(model.transmitModel().isTuning() && backend->hwSets.size() == setsBeforeKey && warnings == 3
              && a.take() == one("aethersdr_hl2_filter_board:n2adr_txonly,true;"),
          "and while tuning");
    tune.tune = false;
    model.transmitModel().applyChanges(tune);

    // A TCI client holding PTT is keyed too, before the radio confirms it.
    TciHl2ExtensionTest::holdTciPtt(server, b.client);
    a.send("aethersdr_hl2_filter_board:none,false;");
    settle();
    check(backend->hwSets.size() == setsBeforeKey && backend->filterBoard == 2
              && a.take() == one("aethersdr_hl2_filter_board:n2adr_txonly,true;"),
          "and while a TCI client holds PTT");
    TciHl2ExtensionTest::holdTciPtt(server, nullptr);

    // A change from elsewhere (the setup page writes with no reply expected).
    model.invokeBackendExtension(QStringLiteral("hl2"), QStringLiteral("hw.set"), 0,
                                 QVariantMap{{QStringLiteral("filterBoard"), 0}});
    settle();
    check(a.take() == one("aethersdr_hl2_filter_board:none,true;")
              && c.take() == one("aethersdr_hl2_filter_board:none,true;") && b.take().isEmpty(),
          "a setup page change reaches enabled clients, and not the plain client");
    model.invokeBackendExtension(QStringLiteral("hl2"), QStringLiteral("hw.set"), 0,
                                 QVariantMap{{QStringLiteral("codec"), 1}});
    settle();
    check(a.take().isEmpty() && c.take().isEmpty(),
          "a change that leaves the board and HPF as they were sends nothing");

    // IO board: to the asker alone, later; `none` on every error.
    a.send("aethersdr_hl2_ioboard_status;");
    check(backend->ioRequests.size() == 1 && a.take().isEmpty(),
          "an IO board read asks the backend and waits for it");
    c.send("aethersdr_hl2_ioboard_status;");
    check(c.take() == one("aethersdr_hl2_ioboard_status:none;"),
          "a second read while one is pending is answered none");
    backend->answerIo(true);
    check(a.take() == one("aethersdr_hl2_ioboard_status:5,true,false,true,false,false,false,"
                          "238,0,2;")
              && c.take().isEmpty() && b.take().isEmpty(),
          "the board's answer goes to the asker alone");
    a.send("aethersdr_hl2_ioboard_status;");
    backend->answerIo(false);
    check(a.take() == one("aethersdr_hl2_ioboard_status:none;"),
          "no board is answered none");
    b.send("aethersdr_hl2_ioboard_status;");
    check(backend->ioRequests.isEmpty() && b.take().isEmpty(),
          "a plain client's read never reaches the backend");
    {
        Wire gone;
        TciHl2ExtensionTest::attach(server, gone.client, model);
        gone.send("aethersdr_ext_enable:true;");
        gone.send("aethersdr_hl2_ioboard_status;");
        gone.take();
    }
    backend->answerIo(true);
    check(a.take().isEmpty() && c.take().isEmpty() && b.take().isEmpty(),
          "an answer for a client that left goes to no one");

    // Readings: a change is broadcast once to enabled clients.
    TciHl2ExtensionTest::checkReadings(server);
    a.take();
    c.take();
    backend->health.insert(QStringLiteral("pttHangTimeMs"), 31);
    TciHl2ExtensionTest::checkReadings(server);
    check(a.take() == one("aethersdr_hl2_tx_timing:31,false,10;")
              && c.take() == one("aethersdr_hl2_tx_timing:31,false,10;") && b.take().isEmpty(),
          "a PTT hang that moves to 31 is sent, auto_unkey false, to enabled clients");
    backend->health.insert(QStringLiteral("paExtTr"), true);
    backend->health.insert(QStringLiteral("telemetryAgeMs"), qlonglong{500});
    TciHl2ExtensionTest::checkReadings(server);
    check(a.take() == one("aethersdr_hl2_exttr:true;") && c.take() == one("aethersdr_hl2_exttr:true;"),
          "an external T/R reading is sent when it appears");
    TciHl2ExtensionTest::checkReadings(server);
    check(a.take().isEmpty() && c.take().isEmpty(), "an unchanged reading is not repeated");
    backend->health.insert(QStringLiteral("telemetryAgeMs"), qlonglong{60000});
    TciHl2ExtensionTest::checkReadings(server);
    check(a.take() == one("aethersdr_hl2_exttr:-1;") && c.take() == one("aethersdr_hl2_exttr:-1;"),
          "and withdrawn as -1 when it goes stale");

    // A client enabling on a newer reading brings the others up to date.
    c.send("aethersdr_ext_enable:false;");
    backend->health.insert(QStringLiteral("txBufferLatencyMs"), 20);
    c.send("aethersdr_ext_enable:true;");
    got = c.take();
    check(got.contains(QStringLiteral("aethersdr_hl2_tx_timing:31,false,20;")),
          "a client enabling gets the newest reading");
    settle(20);
    check(a.take() == one("aethersdr_hl2_tx_timing:31,false,20;"),
          "and the client that held the older one is sent it");
    c.take();

    // A plain or disabled client's command is dropped.
    c.send("aethersdr_ext_enable:false;");
    c.send("aethersdr_hl2_filter_board:n2adr_rxtx,false;aethersdr_hl2_tx_timing;");
    b.send("aethersdr_hl2_filter_board:n2adr_rxtx,false;aethersdr_hl2_tx_timing;");
    settle();
    check(backend->filterBoard == 0 && a.take().isEmpty() && b.take().isEmpty()
              && c.take().isEmpty(),
          "a plain or disabled client's command is dropped and writes nothing");
    b.take();
    check(std::none_of(b.everything.cbegin(), b.everything.cend(),
                       [](const QString& line) { return line.startsWith(QLatin1String("aethersdr_")); }),
          "the plain client never received an aethersdr_ line");

    // Lost namespace: the group leaves the list and answers nothing.
    backend->caps.extensionNamespaces.clear();
    emit model.capabilitiesChanged(true, backend->caps);
    check(a.take() == one("aethersdr_ext:1;"), "a lost namespace re-sends the list without it");
    a.send("aethersdr_hl2_filter_board;aethersdr_hl2_filter_board:n2adr_rxtx,false;");
    settle();
    check(a.take().isEmpty() && backend->filterBoard == 0,
          "and the group neither answers nor writes");
    backend->caps.extensionNamespaces << QStringLiteral("hl2");
    backend->connected = false;
    check(TciHl2ExtensionTest::offeredTokens(server).isEmpty(),
          "a disconnected radio is not offered it");
}

// The real HL2 backend tells every listener when a hw.set moves an option, and
// says nothing when it does not.
void realBackendNotifies()
{
    RadioModel model;
    if (!model.rebuildBackendForTest(QStringLiteral("hl2")) || !model.backend()) {
        check(false, "the HL2 backend builds");
        return;
    }
    QList<QVariantMap> statuses;
    QObject::connect(model.backend(), &IRadioBackend::extensionStatus, &model,
                     [&statuses](const QString& ns, const QString& kind, const QVariantMap& f) {
                         if (ns == QLatin1String("hl2") && kind == QLatin1String("hw")) {
                             statuses << f;
                         }
                     });
    model.invokeBackendExtension(QStringLiteral("hl2"), QStringLiteral("hw.set"), 0,
                                 QVariantMap{{QStringLiteral("filterBoard"), 2},
                                             {QStringLiteral("n2adrHpf"), true}});
    check(statuses.size() == 1
              && statuses.first().value(QStringLiteral("filterBoard")).toInt() == 2
              && statuses.first().value(QStringLiteral("n2adrHpf")).toBool(),
          "Hl2Backend reports a hw.set that moved the board, with the adopted options");
    model.invokeBackendExtension(QStringLiteral("hl2"), QStringLiteral("hw.set"), 0,
                                 QVariantMap{{QStringLiteral("filterBoard"), 2}});
    check(statuses.size() == 1, "and stays quiet on a hw.set that changed nothing");
    QVariantMap got;
    QObject guard;
    QObject::connect(model.backend(), &IRadioBackend::extensionResult, &guard,
                     [&got](quint64 id, const QVariant& v) {
                         if (id == 42) {
                             got = v.toMap();
                         }
                     });
    model.invokeBackendExtension(QStringLiteral("hl2"), QStringLiteral("hw.get"), 42);
    check(TciHl2Extension::filterBoardLine(got)
              == QStringLiteral("aethersdr_hl2_filter_board:n2adr_txonly,true;"),
          "and hw.get, which the state line reads, agrees with it");
}

// The HL2 keeps radiating for a tail after MOX-off, and no source TCI reads
// sees it. The real Hl2Backend refuses a hw.set that asks for the transmit
// refusal during the tail and accepts it once the tail ends; a hw.set without
// it (the setup page's) is unchanged. Then the same through TciServer.
void realBackendRefusesInTheTail()
{
    using hl2::Hl2HwSetTailTestAccess;
    RadioModel model;
    if (!model.rebuildBackendForTest(QStringLiteral("hl2")) || !model.backend()) {
        check(false, "the HL2 backend builds");
        return;
    }
    auto* backend = dynamic_cast<hl2::Hl2Backend*>(model.backend());
    if (!backend) {
        check(false, "the backend is an Hl2Backend");
        return;
    }
    int statuses = 0;
    QList<quint64> errors;
    QObject::connect(backend, &IRadioBackend::extensionStatus, &model,
                     [&statuses](const QString& ns, const QString& kind, const QVariantMap&) {
                         if (ns == QLatin1String("hl2") && kind == QLatin1String("hw")) {
                             ++statuses;
                         }
                     });
    QObject::connect(backend, &IRadioBackend::extensionError, &model,
                     [&errors](quint64 id, const QString&) { errors << id; });
    const auto hw = [&model, backend] {
        QVariantMap got;
        QObject guard;
        QObject::connect(backend, &IRadioBackend::extensionResult, &guard,
                         [&got](quint64 id, const QVariant& v) {
                             if (id == 99) {
                                 got = v.toMap();
                             }
                         });
        model.invokeBackendExtension(QStringLiteral("hl2"), QStringLiteral("hw.get"), 99);
        return got;
    };
    const auto board = [&hw] {
        return hw().value(QStringLiteral("filterBoard"), -1).toInt();
    };
    const auto set = [&model](quint64 id, int filterBoard, bool refuse) {
        QVariantMap in{{QStringLiteral("filterBoard"), filterBoard}};
        if (refuse) {
            in.insert(QStringLiteral("refuseWhileTransmitting"), true);
        }
        model.invokeBackendExtension(QStringLiteral("hl2"), QStringLiteral("hw.set"), id, in);
    };

    const int start = board();
    const int other = start == 1 ? 2 : 1;
    Hl2HwSetTailTestAccess::beginTail(*backend);
    check(Hl2HwSetTailTestAccess::tailPending(*backend), "the tail is pending after MOX-off");
    set(7, other, true);
    check(errors == QList<quint64>{7} && statuses == 0 && board() == start,
          "in the tail, a hw.set asking for the refusal is refused and changes nothing");
    set(0, other, false);
    check(statuses == 1 && board() == other,
          "in the tail, the setup page's hw.set is applied as before");
    set(0, start, false);
    statuses = 0;
    Hl2HwSetTailTestAccess::setKeyed(*backend, true);
    set(8, other, true);
    Hl2HwSetTailTestAccess::setKeyed(*backend, false);
    check(errors.size() == 2 && errors.last() == 8 && statuses == 0 && board() == start,
          "keyed, it is refused too");
    Hl2HwSetTailTestAccess::endTail(*backend);
    check(!Hl2HwSetTailTestAccess::tailPending(*backend), "the tail ends");
    set(9, other, true);
    check(errors.size() == 2 && statuses == 1 && board() == other,
          "after the tail it is accepted");
    set(0, start, false);

    // Through TciServer: a client's set in the tail is answered with what
    // stands and noticed; after the tail it is applied.
    Hl2HwSetTailTestAccess::setConnected(*backend);
    RadioModelWakeTestAccess::identity(model, QStringLiteral("00:00:00:00:00:02"));
    int warnings = 0;
    QObject::connect(&model, &RadioModel::configurationWarning, &model,
                     [&warnings](const QString&) { ++warnings; });
    TciServer server(&model);
    Wire a;
    TciHl2ExtensionTest::attach(server, a.client, model);
    settle();
    a.send("aethersdr_ext_enable:true;");
    a.take();
    const QString standing = TciHl2Extension::filterBoardLine(hw());
    const QString target = other == 1 ? QStringLiteral("aethersdr_hl2_filter_board:n2adr_rxtx,false;")
                                      : QStringLiteral("aethersdr_hl2_filter_board:n2adr_txonly,false;");
    std::printf("      start board %d, standing %s\n", start, qPrintable(standing));
    Hl2HwSetTailTestAccess::beginTail(*backend);
    a.send(qPrintable(target));
    settle();
    QStringList got = a.take();
    check(board() == start && warnings == 1 && got == QStringList{standing},
          "a TCI set in the tail is refused, answered with what stands, and noticed");
    if (got != QStringList{standing}) {
        dump("a", got);
    }
    Hl2HwSetTailTestAccess::endTail(*backend);
    settle(250);
    a.send(qPrintable(target));
    settle();
    got = a.take();
    check(board() == other && warnings == 1 && got == QStringList{target},
          "after the tail the same set is applied and notified");
    if (got != QStringList{target}) {
        dump("a", got);
    }
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-tci-ext-hl2-hw-test"));
    DaxIqModel::setPactlRunnerForTest([](const QStringList&) { return quint32{0}; });
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    check(settingsProfile.isValid(), "isolated settings profile");
    offeredPerBackend();
    grammar();
    serverBehaviour();
    realBackendNotifies();
    realBackendRefusesInTheTail();

    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
