// The Trio's antenna ports as station antennas of the connected radio: the
// view RadioModel hands the RX ANT / TX ANT menus, the one command a pick
// sends, and every refusal, above all while the radio may still radiate
// (keyed, MOX, tuning, CW key down, or the backend's own transmit tail). A
// socket-free backend and a scripted Trio (tests/MicrohamTrioTestPeer.h); a
// private settings profile.
//
// Run:  ./build/microham_trio_link_test

#include "MicrohamTrioTestPeer.h"
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/MicrohamTrioSettings.h"
#include "core/RadioSettingsScope.h"
#include "models/MicrohamTrioModel.h"
#include "models/MicrohamTrioRadioLink.h"
#include "models/StationAntenna.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>
#include <functional>
#include <memory>
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

class QuietBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    bool connected{true};
    bool tail{false};
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override { connected = true; }
    void disconnectRadio() override { connected = false; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
    bool transmitPathBusy() const override { return tail; }
};

const RadioSettingsScope kScope(QStringLiteral("icom"), QStringLiteral("trio-link-radio"));

struct Rig {
    RadioModel radio;
    QuietBackend* backend{nullptr};
    Peer peer;
    MicrohamTrioModel trio;
    std::unique_ptr<MicrohamTrioRadioLink> link;

    Rig()
        : trio(peer.environment())
    {
        auto owned = std::make_unique<QuietBackend>();
        backend = owned.get();
        backend->caps.family = QStringLiteral("icom");
        backend->caps.canTransmit = true;
        radio.setBackendForTest(std::move(owned), QStringLiteral("icom"));
        link = std::make_unique<MicrohamTrioRadioLink>(&radio, &trio);
        trio.connectNetwork(QStringLiteral("192.0.2.10"), 80);
        peer.connectHeld(trio, "radio2", "ant2");
        // The fake radio has no serial; name it as a real one would be.
        trio.setRadio(kScope);
        trio.bindRadioSlot(QStringLiteral("radio2"));
    }
    ~Rig()
    {
        MicrohamTrioBinding::remove(kScope);
        MicrohamTrioSettings::remove();
    }

    // How many antenna commands have left.
    int commands() const
    {
        int count = 0;
        for (const QByteArray& bytes : peer.transport.sent) {
            if (!bytes.startsWith("GET /") && clientText(bytes).contains("set_value")) {
                ++count;
            }
        }
        return count;
    }
    QByteArray lastCommand() const
    {
        for (qsizetype i = peer.transport.sent.size() - 1; i >= 0; --i) {
            const QByteArray text = clientText(peer.transport.sent.at(i));
            if (text.contains("set_value")) {
                return text;
            }
        }
        return {};
    }
    QByteArray lastId() const
    {
        const QByteArray command = lastCommand();
        const int at = command.indexOf("\"id\": \"");
        return command.mid(at + 7, 36);
    }
    // Answer the pending command with the state it asked for.
    void reply(const char* antenna) { peer.push(stateDoc("radio2", antenna, lastId())); }
    void replySplit(const char* tx, const char* rx) { peer.push(splitDoc("radio2", tx, rx, lastId())); }
};

const QString kTrio = QLatin1String(MicrohamTrioRadioLink::kSourceId);

// The Trio's entry in the registry, or an inactive view.
StationAntennaView trioViewOf(const RadioModel& radio)
{
    for (const StationAntennaView& view : radio.stationAntennaViews()) {
        if (view.id == kTrio) {
            return view;
        }
    }
    return {};
}

bool allEnabled(const StationAntennaView& view, bool enabled)
{
    for (const StationAntennaChoice& choice : view.tx) {
        if (choice.enabled != enabled) {
            return false;
        }
    }
    return !view.tx.isEmpty();
}

void testView()
{
    Rig rig;
    report("the radio is connected", rig.radio.isConnected());
    const StationAntennaView view = trioViewOf(rig.radio);
    report("bound: the Trio is a station device in the registry, six ports each way",
           view.active && view.heading == QStringLiteral("microHAM Trio") && view.rx.size() == 6
               && view.tx.size() == 6 && rig.radio.stationAntennaViews().size() == 1
               && view.unavailable.isEmpty());
    report("tokens and labels are the Trio's",
           view.tx.size() == 6 && view.tx.at(5).token == QStringLiteral("TRIO:ant6")
               && view.tx.at(5).label == QStringLiteral("Dummy"));
    report("the current port is shown in both fields",
           view.rxCurrent == QStringLiteral("TRIO:ant2") && view.txCurrent == QStringLiteral("TRIO:ant2")
               && view.txLabel == QStringLiteral("Fan Dipole"));
    report("every port can be picked", allEnabled(view, true));

    rig.trio.clearBinding();
    report("unbound: no source", !trioViewOf(rig.radio).active);
    rig.trio.bindRadioSlot(QStringLiteral("radio2"));
    rig.backend->connected = false;
    report("no radio connected: no source", !trioViewOf(rig.radio).active);
}

void testPick()
{
    Rig rig;
    report("a pick in TX ANT goes out",
           rig.radio.selectStationAntenna(kTrio, QStringLiteral("TRIO:ant6"), true).isEmpty());
    report("as one antenna command for ant6",
           rig.commands() == 1
               && rig.lastCommand().startsWith(
                   R"({"a": "set_value", "gid": "antenna", "v": "ant6", "id": ")"),
           rig.lastCommand().toStdString());
    const QString pending = rig.radio.selectStationAntenna(kTrio, QStringLiteral("TRIO:ant1"), false);
    report("a second pick before the answer is refused, and says why",
           !pending.isEmpty() && rig.commands() == 1, pending.toStdString());
    rig.reply("ant6");
    report("the reply moves both fields",
           trioViewOf(rig.radio).txCurrent == QStringLiteral("TRIO:ant6")
               && trioViewOf(rig.radio).rxCurrent == QStringLiteral("TRIO:ant6"));
    report("a token the view does not list is refused",
           !rig.radio.selectStationAntenna(kTrio, QStringLiteral("TRIO:ant9"), true).isEmpty()
               && !rig.radio.selectStationAntenna(kTrio, QStringLiteral("ANT1"), true).isEmpty()
               && rig.commands() == 1);
}

// Split is implied: RX other than TX turns it on, RX equal to TX turns it off.
void testImplicitSplit()
{
    Rig rig;
    QStringList notices;
    QObject::connect(&rig.trio, &MicrohamTrioModel::notice,
                     [&notices](const QString& n) { notices << n; });
    const StationAntennaView before = trioViewOf(rig.radio);
    report("the RX hint says a different port turns split on",
           before.rx.size() == 6 && before.rx.at(0).hint.contains(QStringLiteral("split on")),
           before.rx.isEmpty() ? std::string() : before.rx.at(0).hint.toStdString());
    report("RX pick EFHW while TX and RX are on Fan Dipole",
           rig.radio.selectStationAntenna(kTrio, QStringLiteral("TRIO:ant1"), false).isEmpty());
    report("first command: split on", rig.commands() == 1
               && rig.lastCommand().startsWith(R"({"a": "set_value", "gid": "split", "v": true, "id": ")"),
           rig.lastCommand().toStdString());
    rig.replySplit("ant2", "ant2");
    report("then the RX half, sent only after the first was confirmed", rig.commands() == 2
               && rig.lastCommand().startsWith(
                   R"({"a": "set_value", "gid": "antenna", "rxv": "rx_ant1", "id": ")"),
           rig.lastCommand().toStdString());
    rig.replySplit("ant2", "ant1");
    report("done: TX Fan Dipole, RX EFHW, nothing more sent",
           rig.commands() == 2 && !rig.trio.pickUnderWay()
               && trioViewOf(rig.radio).txCurrent == QStringLiteral("TRIO:ant2")
               && trioViewOf(rig.radio).rxCurrent == QStringLiteral("TRIO:ant1"));
    report("the TX hint in split keeps RX",
           trioViewOf(rig.radio).tx.at(2).hint.contains(QStringLiteral("RX stays on EFHW")),
           trioViewOf(rig.radio).tx.at(2).hint.toStdString());

    report("TX pick Antenna3 in split moves only the TX half",
           rig.radio.selectStationAntenna(kTrio, QStringLiteral("TRIO:ant3"), true).isEmpty()
               && rig.commands() == 3
               && rig.lastCommand().startsWith(
                   R"({"a": "set_value", "gid": "antenna", "txv": "tx_ant3", "id": ")"));
    rig.replySplit("ant3", "ant1");

    report("RX pick equal to TX ends split",
           rig.radio.selectStationAntenna(kTrio, QStringLiteral("TRIO:ant3"), false).isEmpty()
               && rig.commands() == 4
               && rig.lastCommand().startsWith(R"({"a": "set_value", "gid": "split", "v": false, "id": ")"));
    rig.reply("ant3");
    report("one command was enough: split off, TX and RX on Antenna3",
           rig.commands() == 4 && !rig.trio.pickUnderWay()
               && trioViewOf(rig.radio).rxCurrent == QStringLiteral("TRIO:ant3"));

    // Into split again, then a TX pick equal to the RX half ends it.
    rig.radio.selectStationAntenna(kTrio, QStringLiteral("TRIO:ant1"), false);
    rig.replySplit("ant3", "ant3");
    rig.replySplit("ant3", "ant1");
    const int sent = rig.commands();
    report("TX pick equal to the RX half ends split",
           rig.radio.selectStationAntenna(kTrio, QStringLiteral("TRIO:ant1"), true).isEmpty()
               && rig.commands() == sent + 1
               && rig.lastCommand().startsWith(R"({"a": "set_value", "gid": "split", "v": false)"));
    rig.reply("ant3");
    report("and when split off leaves the other port, `v` follows",
           rig.commands() == sent + 2
               && rig.lastCommand().startsWith(R"({"a": "set_value", "gid": "antenna", "v": "ant1")"),
           rig.lastCommand().toStdString());
    rig.reply("ant1");
    report("RX pick equal to TX without split sends nothing",
           rig.radio.selectStationAntenna(kTrio, QStringLiteral("TRIO:ant1"), false).isEmpty()
               && rig.commands() == sent + 2);

    // Keyed between two commands of one pick: the rest is not sent.
    rig.radio.selectStationAntenna(kTrio, QStringLiteral("TRIO:ant2"), false);
    const int midway = rig.commands();
    rig.radio.transmitModel().setTransmitting(true);
    rig.replySplit("ant1", "ant1");
    report("keyed between the commands of a pick: the next one is not sent, and it says so",
           rig.commands() == midway && !rig.trio.pickUnderWay() && !notices.isEmpty()
               && notices.last() == stationAntennaTransmitRefusal(),
           notices.isEmpty() ? std::string() : notices.last().toStdString());
    rig.radio.transmitModel().setTransmitting(false);

    // A port marked RX only: an RX half, never a TX port.
    Rig marked;
    QByteArray doc = stateDoc("radio2", "ant2");
    doc.replace(R"("l":"Antenna4","e":true)", R"("l":"Antenna4","e":false)");
    marked.peer.push(doc);
    report("an RX-only port cannot be picked for TX",
           !marked.radio.selectStationAntenna(kTrio, QStringLiteral("TRIO:ant4"), true).isEmpty()
               && marked.commands() == 0);
    report("but can be the RX half (split on)",
           marked.radio.selectStationAntenna(kTrio, QStringLiteral("TRIO:ant4"), false).isEmpty()
               && marked.commands() == 1);
}

// The registry holds one entry per device; a pick reaches only the device it
// names; an entry goes with its owner, and one registered again replaces it.
void testRegistry()
{
    RadioModel radio;
    auto owned = std::make_unique<QuietBackend>();
    owned->caps.family = QStringLiteral("icom");
    owned->caps.canTransmit = true;
    radio.setBackendForTest(std::move(owned), QStringLiteral("icom"));
    const auto device = [](const char* heading) {
        return [heading]() {
            StationAntennaView view;
            view.active = true;
            view.heading = QLatin1String(heading);
            StationAntennaChoice choice;
            choice.token = QStringLiteral("p1");
            choice.label = QStringLiteral("Port 1");
            choice.enabled = true;
            view.rx.append(choice);
            view.tx.append(choice);
            return view;
        };
    };
    QStringList picks;
    auto first = std::make_unique<QObject>();
    QObject second;
    radio.addStationAntennaSource(first.get(), QStringLiteral("a"), device("Switch A"),
        [&picks](const QString& token, bool) { picks << QStringLiteral("a:") + token; return QString(); });
    radio.addStationAntennaSource(&second, QStringLiteral("b"), device("Switch B"),
        [&picks](const QString& token, bool) { picks << QStringLiteral("b:") + token; return QString(); });
    const QVector<StationAntennaView> views = radio.stationAntennaViews();
    report("two devices: two entries, in registration order, each with its id",
           views.size() == 2 && views.at(0).id == QStringLiteral("a")
               && views.at(1).id == QStringLiteral("b")
               && views.at(1).heading == QStringLiteral("Switch B"));
    report("the same token on two devices reaches only the one named",
           radio.selectStationAntenna(QStringLiteral("b"), QStringLiteral("p1"), true).isEmpty()
               && picks == QStringList{QStringLiteral("b:p1")});
    report("a device not registered is refused",
           !radio.selectStationAntenna(QStringLiteral("c"), QStringLiteral("p1"), true).isEmpty()
               && picks.size() == 1);
    QObject replacement;
    radio.addStationAntennaSource(&replacement, QStringLiteral("b"), device("Switch B2"),
        [&picks](const QString& token, bool) { picks << QStringLiteral("b2:") + token; return QString(); });
    report("registered again under its id: replaced, not added",
           radio.stationAntennaViews().size() == 2
               && radio.stationAntennaViews().at(1).heading == QStringLiteral("Switch B2"));
    first.reset();
    report("an owner destroyed: its entry goes",
           radio.stationAntennaViews().size() == 1
               && radio.stationAntennaViews().at(0).id == QStringLiteral("b"));
    report("and the replacement still answers for its id",
           radio.selectStationAntenna(QStringLiteral("b"), QStringLiteral("p1"), false).isEmpty()
               && picks.last() == QStringLiteral("b2:p1"));
}

// The switch must not move under RF.
void testRefusedWhileTransmitting()
{
    struct Case {
        const char* name;
        std::function<void(Rig&)> set;
        std::function<void(Rig&)> clear;
    };
    const Case cases[] = {
        {"keyed (TransmitModel transmitting)",
         [](Rig& r) { r.radio.transmitModel().setTransmitting(true); },
         [](Rig& r) { r.radio.transmitModel().setTransmitting(false); }},
        {"in the backend's transmit tail after un-key",
         [](Rig& r) { r.backend->tail = true; },
         [](Rig& r) { r.backend->tail = false; }},
    };
    for (const Case& c : cases) {
        Rig rig;
        c.set(rig);
        const QString refusal = rig.radio.selectStationAntenna(kTrio, QStringLiteral("TRIO:ant6"), true);
        report((std::string("refused while ") + c.name + ", in the shared sentence").c_str(),
               refusal == QStringLiteral("Antenna can't change while transmitting.")
                   && rig.commands() == 0,
               refusal.toStdString());
        report((std::string("RX ANT too, while ") + c.name).c_str(),
               !rig.radio.selectStationAntenna(kTrio, QStringLiteral("TRIO:ant1"), false).isEmpty()
                   && rig.commands() == 0);
        report((std::string("and the ports are shown disabled while ") + c.name
                + ", with the same sentence").c_str(),
               allEnabled(trioViewOf(rig.radio), false)
                   && trioViewOf(rig.radio).tx.at(0).reason == stationAntennaTransmitRefusal());
        c.clear(rig);
        report((std::string("after ") + c.name + " ends: allowed again").c_str(),
               rig.radio.selectStationAntenna(kTrio, QStringLiteral("TRIO:ant6"), true).isEmpty()
                   && rig.commands() == 1);
    }
    // The link refuses on its own too: the last step before the command leaves.
    Rig rig;
    rig.backend->tail = true;
    report("the link itself refuses in the tail",
           !rig.link->selectAntenna(QStringLiteral("TRIO:ant6"), true).isEmpty()
               && rig.commands() == 0);
}

// RadioModel refuses on its own, before any source is asked: a source that
// accepts everything is never reached while the radio may radiate.
void testRadioModelRefusesOnItsOwn()
{
    RadioModel radio;
    auto owned = std::make_unique<QuietBackend>();
    QuietBackend* backend = owned.get();
    backend->caps.family = QStringLiteral("icom");
    backend->caps.canTransmit = true;
    radio.setBackendForTest(std::move(owned), QStringLiteral("icom"));
    QObject owner;
    int picks = 0;
    radio.addStationAntennaSource(&owner, QStringLiteral("sw"), []() {
        StationAntennaView view;
        view.active = true;
        view.heading = QStringLiteral("Test switch");
        StationAntennaChoice choice;
        choice.token = QStringLiteral("SW:1");
        choice.label = QStringLiteral("One");
        choice.enabled = true;
        view.rx.append(choice);
        view.tx.append(choice);
        view.txLabel = view.rxLabel = QStringLiteral("One");
        return view;
    }, [&picks](const QString&, bool) { ++picks; return QString(); });
    report("an accepting source is reached when nothing transmits",
           radio.selectStationAntenna(QStringLiteral("sw"), QStringLiteral("SW:1"), true).isEmpty() && picks == 1);
    radio.transmitModel().setTransmitting(true);
    report("transmitting: RadioModel refuses, the source is not asked",
           !radio.selectStationAntenna(QStringLiteral("sw"), QStringLiteral("SW:1"), true).isEmpty() && picks == 1);
    radio.transmitModel().setTransmitting(false);
    backend->tail = true;
    report("in the backend's tail: RadioModel refuses, the source is not asked",
           !radio.selectStationAntenna(QStringLiteral("sw"), QStringLiteral("SW:1"), false).isEmpty() && picks == 1);
    backend->tail = false;
    report("a token the source does not list is refused before it is asked",
           !radio.selectStationAntenna(QStringLiteral("sw"), QStringLiteral("SW:2"), true).isEmpty() && picks == 1);
}

void testRefusals()
{
    {
        Rig rig;
        rig.peer.push(stateDoc("radio1", "ant2"));
        const QString refusal = rig.radio.selectStationAntenna(kTrio, QStringLiteral("TRIO:ant6"), true);
        report("focus on another radio: refused, naming it",
               refusal.contains(QStringLiteral("FT710")) && rig.commands() == 0,
               refusal.toStdString());
        report("and the fields show that radio; the whole submenu says why",
               trioViewOf(rig.radio).txLabel.contains(QStringLiteral("FT710"))
                   && trioViewOf(rig.radio).txCurrent.isEmpty()
                   && trioViewOf(rig.radio).unavailable == refusal
                   && trioViewOf(rig.radio).tx.size() == 6);
    }
    {
        Rig rig;
        rig.peer.push(splitDoc("radio2", "ant1", "ant2"));
        report("split on at the Trio: a TX pick moves only the TX half",
               rig.radio.selectStationAntenna(kTrio, QStringLiteral("TRIO:ant6"), true).isEmpty()
                   && rig.commands() == 1
                   && rig.lastCommand().startsWith(
                       R"({"a": "set_value", "gid": "antenna", "txv": "tx_ant6", "id": ")"));
    }
    {
        Rig rig;
        rig.peer.push(stateDoc("radio2", "ant2", TrioTestPeer::kZeroId, "IC7300"));
        report("a relabelled radio port: refused as a label error",
               rig.radio.selectStationAntenna(kTrio, QStringLiteral("TRIO:ant6"), true)
                       .contains(QStringLiteral("label error"))
                   && rig.commands() == 0);
    }
    {
        Rig rig;
        rig.peer.transport.peerCloses();
        report("evicted: refused, the ports still listed, disabled, with the reason",
               !rig.radio.selectStationAntenna(kTrio, QStringLiteral("TRIO:ant6"), true).isEmpty()
                   && rig.commands() == 0 && trioViewOf(rig.radio).tx.size() == 6
                   && allEnabled(trioViewOf(rig.radio), false)
                   && trioViewOf(rig.radio).unavailable.contains(QStringLiteral("Another client")),
               trioViewOf(rig.radio).unavailable.toStdString());
    }
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("microham-trio-link"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    testView();
    testPick();
    testImplicitSplit();
    testRefusedWhileTransmitting();
    testRadioModelRefusesOnItsOwn();
    testRegistry();
    testRefusals();

    std::printf("\n%s (%d failed)\n", g_failed == 0 ? "ALL PASSED" : "FAILURES", g_failed);
    return g_failed == 0 ? 0 : 1;
}
