// microHAM TRIO model tests, read only: the stored address, the session words
// the Peripherals page shows, an eviction that is never taken back, the
// release to the Trio's own web page, and the proof that nothing but the
// handshake, keep-alives and a close frame ever leaves. The device is a
// scripted transport and a hand-moved clock (tests/MicrohamTrioTestPeer.h):
// no socket, no event loop. The settings store is a private temporary profile.
//
// Run:  ./build/microham_trio_model_test

#include "MicrohamTrioTestPeer.h"
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/MicrohamTrioSettings.h"
#include "core/PeripheralSettings.h"
#include "core/RadioSettingsScope.h"
#include "models/MicrohamTrioModel.h"

#include <QCoreApplication>
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

    // Everything the model has sent since the handshake, other than keep-alives
    // and a close frame: the read-only claim, checked on the bytes.
    bool sentOnlyReads() const
    {
        for (const QByteArray& bytes : peer.transport.sent) {
            if (bytes.startsWith("GET /")) {
                continue;   // the page request and the upgrade
            }
            if (static_cast<quint8>(bytes.at(0)) == 0x88) {
                continue;   // close
            }
            const QByteArray text = clientText(bytes);
            if (text != QByteArrayLiteral("ping") && !text.startsWith(R"({"a":"set_labels")")
                && !text.startsWith(R"({"a":"reset_labels")")) {
                return false;   // anything else would select something
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
        Rig rig;
        rig.model.connectNetwork(QLatin1String(kHost), 80);
        rig.peer.connectHeld(rig.model);
        rig.peer.push(QByteArrayLiteral(R"({"s":{},"id":"x"})"));
        report("a state this version cannot read fails the session, with words",
               rig.word() == SessionWord::Offline && rig.failures.size() == 1
                   && rig.failures.first().contains(QStringLiteral("this version")));
    }
}

void testBinding()
{
    Rig rig;
    using Stored = MicrohamTrioBinding::Stored;
    using Bind = MicrohamTrioModel::BindResult;
    const RadioSettingsScope hl2(QStringLiteral("hl2"), QStringLiteral("trio-test-radio-a"));
    const RadioSettingsScope other(QStringLiteral("hl2"), QStringLiteral("trio-test-radio-b"));
    MicrohamTrioBinding::remove(hl2);
    MicrohamTrioBinding::remove(other);

    report("no Trio configured: binding refused",
           rig.model.bindRadioSlot(QStringLiteral("radio2")) == Bind::NoTrio);
    rig.model.connectNetwork(QLatin1String(kHost), 80);
    rig.model.setRadio(RadioSettingsScope(QStringLiteral("hl2"), QString()));
    report("a family-only scope names no radio",
           rig.model.bindRadioSlot(QStringLiteral("radio2")) == Bind::NoRadioIdentity);
    rig.model.setRadio(hl2);
    report("no state yet: nothing to bind against",
           rig.model.bindRadioSlot(QStringLiteral("radio2")) == Bind::NoState);
    rig.peer.connectHeld(rig.model, "radio2", "ant6");
    report("a port the Trio does not have is refused",
           rig.model.bindRadioSlot(QStringLiteral("radio9")) == Bind::NoSuchPort);
    const int sentBefore = rig.peer.transport.sent.size();
    report("bind to radio2", rig.model.bindRadioSlot(QStringLiteral("radio2")) == Bind::Ok);
    const MicrohamTrioModel::View view = rig.model.view();
    report("stored with the label it carries now",
           view.binding == Stored::Bound && view.slotKey == QStringLiteral("radio2")
               && view.slotLabel == QStringLiteral("HL2") && view.bindingIssue.isEmpty());
    report("binding sent nothing to the Trio", rig.peer.transport.sent.size() == sentBefore);
    report("it reads back for this radio",
           MicrohamTrioBinding::load(hl2).stored == Stored::Bound
               && MicrohamTrioBinding::load(hl2).slotKey == QStringLiteral("radio2"));
    report("and another radio does not inherit it",
           MicrohamTrioBinding::load(other).stored == Stored::None);
    report("in words, with focus", MicrohamTrioModel::describeBinding(view).contains(
               QStringLiteral("radio2 \u201cHL2\u201d (has focus)")),
           MicrohamTrioModel::describeBinding(view).toStdString());

    rig.peer.push(stateDoc("radio2", "ant6", TrioTestPeer::kZeroId, "IC7300"));
    const MicrohamTrioModel::View relabelled = rig.model.view();
    report("a relabelled port is a label error naming both labels",
           relabelled.bindingIssue.contains(QStringLiteral("IC7300"))
               && relabelled.bindingIssue.contains(QStringLiteral("HL2")),
           relabelled.bindingIssue.toStdString());
    rig.peer.push(stateDoc("radio2", "ant6"));
    report("back to its label: no error", rig.model.view().bindingIssue.isEmpty());

    rig.model.setRadio(other);
    report("the other radio has said nothing", rig.model.view().binding == Stored::None);
    report("not on the Trio", rig.model.setNotOnTrio() == Bind::Ok
               && MicrohamTrioBinding::load(other).stored == Stored::NotOnTrio);
    report("forget", rig.model.clearBinding() == Bind::Ok
               && MicrohamTrioBinding::load(other).stored == Stored::None);

    rig.model.setRadio(hl2);
    rig.model.connectNetwork(QStringLiteral("192.0.2.99"), 80);
    report("bound against another Trio address: an issue",
           rig.model.view().bindingIssue.contains(QLatin1String(kHost)));
    MicrohamTrioBinding::remove(hl2);
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
}

void testLabels()
{
    using MicrohamTrio::LabelEdit;
    Rig rig;
    bool keyed = false;
    rig.model.setTransmitBlocked([&keyed]() { return keyed; });
    QStringList results;
    QObject::connect(&rig.model, &MicrohamTrioModel::labelsWritten,
                     [&results](bool ok, const QString& m) { results << (ok ? "ok:" : "fail:") + m; });
    const QVector<LabelEdit> rename = {{QStringLiteral("antenna"), QStringLiteral("ant1"), QStringLiteral("EFHW 80")}};
    report("not connected: renaming refused", !rig.model.writeLabels(rename).sent);
    rig.model.connectNetwork(QLatin1String(kHost), 80);
    rig.peer.connectHeld(rig.model, "radio2", "ant6");
    const RadioSettingsScope scope(QStringLiteral("hl2"), QStringLiteral("trio-label-radio"));
    rig.model.setRadio(scope);
    rig.model.bindRadioSlot(QStringLiteral("radio2"));
    rig.model.markDummyLoad(QStringLiteral("ant6"));
    report("the dummy load is marked with its label",
           rig.model.view().dummyLoadKey == QStringLiteral("ant6")
               && rig.model.view().dummyLoadLabel == QStringLiteral("Dummy")
               && rig.model.view().dummyLoadIssue.isEmpty());

    keyed = true;
    const MicrohamTrioModel::LabelVerdict whileKeyed = rig.model.writeLabels(rename);
    report("keyed: renaming refused, nothing sent",
           !whileKeyed.sent && whileKeyed.message.contains(QStringLiteral("transmitting"))
               && rig.sentOnlyReads());
    report("keyed: reset refused too", !rig.model.resetLabels(true).sent && rig.sentOnlyReads());
    keyed = false;

    report("an unchanged name is not sent",
           !rig.model.writeLabels({{QStringLiteral("antenna"), QStringLiteral("ant1"), QStringLiteral("EFHW")}}).sent);
    report("a name longer than the limit is refused",
           !rig.model.writeLabels({{QStringLiteral("antenna"), QStringLiteral("ant1"),
                                    QString(MicrohamTrio::kMaxLabelChars + 1, QLatin1Char('x'))}}).sent);
    report("a rename goes out", rig.model.writeLabels(rename).sent && rig.model.view().labelsPending);
    report("a second while the first is unanswered is refused", !rig.model.writeLabels(rename).sent);
    const QByteArray command = clientText(rig.peer.transport.sent.last());
    const QByteArray id = command.mid(command.indexOf("\"id\":\"") + 6, 36);
    rig.peer.push(stateDoc("radio2", "ant6", id).replace("\"l\":\"EFHW\"", "\"l\":\"EFHW 80\""));
    report("its reply confirms it", results.size() == 1 && results.last().startsWith("ok:"));

    const QVector<LabelEdit> dummyRename = {{QStringLiteral("antenna"), QStringLiteral("ant6"), QStringLiteral("Load")}};
    const MicrohamTrioModel::LabelVerdict unconfirmed = rig.model.writeLabels(dummyRename);
    report("renaming the dummy-load port needs a confirmation",
           !unconfirmed.sent && unconfirmed.needsDummyConfirmation);
    report("confirmed, it goes out", rig.model.writeLabels(dummyRename, true).sent);
    const QByteArray command2 = clientText(rig.peer.transport.sent.last());
    const QByteArray id2 = command2.mid(command2.indexOf("\"id\":\"") + 6, 36);
    rig.peer.push(stateDoc("radio2", "ant6", id2, "HL2", "Load"));
    report("and the mark follows the name AetherSDR wrote, with no warning",
           rig.model.view().dummyLoadLabel == QStringLiteral("Load")
               && rig.model.view().dummyLoadIssue.isEmpty());

    const QVector<LabelEdit> slotRename = {{QStringLiteral("radio"), QStringLiteral("radio2"), QStringLiteral("HL2 bench")}};
    rig.model.writeLabels(slotRename);
    const QByteArray command3 = clientText(rig.peer.transport.sent.last());
    const QByteArray id3 = command3.mid(command3.indexOf("\"id\":\"") + 6, 36);
    rig.peer.push(stateDoc("radio2", "ant6", id3, "HL2 bench", "Load"));
    report("renaming this AetherSDR's radio port keeps the binding, no label error",
           rig.model.view().slotLabel == QStringLiteral("HL2 bench")
               && rig.model.view().bindingIssue.isEmpty());

    rig.peer.push(stateDoc("radio2", "ant6", TrioTestPeer::kZeroId, "HL2 bench", "Spare"));
    report("a rename outside AetherSDR is a dummy-load warning",
           rig.model.view().dummyLoadIssue.contains(QStringLiteral("Spare")));
    report("confirm accepts it", rig.model.confirmDummyLoad() == MicrohamTrioModel::BindResult::Ok
               && rig.model.view().dummyLoadIssue.isEmpty());

    report("reset with a dummy load marked needs a confirmation",
           rig.model.resetLabels(false).needsDummyConfirmation);
    report("confirmed, reset_labels goes out", rig.model.resetLabels(true).sent
               && clientText(rig.peer.transport.sent.last()).startsWith(R"({"a":"reset_labels","id":")"));
    rig.peer.advance(rig.model, 2000);
    report("no reply in two seconds: reported, the state read again",
           !results.isEmpty() && results.last().startsWith("fail:") && !rig.model.view().labelsPending);
    MicrohamTrioBinding::remove(scope);
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
    testBinding();
    testTestConnection();
    testLabels();
    testRemove();

    std::printf("\n%s (%d failed)\n", g_failed == 0 ? "ALL PASSED" : "FAILURES", g_failed);
    return g_failed == 0 ? 0 : 1;
}
