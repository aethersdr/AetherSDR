// microHAM TRIO model tests: the stored address, the session words the
// Peripherals page shows, an eviction that is never taken back, the release
// to the Trio's own web page, device text kept out of log lines as control
// characters, and the proof that nothing but the handshake, keep-alives and a
// close frame ever leaves. The device is a
// scripted transport and a hand-moved clock (tests/MicrohamTrioTestPeer.h):
// no socket, no event loop. The settings store is a private temporary profile.
//
// Run:  ./build/microham_trio_model_test

#include "MicrohamTrioTestPeer.h"
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/MicrohamTrioSettings.h"
#include "core/PeripheralSettings.h"
#include "models/MicrohamTrioModel.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <string>

using namespace AetherSDR;
using TrioTestPeer::Peer;
using TrioTestPeer::clientText;
using TrioTestPeer::splitDoc;
using TrioTestPeer::stateDoc;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-74s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) {
        ++g_failed;
    }
}

using SessionWord = MicrohamTrioModel::SessionWord;

const char* const kHost = "192.0.2.10";

struct Rig {
    Peer peer;
    MicrohamTrioModel model;
    int connects{0};
    int disconnects{0};
    QStringList failures;

    Rig()
        : model(peer.environment())
    {
        QObject::connect(&model, &MicrohamTrioModel::connected, [this]() { ++connects; });
        QObject::connect(&model, &MicrohamTrioModel::disconnected, [this]() { ++disconnects; });
        QObject::connect(&model, &MicrohamTrioModel::connectionFailed,
                         [this](const QString& error) { failures.append(error); });
    }
    ~Rig() { MicrohamTrioSettings::remove(); }

    SessionWord word() const { return model.view().session; }

    // Nothing the model sent is a command: the handshake, keep-alives and a
    // close frame only, checked on the bytes.
    bool sentOnlyReads() const
    {
        for (const QByteArray& bytes : peer.transport.sent) {
            if (bytes.startsWith("GET /")) {
                continue;   // the page request and the upgrade
            }
            if (static_cast<quint8>(bytes.at(0)) == 0x88) {
                continue;   // close
            }
            if (clientText(bytes) != QByteArrayLiteral("ping")) {
                return false;   // anything else would be a command
            }
        }
        return true;
    }
};

void testSettings()
{
    MicrohamTrioSettings::remove();
    report("nothing stored: not configured", !MicrohamTrioSettings::load().isConfigured());
    MicrohamTrioSettings saved;
    saved.host = QStringLiteral(" 192.168.8.10 ");
    saved.port = 8080;
    saved.host = saved.host.trimmed();
    saved.save();
    report("the address is Peripherals.trio.ManualIp",
           PeripheralSettings::deviceString(QStringLiteral("trio"), QStringLiteral("ManualIp"))
               == QStringLiteral("192.168.8.10"));
    report("the port is Peripherals.trio.ManualPort",
           PeripheralSettings::deviceInt(QStringLiteral("trio"), QStringLiteral("ManualPort"), 0)
               == 8080);
    const MicrohamTrioSettings loaded = MicrohamTrioSettings::load();
    report("and they read back", loaded.host == QStringLiteral("192.168.8.10") && loaded.port == 8080);
    PeripheralSettings::setDeviceInt(QStringLiteral("trio"), QStringLiteral("ManualPort"), 70000);
    report("a port out of range reads as the default",
           MicrohamTrioSettings::load().port == MicrohamTrioSettings::kDefaultPort);
    PeripheralSettings::setAutoConnect(QStringLiteral("trio"), false);
    MicrohamTrioSettings::remove();
    report("remove clears the address and leaves Connect automatically to the row",
           !MicrohamTrioSettings::load().isConfigured()
               && !PeripheralSettings::autoConnect(QStringLiteral("trio")));
    PeripheralSettings::resetAutoConnect(QStringLiteral("trio"));
}

void testInertUntilConnect()
{
    Rig rig;
    rig.model.loadSettings(true);
    rig.peer.advance(rig.model, 60000);
    report("not configured: no session, nothing opened",
           !rig.model.hasSession() && rig.peer.transport.opens == 0
               && rig.word() == SessionWord::NotConfigured);

    MicrohamTrioSettings saved;
    saved.host = QLatin1String(kHost);
    saved.save();
    rig.model.loadSettings(false);
    rig.peer.advance(rig.model, 60000);
    report("configured with Connect automatically off: Off, nothing opened",
           !rig.model.hasSession() && rig.peer.transport.opens == 0
               && rig.word() == SessionWord::Off && rig.model.description().isEmpty());
}

void testConnectAndState()
{
    Rig rig;
    rig.model.connectNetwork(QLatin1String(kHost), 80);
    report("connect stores the address",
           MicrohamTrioSettings::load().host == QLatin1String(kHost));
    report("and is connecting", rig.word() == SessionWord::Connecting && rig.model.isConnecting()
               && rig.model.description() == QStringLiteral("192.0.2.10:80"));
    report("held with the state on connect", rig.peer.connectHeld(rig.model, "radio2", "ant6"));
    const MicrohamTrioModel::View view = rig.model.view();
    report("Direct, one connected()", view.session == SessionWord::Direct && rig.model.isConnected()
               && rig.connects == 1);
    report("the state is the one the Trio sent, current",
           view.snapshot && view.stateCurrent && view.snapshot->radio.selected == QStringLiteral("radio2")
               && view.snapshot->antennaTx == QStringLiteral("ant6"));
    const QString text = MicrohamTrioModel::describeState(*view.snapshot);
    report("the state in words names the focus and the antenna with their labels",
           text.contains(QStringLiteral("Radio with focus: R2 “HL2”"))
               && text.contains(QStringLiteral("Antenna: A6 “Dummy”")),
           text.toStdString());
    report("and lists every port's label",
           text.contains(QStringLiteral("A1 EFHW")) && text.contains(QStringLiteral("A6 Dummy"))
               && text.contains(QStringLiteral("R1 FT710")));

    rig.peer.hold(rig.model, 3000);
    rig.peer.push(stateDoc("radio1", "ant2"));
    report("a front-panel push moves the shown state",
           rig.model.view().snapshot->radio.selected == QStringLiteral("radio1")
               && rig.model.view().snapshot->antennaTx == QStringLiteral("ant2"));
    rig.peer.push(splitDoc("radio2", "ant1", "ant2"));
    const QString split = MicrohamTrioModel::describeState(*rig.model.view().snapshot);
    report("split is shown with both halves",
           split.contains(QStringLiteral("TX A1")) && split.contains(QStringLiteral("RX A2"))
               && split.contains(QStringLiteral("split")), split.toStdString());
    rig.peer.hold(rig.model, 60000);
    report("an hour's worth of minute: still held, keep-alives only",
           rig.word() == SessionWord::Direct && rig.sentOnlyReads());

    rig.model.disconnect();
    report("disconnect: Off, address kept, one disconnected()",
           rig.word() == SessionWord::Off && rig.disconnects == 1
               && MicrohamTrioSettings::load().host == QLatin1String(kHost));
    report("the last state is kept, marked not current",
           rig.model.view().snapshot && !rig.model.view().stateCurrent);
    report("disconnect sent a close frame and nothing that selects",
           !rig.peer.transport.sent.isEmpty()
               && static_cast<quint8>(rig.peer.transport.sent.last().at(0)) == 0x88
               && rig.sentOnlyReads());
}

void testEvictionIsNotRetaken()
{
    Rig rig;
    rig.model.connectNetwork(QLatin1String(kHost), 80);
    rig.peer.connectHeld(rig.model);
    rig.peer.hold(rig.model, 2000);
    const int opens = rig.peer.transport.opens;
    rig.peer.transport.peerCloses();
    report("another client takes the session: Evicted, no session held",
           rig.word() == SessionWord::Evicted && !rig.model.hasSession());
    report("reported once as a failure, and as a disconnect",
           rig.failures.size() == 1 && rig.disconnects == 1,
           rig.failures.isEmpty() ? std::string() : rig.failures.first().toStdString());
    rig.peer.advance(rig.model, 3600000);
    report("an hour later nothing has reconnected", rig.peer.transport.opens == opens
               && rig.word() == SessionWord::Evicted);
    rig.model.connectNetwork(QLatin1String(kHost), 80);
    report("Connect takes it back", rig.peer.connectHeld(rig.model)
               && rig.word() == SessionWord::Direct && rig.connects == 2);
}

void testRelease()
{
    Rig rig;
    rig.model.connectNetwork(QLatin1String(kHost), 80);
    rig.peer.connectHeld(rig.model);
    const int opens = rig.peer.transport.opens;
    rig.model.releaseForBrowser();
    report("release: the session is closed and the word is Released",
           !rig.model.hasSession() && rig.word() == SessionWord::Released && rig.disconnects == 1);
    rig.peer.advance(rig.model, 600000);
    report("and nothing reconnects", rig.peer.transport.opens == opens);
    rig.model.connectNetwork(QLatin1String(kHost), 80);
    report("Connect takes it back", rig.peer.connectHeld(rig.model) && rig.word() == SessionWord::Direct);
}

void testFailures()
{
    {
        Rig rig;
        rig.model.connectNetwork(QLatin1String(kHost), 80);
        rig.peer.advance(rig.model, 1);
        rig.peer.transport.breaks();
        report("reconnect off: a failed attempt is Offline and not retried",
               rig.word() == SessionWord::Offline && !rig.model.hasSession()
                   && rig.failures.size() == 1);
        const int opens = rig.peer.transport.opens;
        rig.peer.advance(rig.model, 600000);
        report("nothing reconnects", rig.peer.transport.opens == opens);
    }
    {
        Rig rig;
        rig.model.setAutoReconnect(true);
        rig.model.connectNetwork(QLatin1String(kHost), 80);
        rig.peer.connectHeld(rig.model);
        rig.peer.hold(rig.model, 2000);
        rig.peer.transport.breaks();
        report("reconnect on: a lost connection is Offline, still trying",
               rig.word() == SessionWord::Offline && rig.model.hasSession()
                   && rig.disconnects == 1);
        const int opens = rig.peer.transport.opens;
        rig.peer.advance(rig.model, 5000);
        report("and retries after the backoff", rig.peer.transport.opens == opens + 1);
        report("which holds again", rig.peer.handshakeWithState()
                   && rig.word() == SessionWord::Direct && rig.connects == 2);
    }
    {
        // A Trio that stops answering is a failed attempt: with Reconnect
        // automatically off it is closed, not reopened.
        Rig rig;
        rig.model.connectNetwork(QLatin1String(kHost), 80);
        rig.peer.connectHeld(rig.model);
        rig.peer.hold(rig.model, 2000);
        rig.peer.autoPong = false;
        const int opens = rig.peer.transport.opens;
        rig.peer.advance(rig.model, 30000);
        report("reconnect off: a silent session is Offline, closed, not reopened",
               rig.word() == SessionWord::Offline && !rig.model.hasSession()
                   && rig.peer.transport.opens == opens && rig.failures.size() == 1
                   && rig.failures.first().contains(QStringLiteral("stopped answering"))
                   && rig.disconnects == 1,
               "opens=" + std::to_string(rig.peer.transport.opens - opens)
                   + " failures=" + std::to_string(rig.failures.size()));
        rig.peer.advance(rig.model, 600000);
        report("and nothing reconnects later", rig.peer.transport.opens == opens);
    }
    {
        Rig rig;
        rig.model.setAutoReconnect(true);
        rig.model.connectNetwork(QLatin1String(kHost), 80);
        rig.peer.connectHeld(rig.model);
        rig.peer.hold(rig.model, 2000);
        rig.peer.autoPong = false;
        const int opens = rig.peer.transport.opens;
        for (int i = 0; i < 300 && rig.peer.transport.opens == opens; ++i) {
            rig.peer.advance(rig.model, 100);
        }
        report("reconnect on: a silent session is reopened",
               rig.model.hasSession() && rig.peer.transport.opens == opens + 1
                   && rig.failures.size() == 1,
               "opens=" + std::to_string(rig.peer.transport.opens - opens));
        rig.peer.autoPong = true;
        report("which holds again", rig.peer.handshakeWithState()
                   && rig.word() == SessionWord::Direct && rig.connects == 2);
    }
    {
        Rig rig;
        rig.model.connectNetwork(QLatin1String(kHost), 80);
        rig.peer.connectHeld(rig.model);
        rig.peer.push(QByteArrayLiteral(R"({"s":{},"id":"x"})"));
        report("a state this version cannot read fails the session, with words",
               rig.word() == SessionWord::Offline && rig.failures.size() == 1
                   && rig.failures.first().contains(QStringLiteral("this version")));
    }
}

void testTestConnection()
{
    {
        Rig rig;
        QStringList reports;
        bool reachable = false;
        QObject::connect(&rig.model, &MicrohamTrioModel::testFinished,
                         [&](bool ok, const QString& report) { reachable = ok; reports << report; });
        rig.model.testConnection(QLatin1String(kHost), 80);
        report("a test of an unsaved address takes a session", rig.model.isTesting()
                   && rig.model.hasSession() && rig.word() == SessionWord::NotConfigured);
        rig.peer.connectHeld(rig.model, "radio2", "ant6");
        report("the state arrives: reachable, one answer",
               reachable && reports.size() == 1 && !rig.model.isTesting());
        report("the answer names the focus, RADIO labels and antenna labels",
               !reports.isEmpty() && reports.first().contains(QStringLiteral("Reachable"))
                   && reports.first().contains(QStringLiteral("R2 HL2"))
                   && reports.first().contains(QStringLiteral("A6 Dummy")),
               reports.isEmpty() ? std::string() : reports.first().toStdString());
        report("and the session is released with a close frame",
               !rig.model.hasSession() && !rig.peer.transport.sent.isEmpty()
                   && static_cast<quint8>(rig.peer.transport.sent.last().at(0)) == 0x88);
        report("nothing was stored, nothing selected",
               !MicrohamTrioSettings::load().isConfigured() && rig.sentOnlyReads());
        const int opens = rig.peer.transport.opens;
        rig.peer.advance(rig.model, 600000);
        report("and nothing reconnects", rig.peer.transport.opens == opens);
    }
    {
        Rig rig;
        QStringList reports;
        bool reachable = true;
        QObject::connect(&rig.model, &MicrohamTrioModel::testFinished,
                         [&](bool ok, const QString& report) { reachable = ok; reports << report; });
        rig.model.testConnection(QLatin1String(kHost), 80);
        rig.peer.advance(rig.model, 1);
        rig.peer.transport.breaks();
        report("no answer: not reachable, with the reason",
               !reachable && reports.size() == 1 && reports.first().contains(QStringLiteral("Not reachable"))
                   && !rig.model.hasSession() && !rig.model.isTesting(),
               reports.isEmpty() ? std::string() : reports.first().toStdString());
        rig.model.testConnection(QString(), 80);
        report("no address: says so", reports.size() == 2 && !reports.last().isEmpty());
    }
    {
        Rig rig;
        QStringList reports;
        QObject::connect(&rig.model, &MicrohamTrioModel::testFinished,
                         [&](bool, const QString& report) { reports << report; });
        rig.model.connectNetwork(QLatin1String(kHost), 80);
        rig.peer.connectHeld(rig.model);
        const int opens = rig.peer.transport.opens;
        rig.model.testConnection(QLatin1String(kHost), 80);
        report("already connected: answered from the held session, no second connect",
               reports.size() == 1 && rig.peer.transport.opens == opens
                   && rig.word() == SessionWord::Direct);
    }
    {
        // A Test cancelled from the row (its Connect button reads Cancel while
        // the test runs): answered, and the next Connect is a Connect again.
        Rig rig;
        QStringList reports;
        bool reachable = true;
        QObject::connect(&rig.model, &MicrohamTrioModel::testFinished,
                         [&](bool ok, const QString& report) { reachable = ok; reports << report; });
        rig.model.connectNetwork(QLatin1String(kHost), 80);
        rig.peer.connectHeld(rig.model);
        rig.model.testConnection(QStringLiteral("192.168.8.99"), 80);
        const bool connectingDuringTest = rig.model.isConnecting() && rig.model.isTesting();
        rig.model.disconnect();
        report("cancel during a Test: testing cleared and answered",
               connectingDuringTest && !rig.model.isTesting() && reports.size() == 1 && !reachable,
               "connectingDuringTest=" + std::to_string(connectingDuringTest)
                   + " answers=" + std::to_string(reports.size()));
        rig.model.connectNetwork(QLatin1String(kHost), 80);
        const bool held = rig.peer.connectHeld(rig.model);
        report("then Connect holds the saved Trio's session, not a test",
               held && rig.model.hasSession() && reports.size() == 1
                   && rig.word() == SessionWord::Direct
                   && rig.peer.transport.lastHost == QLatin1String(kHost),
               "held=" + std::to_string(held) + " answers=" + std::to_string(reports.size())
                   + " host=" + rig.peer.transport.lastHost.toStdString());
    }
    {
        // Every other way the session is replaced ends the test the same way.
        Rig rig;
        int answers = 0;
        QObject::connect(&rig.model, &MicrohamTrioModel::testFinished,
                         [&](bool, const QString&) { ++answers; });
        rig.model.connectNetwork(QLatin1String(kHost), 80);
        rig.peer.connectHeld(rig.model);
        rig.model.testConnection(QStringLiteral("192.168.8.99"), 80);
        rig.model.releaseForBrowser();
        const bool release = !rig.model.isTesting() && answers == 1;
        rig.model.testConnection(QStringLiteral("192.168.8.99"), 80);
        rig.model.connectNetwork(QLatin1String(kHost), 80);
        const bool connect = !rig.model.isTesting() && answers == 2;
        rig.model.testConnection(QStringLiteral("192.168.8.99"), 80);
        rig.model.loadSettings(false);
        const bool load = !rig.model.isTesting() && answers == 3;
        rig.model.testConnection(QStringLiteral("192.168.8.99"), 80);
        rig.model.removeTrio();
        const bool remove = !rig.model.isTesting() && answers == 4;
        report("Web UI, Connect, a reload and Remove each end a Test, answered",
               release && connect && load && remove,
               "release=" + std::to_string(release) + " connect=" + std::to_string(connect)
                   + " load=" + std::to_string(load) + " remove=" + std::to_string(remove));
    }
}

void testRemove()
{
    Rig rig;
    rig.model.connectNetwork(QLatin1String(kHost), 80);
    rig.peer.connectHeld(rig.model);
    rig.model.removeTrio();
    report("remove: no session, not configured, nothing stored, no state",
           !rig.model.hasSession() && rig.word() == SessionWord::NotConfigured
               && !MicrohamTrioSettings::load().isConfigured() && !rig.model.view().snapshot);
    rig.model.connectNetwork(QString(), 80);
    report("connecting to an empty address is a remove", rig.word() == SessionWord::NotConfigured);
}

QStringList g_logged;

void captureLog(QtMsgType, const QMessageLogContext&, const QString& message)
{
    g_logged.append(message);
}

// A label is the device's text: a CR or an ESC in it must not reach a log
// line as itself, where it could overwrite or forge what a support bundle shows.
void testControlCharactersAreNotLogged()
{
    const QString mapped = MicrohamTrioModel::forLog(
        QStringLiteral("a\nb\rc\x1b[2Jd\te\x7f") + QChar(0x9b) + QStringLiteral("f"));
    report("forLog: newline as \" | \", every other control character as U+FFFD",
           mapped == QStringLiteral("a | b\uFFFDc\uFFFD[2Jd\uFFFDe\uFFFD\uFFFDf"),
           mapped.toStdString());

    QLoggingCategory::setFilterRules(QStringLiteral("aether.devices.info=true"));
    g_logged.clear();
    const QtMessageHandler previous = qInstallMessageHandler(captureLog);
    {
        Rig rig;
        rig.model.connectNetwork(QLatin1String(kHost), 80);
        rig.peer.advance(rig.model, 1);
        rig.peer.handshake(TrioTestPeer::textFrame(
            stateDoc("radio2", "ant6", TrioTestPeer::kZeroId, "HL2", "Dum\\rmy\\u001b[2J")));
        rig.peer.hold(rig.model, 1);
        rig.model.testConnection(QStringLiteral("192.0.2.77"), 8080);
    }
    qInstallMessageHandler(previous);
    QLoggingCategory::setFilterRules(QString());
    QString line;
    for (const QString& logged : g_logged) {
        if (logged.contains(QStringLiteral("state on connect"))) {
            line = logged;
        }
    }
    bool testTargetLogged = false;
    for (const QString& logged : g_logged) {
        testTargetLogged = testTargetLogged
            || (logged.contains(QStringLiteral("taking the session at"))
                && logged.contains(QStringLiteral("192.0.2.77:8080")));
    }
    report("a Test logs the address it takes the session at", testTargetLogged);
    report("a label with CR and ESC: the state-on-connect line carries neither",
           !line.isEmpty() && !line.contains(QLatin1Char('\r')) && !line.contains(QChar(0x1b))
               && line.contains(QStringLiteral("Dum\uFFFDmy\uFFFD[2J")),
           line.toStdString());
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("microham-trio-model"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    testSettings();
    testInertUntilConnect();
    testConnectAndState();
    testEvictionIsNotRetaken();
    testRelease();
    testFailures();
    testTestConnection();
    testRemove();
    testControlCharactersAreNotLogged();

    std::printf("\n%s (%d failed)\n", g_failed == 0 ? "ALL PASSED" : "FAILURES", g_failed);
    return g_failed == 0 ? 0 : 1;
}
