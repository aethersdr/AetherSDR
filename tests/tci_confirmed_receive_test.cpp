// Production TCI/model/backend RX requests and model-to-TCI readback relays.
// All transport writes and client text sinks are injected; no socket/RF/device.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/TciProtocol.h"
#include "core/RigctlProtocol.h"
#include "core/SmartCatProtocol.h"
#include "core/TciServer.h"
#include "core/TciClient.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/MeterModel.h"
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
using namespace AetherSDR;
namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& backend)
    {
        backend.m_clock.start();
        backend.m_protocol.start("fixture", "fixture", false, 0);
        backend.m_protocol.receive("##CN1;", 1);
        backend.m_protocol.receive("##ID1;", 2);
        backend.ready();
    }
    static void observe(KenwoodBackend& backend, const QByteArray& message) { backend.adopt(message); }
    static void maximum(KenwoodBackend& backend, const QString& variant)
    { backend.m_maximumFrequencyHz = *maximumFrequencyForVariant(variant); }
};
}
namespace AetherSDR {
class TciServerReviewTest {
public:
    static TciClient* capture(TciServer& server, RadioModel& model, QStringList& messages)
    {
        auto* client = new TciClient(&server); // Qt parent owns identity; no socket.
        client->textSink = [&messages](const QString& text) { messages.append(text); return text.size(); };
        auto protocol = std::make_unique<TciProtocol>(&model, &server.m_routingState, &server.m_trxMap);
        TciServer::ClientState state;
        state.socket = client;
        state.protocol = protocol.release(); // TciServer::stopIo owns registered protocols.
        state.rxSensorsEnabled = true;
        server.m_clients.append(state);
        QObject::connect(client, &TciClient::textMessageReceived, &server, &TciServer::onTextMessage);
        return client;
    }
    static TciProtocol& protocol(TciServer& server, TciClient* client)
    { return *server.clientStateFor(client)->protocol; }
    static void status(TciServer& server) { server.broadcastStatus(); }
    // Injected backends omit setupBackend's lifecycle capability relay. Exercise
    // its real publication endpoint without copying a receive-control relay.
    static void capabilities(RadioModel& model) { model.publishCapabilities(model.isConnected()); }
};
}
namespace {
using Access = kenwood::KenwoodBackendTestAccess;
void require(bool condition, const char* claim)
{
    if (!condition) { std::cerr << claim << '\n'; std::exit(1); }
}
void pump()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}
void spin(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}
bool contains(const QStringList& messages, const QString& command)
{
    for (const QString& message : messages) {
        if (message.split(';', Qt::SkipEmptyParts).contains(command.chopped(1))) { return true; }
    }
    return false;
}
bool meterMessage(const QStringList& messages)
{
    for (const QString& message : messages) {
        if (message.contains("rx_smeter:") || message.contains("rx_channel_sensors:")) { return true; }
    }
    return false;
}
struct Fixture {
    QStringList messages;
    QList<QByteArray> writes;
    bool accept = true;
    RadioModel model;
    TciServer server{&model};
    TciClient* client = TciServerReviewTest::capture(server, model, messages);
    kenwood::KenwoodBackend* backend = nullptr;
    Fixture(char mode = '3') { replace(mode); }
    void observe(const QByteArray& message) { Access::observe(*backend, message); }
    TciProtocol& protocol() { return TciServerReviewTest::protocol(server, client); }
    void replace(char mode = '3')
    {
        auto owned = std::make_unique<kenwood::KenwoodBackend>(kenwood::KenwoodBackend::Transport{
            [this](const QByteArray& command) { writes.append(command); return accept; },
            [](const QByteArray&) { return false; }});
        backend = owned.get();
        model.setBackendForTest(std::move(owned), "kenwood");
        Access::authenticate(*backend);
        for (const QByteArray& message : {QByteArray("ID024"), QByteArray("FR0"), QByteArray("FT0"),
             QByteArray("TS0"), QByteArray("SF000014074000") + mode, QByteArray("RX"), QByteArray("MO20"),
             QByteArray("SL006"), QByteArray("SH0080"), QByteArray("GC1"), QByteArray("NR0"),
             QByteArray("NB10"), QByteArray("NL1001"), QByteArray("BC0"), QByteArray("AG128"),
             QByteArray("MU0"), QByteArray("RT0"), QByteArray("RF00000"), QByteArray("SQ128"), QByteArray("AP00")}) { observe(message); }
        require(model.slice(0) && model.slice(0)->receiveControlsConfirmed(), "actual backend receiver creates confirmed model slice");
        // Same public state-relay binding MainWindow_Session uses, not a copied
        // slice control relay. RadioModel already wires actual typed requests.
        server.wireSlice(0, model.slice(0));
        writes.clear(); messages.clear();
    }
};
class OptimisticBackend final : public IRadioBackend {
public:
    bool connected = true;
    bool confirmed = false;
    QString requestedMode;
    ReceiveControlPolicy receiveControlPolicy() const override { return confirmed ? ReceiveControlPolicy::Confirmed : ReceiveControlPolicy::Optimistic; }
    QStringList modes;
    RadioCapabilities capabilities() const override {
        RadioCapabilities caps;
        if (!modes.isEmpty()) { caps.receiveModeControl = ReceiveModeControl{SliceFrequencyControl::Authority::Radio, modes}; }
        return caps;
    }
    void announce(const QString& mode = QStringLiteral("CW"), bool filterKnown = false)
    {
        SliceDelta state;
        state.inUse = true; state.active = true; state.mode = mode; state.panId = "0"; state.modeList = modes;
        if (filterKnown) { state.filterLow = -1000; state.filterHigh = 1000; }
        emit sliceChanged(0, state);
    }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override { connected = false; }
    bool isConnected() const override { return connected; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString& mode) override { requestedMode = mode; }
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};
void checkDeclaredModes()
{
    struct Mode { char code; QString native; QString hamlib; QString md; QString zz; };
    const QList<Mode> modes{
        {'1', "LSB", "LSB", "1", "00"}, {'2', "USB", "USB", "2", "01"},
        {'3', "CW", "CW", "3", "04"}, {'4', "FM", "FM", "4", "05"},
        {'5', "AM", "AM", "5", "06"}, {'6', "RTTY", "RTTY", "", "30"},
        {'7', "CWR", "CWR", "", "03"}, {'9', "RTTYR", "RTTYR", "", ""},
        {'A', "PSK", "PSK", "", ""}, {'B', "PSKR", "PSKR", "", ""},
        {'C', "DIGL", "PKTLSB", "6", "09"}, {'D', "DIGU", "PKTUSB", "9", "07"},
        {'E', "DFM", "PKTFM", "", "12"}, {'F', "DAM", "PKTAM", "", ""}};
    int mdSetterCases = 0;
    int zzSetterCases = 0;
    for (const Mode& mode : modes) {
        std::cerr << "MODE " << qPrintable(mode.native) << std::endl;
        Fixture f(mode.code);
        const bool sharedWidthKnown = mode.native == "LSB" || mode.native == "USB" || mode.native == "CW"
            || mode.native == "CWR" || mode.native == "DIGL" || mode.native == "DIGU";
        if (mode.native == "LSB" || mode.native == "USB" || mode.native == "DIGL" || mode.native == "DIGU") {
            f.observe("EX00611 000"); f.observe("EX00612 000"); f.observe("SL002"); f.observe("SH0024");
            f.writes.clear(); f.messages.clear();
        }
        RigctlProtocol rig(&f.model);
        SmartCatProtocol cat(&f.model);
        const QString token = mode.native.toLower();
        require(f.protocol().handleCommand("modulation:0") == "modulation:0," + token + ";",
            "native TCI mode GET is exact, never USB fallback");
        const QString burst = f.protocol().generateInitBurst();
        for (const Mode& advertised : modes) {
            require(burst.section("modulations_list:", 1).section(';', 0, 0).split(',').contains(advertised.native.toLower()),
                "all fourteen native modes are advertised exactly");
        }
        require(sharedWidthKnown ? rig.handleLine("m").startsWith(mode.hamlib + "\n") : rig.handleLine("m") == "RPRT -11\n",
            "native rigctl mode GET is exact only with actual shared width; unprojected native axes remain unavailable");
        require(rig.handleLine("\\get_modes").trimmed().split(' ', Qt::SkipEmptyParts).contains(mode.hamlib),
                "native rigctl list contains exact representable mode");
        require(cat.processCommand("MD") == (mode.md.isEmpty() ? "?;" : "MD" + mode.md + ";"),
                "SmartCAT MD refuses modes outside its established dialect");
        require(cat.processCommand("ZZMD") == (mode.zz.isEmpty() ? "?;" : "ZZMD" + mode.zz + ";"),
                "SmartCAT ZZMD encodes only documented mode equivalents");
        if (mode.md.isEmpty()) {
            require(cat.processCommand("IF") == "?;" && cat.processCommand("ZZIF") == "?;"
                && cat.processCommand("OI") == "?;", "composite status never fabricates USB for an unrepresentable mode");
        }
        // A setter starts in another known mode so actual backend dispatch,
        // rather than a same-value early return, proves the reverse mapping.
        const char other = mode.code == '3' ? '2' : '3';
        const QByteArray otherState = QByteArray("SF000014074000") + other;
        const QByteArray actualState = QByteArray("SF000014074000") + mode.code;
        f.observe(otherState);
        f.writes.clear(); f.messages.clear();
        f.protocol().handleCommand("modulation:0," + token); pump();
        require(f.writes.contains(QByteArray("OM0") + mode.code + ';'), "native TCI setter reaches exact CAT mode code");
        require(!contains(f.messages, "modulation:0," + token + ";"), "mode SET does not fabricate readback");
        f.observe(actualState);
        require(contains(f.messages, "modulation:0," + token + ";"), "real TCI mode callback uses the same exact vocabulary");
        f.observe(otherState); f.writes.clear();
        require(rig.handleLine("M " + mode.hamlib + " 500") == "RPRT -8\n",
            "native mode plus passband is refused before changing an unobserved filter bank");
        pump(); require(f.writes.isEmpty(), "refused inseparable mode passband sends no partial mode write");
        require(rig.handleLine("M " + mode.hamlib + " 0") == "RPRT 0\n", "native rigctl setter accepted declared mode");
        pump();
        require(f.writes.contains(QByteArray("OM0") + mode.code + ';'), "native rigctl setter preserves reverse/digital identity");
        f.observe(actualState);
        for (const bool extended : {false, true}) {
            const QString code = extended ? mode.zz : mode.md;
            if (code.isEmpty()) { continue; }
            const QString prefix = extended ? QStringLiteral("ZZMD") : QStringLiteral("MD");
            const QString previousCode = extended ? (other == '3' ? "04" : "01") : (other == '3' ? "3" : "2");
            f.observe(otherState);
            f.writes.clear(); f.messages.clear();
            require(cat.processCommand(prefix + code).isEmpty(), "representable SmartCAT mode setter is admitted");
            require(f.writes.contains(QByteArray("OM0") + mode.code + ';') && f.writes.contains("OM0;"),
                "actual SmartCAT model backend route writes exact native mode and readback query");
            require(cat.processCommand(prefix) == prefix + previousCode + ';'
                && f.model.slice(0)->mode() == (other == '3' ? "CW" : "USB")
                && !contains(f.messages, "modulation:0," + token + ";"),
                "SmartCAT setter preserves old confirmed GET and emits no requested mode before readback");
            f.observe(actualState);
            require(cat.processCommand(prefix) == prefix + code + ';' && f.model.slice(0)->mode() == mode.native
                && contains(f.messages, "modulation:0," + token + ";"),
                "fresh native SF readback completes SmartCAT setter and actual model broadcast");
            if (extended) { ++zzSetterCases; } else { ++mdSetterCases; }
        }
        f.writes.clear();
        if (mode.native == "RTTYR" || mode.native == "PSKR" || mode.native == "DAM") {
            require(rig.handleLine("M " + mode.hamlib + " 500") == "RPRT -8\n",
                "unsupported native shared-filter geometry is refused, never USB fallback");
            pump(); require(f.writes.isEmpty(), "unsupported native filter request writes nothing");
        }
        require(rig.handleLine("M not_a_mode 0") == "RPRT -1\n" && cat.processCommand("MD0") == "?;"
            && cat.processCommand("ZZMD99") == "?;", "malformed mode names/codes are refused");
        f.protocol().handleCommand("modulation:0,not_a_mode"); pump();
        require(f.writes.isEmpty(), "malformed mode requests never become USB writes");
        f.observe("TS1"); f.writes.clear();
        require(f.protocol().handleCommand("modulation:0") == "modulation:0," + token + ";",
            "temporary write unavailability preserves exact observed mode");
        require(rig.handleLine("M USB 0") == "RPRT -1\n" && cat.processCommand("ZZMD01") == "?;",
            "temporary unavailable native vocabulary refuses setters");
        if (!mode.md.isEmpty()) { require(cat.processCommand("MD" + mode.md) == "?;", "every representable MD code is refused while native controls unavailable"); }
        if (!mode.zz.isEmpty()) { require(cat.processCommand("ZZMD" + mode.zz) == "?;", "every representable ZZMD code is refused while native controls unavailable"); }
        f.protocol().handleCommand("modulation:0,usb"); pump();
        require(f.writes.isEmpty(), "unavailable TCI mode does not dispatch");
        f.backend->disconnectRadio(); f.writes.clear();
        require(f.protocol().handleCommand("modulation:0").isEmpty() && rig.handleLine("m") == "RPRT -8\n"
            && cat.processCommand("MD") == "?;", "disconnected lists cannot authorize or fabricate live modes");
        if (!mode.md.isEmpty()) { require(cat.processCommand("MD" + mode.md) == "?;", "disconnected receiver refuses representable MD setter"); }
        if (!mode.zz.isEmpty()) { require(cat.processCommand("ZZMD" + mode.zz) == "?;", "disconnected receiver refuses representable ZZMD setter"); }
        require(f.writes.isEmpty(), "disconnected SmartCAT mode setters never dispatch");
    }
    require(mdSetterCases == 7 && zzSetterCases == 10, "all seven MD and ten ZZMD native setter encodings were positively exercised");
    {
        Fixture f;
        SmartCatProtocol cat(&f.model);
        f.writes.clear();
        for (const QString& command : {QStringLiteral("MD7"), QStringLiteral("MD8"), QStringLiteral("ZZMD10"),
                QStringLiteral("ZZMD11"), QStringLiteral("ZZMD20"), QStringLiteral("ZZMD40")}) {
            require(cat.processCommand(command) == "?;", "unsupported dialect modes cannot be silently approximated to a native mode");
        }
        require(f.writes.isEmpty() && cat.processCommand("MD") == "MD3;", "wrong native modes leave actual CW unchanged and dispatch nothing");
    }
    {
        Fixture f;
        RigctlProtocol rig(&f.model);
        for (const QString& variant : {QStringLiteral("k"), QStringLiteral("e")}) {
            Access::maximum(*f.backend, variant);
            const QString dump = rig.handleLine("\\dump_state");
            const QStringList rx = dump.split('\n').value(3).split(' ');
            bool validMask = false;
            const quint64 mask = rx.value(2).mid(2).toULongLong(&validMask, 16);
            require(rx.value(1).toDouble() == (variant == "k" ? 60000000.0 : 74800000.0)
                && validMask && (mask & (1ULL << 31)) && (mask & (1ULL << 30)) && (mask & (1ULL << 22))
                && !(mask & 0x200), "native dump advertises K/E receive limits and exact unsigned PSKR/PSK/PKTAM flags, no AMS");
            require(dump.split('\n').value(5) == "30000.000000 54000000.000000 0xebf 1 100000 0x40000003 0x3",
                "RX discovery does not expand the existing TX declaration");
        }
        f.messages.clear();
        f.observe("TS1"); TciServerReviewTest::capabilities(f.model);
        require(!f.messages.join(' ').contains("modulations_list:"),
            "temporary write unavailability preserves supported native vocabulary without spam");
        f.messages.clear(); f.observe("TS1"); TciServerReviewTest::capabilities(f.model);
        require(!f.messages.join(' ').contains("modulations_list:"), "unchanged native list does not spam clients");
        f.observe("TS0"); TciServerReviewTest::capabilities(f.model);
        require(!f.messages.join(' ').contains("modulations_list:"), "restored editability does not change supported vocabulary");
        f.messages.clear(); f.backend->disconnectRadio(); emit f.model.connectionStateChanged(false);
        require(contains(f.messages, "modulations_list:;"), "disconnect withdraws prior native repertoire for existing client");
        auto replacement = std::make_unique<OptimisticBackend>();
        OptimisticBackend* other = replacement.get(); other->confirmed = true; other->modes = {"FMN", "WFM"};
        f.model.setBackendForTest(std::move(replacement), "fixture"); other->announce("FMN", true);
        require(contains(f.messages, "modulations_list:fmn,wfm;"), "real slice callback publishes replacement native vocabulary");
        RigctlProtocol otherRig(&f.model);
        require(otherRig.handleLine("m").startsWith("FMN\n"), "declared native FMN has exact Hamlib readback");
        other->announce("WFM", true);
        require(otherRig.handleLine("m").startsWith("WFM\n"), "declared native WFM has exact Hamlib readback");
        require(otherRig.handleLine("M FMN 0") == "RPRT 0\n", "exact declared FMN takes precedence over legacy NFM alias");
        pump();
        require(other->requestedMode == "FMN" && otherRig.handleLine("m").startsWith("WFM\n"),
            "native FMN setter dispatches exactly and awaits actual readback");
        other->announce("FMN", true);
        require(otherRig.handleLine("m").startsWith("FMN\n"), "actual FMN readback completes native setter roundtrip");
        f.messages.clear(); other->modes.clear(); TciServerReviewTest::capabilities(f.model);
        require(contains(f.messages, "modulations_list:;"), "actual model caps callback withdraws absent native declaration");
        f.messages.clear(); TciServerReviewTest::capabilities(f.model);
        require(!f.messages.join(' ').contains("modulations_list:"), "absent declaration is deduplicated");
        other->modes = {"FMN", "WFM"}; TciServerReviewTest::capabilities(f.model);
        require(contains(f.messages, "modulations_list:fmn,wfm;"), "actual model caps callback restores declared vocabulary");
    }
    for (const bool physicalVfoChange : {true, false}) {
        Fixture f;
        RigctlProtocol rig(&f.model);
        require(rig.handleLine("M CWR 0") == "RPRT 0\n", "queue native reverse CW against the original receive context");
        if (physicalVfoChange) {
            f.observe("SF1000140740003"); f.observe("FR1"); f.observe("SF1000140740003");
        } else {
            f.observe("SF0000140740002"); f.observe("SF0000140740003");
        }
        f.observe("TS0"); f.observe("RX"); f.observe("AP00");
        require(f.model.backendCapabilities().receiveModeControl->modes.contains("CWR"),
            "new receiver context is fully qualified before queued rigctl mode runs");
        f.writes.clear(); pump();
        require(!f.writes.contains("OM07;"), "queued native rigctl mode cannot survive physical source or observed mode cycle");
    }
    for (const bool physicalVfoChange : {true, false}) {
        Fixture f('7');
        RigctlProtocol rig(&f.model);
        require(rig.handleLine("M CWR 200") == "RPRT 0\n", "queue a qualified same-mode CWR filter change");
        if (physicalVfoChange) {
            f.observe("SF1000140740007"); f.observe("FR1"); f.observe("SF1000140740007");
        } else {
            f.observe("SF0000140740002"); f.observe("SF0000140740007");
        }
        f.observe("TS0"); f.observe("RX"); f.observe("SL006"); f.observe("SH0080"); f.observe("AP00");
        require(f.model.slice(0)->mode() == "CWR"
            && f.model.backendCapabilities().receiveModeControl->modes.contains("CWR")
            && f.model.backendCapabilities().receiveFilterControl.has_value(),
            "new physical or mode-cycle target is fully qualified before queued filter dispatch");
        f.writes.clear(); pump();
        require(!f.writes.contains("SL004;") && !f.writes.contains("SH0080;"),
            "old queued filter cannot reach a fresh physical VFO or a restored mode bank");
    }
    {
        Fixture f('7');
        RigctlProtocol rig(&f.model);
        require(rig.handleLine("M CWR 200") == "RPRT 0\n", "same-mode CWR width uses qualified symmetric geometry");
        pump();
        require(f.writes.contains("SL004;") && f.writes.contains("SH0080;"), "CWR passband is centered, not USB high-side fallback");
    }
    RadioModel model;
    auto owned = std::make_unique<OptimisticBackend>();
    OptimisticBackend* backend = owned.get();
    model.setBackendForTest(std::move(owned), "sim"); backend->announce();
    RigctlProtocol rig(&model);
    TciProtocol tci(&model);
    model.slice(0)->setMode("DFM");
    require(rig.handleLine("m").startsWith("FM\n") && tci.handleCommand("modulation:0") == "modulation:0,fm;",
        "legacy optimistic data FM mapping remains unchanged");
    require(rig.handleLine("M CWR 0") == "RPRT 0\n", "legacy CWR alias still accepted"); pump();
    require(model.slice(0)->mode() == "CW", "legacy CWR still resolves to existing Flex CW mode");
    RadioModel undeclared;
    auto strict = std::make_unique<OptimisticBackend>();
    OptimisticBackend* strictBackend = strict.get(); strictBackend->confirmed = true;
    undeclared.setBackendForTest(std::move(strict), "fixture"); strictBackend->announce();
    RigctlProtocol strictRig(&undeclared);
    SmartCatProtocol strictCat(&undeclared);
    TciProtocol strictTci(&undeclared);
    require(strictRig.handleLine("m") == "RPRT -8\n" && strictRig.handleLine("M USB 0") == "RPRT -1\n"
        && strictCat.processCommand("MD") == "?;" && strictCat.processCommand("ZZMD01") == "?;"
        && strictTci.handleCommand("modulation:0").isEmpty(), "confirmed receiver without a declared vocabulary stays unavailable, never legacy USB");
}
void checkConfirmedOnlyStateRelays()
{
    const QStringList addedCommands{"rx_mute:", "rx_record:", "rx_play:", "rx_balance:",
        "rx_nb_param:", "digl_offset:", "digu_offset:"};
    const auto hasAddedRelay = [&addedCommands](const QStringList& messages) {
        for (const QString& message : messages) {
            for (const QString& command : message.split(';', Qt::SkipEmptyParts)) {
                for (const QString& prefix : addedCommands) {
                    if (command.startsWith(prefix)) { return true; }
                }
            }
        }
        return false;
    };
    for (bool confirmed : {false, true}) {
        QStringList messages;
        RadioModel model;
        TciServer server(&model);
        TciServerReviewTest::capture(server, model, messages);
        auto owned = std::make_unique<OptimisticBackend>();
        OptimisticBackend* backend = owned.get();
        backend->confirmed = confirmed;
        model.setBackendForTest(std::move(owned), "fixture");
        backend->announce();
        SliceModel* slice = model.slice(0);
        require(slice && slice->receiveControlsConfirmed() == confirmed,
            "relay regression binds the requested production receive policy");
        SliceDelta baseline;
        baseline.audioMute = false;
        baseline.recordOn = false;
        baseline.play = QStringLiteral("0");
        baseline.audioPan = 50;
        baseline.nbLevel = 0;
        slice->applyChanges(baseline);
        server.wireSlice(0, slice);
        spin(450);
        require(contains(messages, "mute:0,false;"), "existing mute seed remains available for both policies");
        if (confirmed) {
            require(contains(messages, "rx_mute:0,false;") && contains(messages, "rx_record:0,false;")
                && contains(messages, "rx_play:0,false;") && contains(messages, "rx_balance:0,0;")
                && contains(messages, "rx_nb_param:0,0,0;"),
                "confirmed receive binding retains its added actual-state seeds");
        } else {
            require(!hasAddedRelay(messages), "legacy receive binding must not gain new state-relay seeds");
        }
        messages.clear();
        SliceDelta observation;
        observation.audioMute = true;
        observation.recordOn = true;
        observation.play = QStringLiteral("1");
        observation.audioPan = 60;
        observation.nbLevel = 23;
        observation.diglOffset = 1300;
        observation.diguOffset = 1700;
        slice->applyChanges(observation);
        require(contains(messages, "mute:0,true;"), "existing mute change relay remains unchanged");
        if (confirmed) {
            for (const QString& expected : QStringList{"rx_mute:0,true;", "rx_record:0,true;", "rx_play:0,true;",
                    "rx_balance:0,10;", "rx_nb_param:0,0,23;", "digl_offset:1300;", "digu_offset:1700;"}) {
                require(contains(messages, expected), qPrintable(expected));
            }
        } else {
            require(!hasAddedRelay(messages), "legacy receive binding must not gain new change notifications");
        }
    }
}
void checkMeters()
{
    QStringList messages;
    RadioModel model;
    TciServer server(&model);
    TciClient* client = TciServerReviewTest::capture(server, model, messages);
    Q_UNUSED(client);
    auto owned = std::make_unique<OptimisticBackend>();
    OptimisticBackend* backend = owned.get();
    model.setBackendForTest(std::move(owned), "sim");
    backend->announce();
    server.wireSlice(0, model.slice(0));
    TciServerReviewTest::status(server);
    require(!meterMessage(messages), "no invented default minus130 dBm before any calibrated arrival");
    MeterDef def;
    def.index = 501; def.source = "SLC"; def.sourceIndex = 0; def.name = "LEVEL"; def.unit = "dBm";
    model.meterModel().defineMeter(def);
    model.meterModel().updateValueByName("SLC", "LEVEL", -73.0f, 0);
    messages.clear(); TciServerReviewTest::status(server);
    require(contains(messages, "rx_smeter:0,-73;") && contains(messages, "rx_channel_sensors:0,0,-73.0;"),
        "actual fresh calibrated model value supplies both TCI meter paths");
    model.slice(0)->invalidateFrequencyObservation();
    messages.clear(); TciServerReviewTest::status(server);
    require(!meterMessage(messages), "retired receiver binding cannot reuse a still-fresh calibrated sample");
    model.meterModel().updateValueByName("SLC", "LEVEL", -74.0f, 0);
    messages.clear(); TciServerReviewTest::status(server);
    require(contains(messages, "rx_smeter:0,-74;"), "new calibrated arrival rebinds the current receiver epoch");
    spin(MeterModel::kVitalsFreshMs + 10);
    messages.clear(); TciServerReviewTest::status(server);
    require(!meterMessage(messages), "expired calibrated sample cannot keep broadcasting indefinitely");
    model.meterModel().updateValueByName("SLC", "LEVEL", -80.0f, 0);
    def.unavailableReason = "Radio measurement unavailable";
    model.meterModel().defineMeter(def);
    messages.clear(); TciServerReviewTest::status(server);
    require(!meterMessage(messages), "explicitly unavailable calibrated meter is not published");
    def.unavailableReason.clear();
    model.meterModel().defineMeter(def);
    model.meterModel().updateValueByName("SLC", "LEVEL", -81.0f, 0);
    auto replacement = std::make_unique<OptimisticBackend>();
    backend = replacement.get();
    model.setBackendForTest(std::move(replacement), "sim");
    backend->announce();
    server.wireSlice(0, model.slice(0));
    messages.clear(); TciServerReviewTest::status(server);
    require(!meterMessage(messages), "new session/slice cannot reuse a still-fresh previous receiver sample");
    def.unit = "dot"; def.low = 0; def.high = 70;
    model.meterModel().defineMeter(def);
    model.meterModel().updateValueByName("SLC", "LEVEL", 36.0f, 0);
    messages.clear(); TciServerReviewTest::status(server);
    require(!meterMessage(messages), "raw dots never become TCI calibrated dBm or a remembered cache");
}
}
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("tci-confirmed-receive"));
    require(profile.isValid(), "isolated settings");
    QCoreApplication application(argc, argv);
    AppSettings::instance().load();
    struct Case {
        char mode;
        QString set;
        QString get;
        QString before;
        QString after;
        QList<QByteArray> setters;
        QList<QByteArray> observations;
    };
    const QList<Case> cases{
        {'3', "modulation:0,usb", "modulation:0", "modulation:0,cw;", "modulation:0,usb;", {"OM02;"}, {"SF0000140740002"}},
        {'3', "rx_filter_band:0,-100,100", "rx_filter_band:0", "rx_filter_band:0,-150,150;", "rx_filter_band:0,-100,100;", {"SL004;", "SH0080;"}, {"SL004", "SH0080"}},
        {'3', "rx_nr_enable:0,true", "rx_nr_enable:0", "rx_nr_enable:0,false;", "rx_nr_enable:0,true;", {"NR1;"}, {"NR1"}},
        {'3', "rx_nb_enable:0,true", "rx_nb_enable:0", "rx_nb_enable:0,false;", "rx_nb_enable:0,true;", {"NB11;"}, {"NB11"}},
        {'2', "rx_anf_enable:0,true", "rx_anf_enable:0", "rx_anf_enable:0,false;", "rx_anf_enable:0,true;", {"BC1;"}, {"BC1"}},
        {'3', "rx_apf_enable:0,true", "rx_apf_enable:0", "rx_apf_enable:0,false;", "rx_apf_enable:0,true;", {"AP01;"}, {"AP01"}},
        {'3', "agc_mode:0,fast", "agc_mode:0", "agc_mode:0,slow;", "agc_mode:0,fast;", {"GC3;"}, {"GC3"}},
        {'3', "rx_mute:0,true", "rx_mute:0", "rx_mute:0,false;", "rx_mute:0,true;", {"MU1;"}, {"MU1"}},
        {'3', "rx_volume:0,70", "rx_volume:0", "rx_volume:0,50;", "rx_volume:0,70;", {"AG179;"}, {"AG179"}},
        {'3', "rit_enable:0,true", "rit_enable:0", "rit_enable:0,false;", "rit_enable:0,true;", {"RT1;"}, {"RT1"}},
        {'3', "rit_offset:0,123", "rit_offset:0", "rit_offset:0,0;", "rit_offset:0,123;", {"RU00123;"}, {"RF00123"}},
        {'3', "sql_enable:0,false", "sql_enable:0", "sql_enable:0,true;", "sql_enable:0,false;", {"SQ000;"}, {"SQ000"}},
        {'3', "rx_nb_param:0,0,50", "rx_nb_param:0", "rx_nb_param:0,0,0;", "rx_nb_param:0,0,53;", {"NL1011;"}, {"NL1011"}},
    };
    for (const Case& test : cases) {
        std::cerr << "CASE " << qPrintable(test.set) << std::endl;
        Fixture f(test.mode);
        require(f.protocol().handleCommand(test.get) == test.before, qPrintable(test.get));
        f.protocol().handleCommand(test.set);
        require(f.protocol().pendingNotification().isEmpty(), "confirmed SET emits no requested-state notification");
        require(f.protocol().handleCommand(test.get) == test.before, "GET stays at actual state before queued dispatch");
        pump();
        for (const QByteArray& command : test.setters) { require(f.writes.contains(command), qPrintable(QString::fromLatin1(command))); }
        // APF availability is withdrawn while its native readback is pending;
        // the prior model boolean remains confirmed, but GET must omit it then.
        const bool pendingApf = test.get == QStringLiteral("rx_apf_enable:0");
        const QString expectedDispatched = pendingApf ? QString() : test.before;
        const QString dispatchedState = f.protocol().handleCommand(test.get);
        if (pendingApf) {
            require(!f.model.slice(0)->apfOn() && !f.model.backendCapabilities().hasAudioPeakingFilter,
                "pending APF keeps prior confirmed Off and withdraws availability until readback");
        }
        if (dispatchedState != expectedDispatched || contains(f.messages, test.after)) {
            std::cerr << "SET " << qPrintable(test.set) << " expected dispatched=" << qPrintable(expectedDispatched)
                      << " actual=" << qPrintable(dispatchedState) << " messages=" << qPrintable(f.messages.join(" | "))
                      << " writes=";
            for (const QByteArray& command : f.writes) { std::cerr << command.constData(); }
            std::cerr << std::endl;
        }
        require(dispatchedState == expectedDispatched && !contains(f.messages, test.after),
            "accepted transport dispatch still cannot claim unobserved radio state");
        for (const QByteArray& observation : test.observations) { f.observe(observation); }
        require(f.protocol().handleCommand(test.get) == test.after && contains(f.messages, test.after),
            "actual backend readback changes model GET and the production TCI broadcast");
    }
    {
        Fixture f('2'); // Native APF is unsupported in USB.
        require(f.protocol().handleCommand("rx_apf_enable:0").isEmpty(), "unknown/unsupported APF GET is not fabricated false");
        require(!f.protocol().generateInitBurst().contains("rx_apf_enable:"), "init omits unavailable confirmed APF state");
        f.protocol().handleCommand("rx_apf_enable:0,true");
        require(f.protocol().pendingNotification().isEmpty(), "unsupported APF has no false success echo");
        pump();
        require(!f.writes.contains("AP01;") && !contains(f.messages, "rx_apf_enable:0,true;"), "unsupported APF is never dispatched or announced On");
        for (const QString& command : {QStringLiteral("rx_bin_enable:0,true"), QStringLiteral("rx_anc_enable:0,true"),
             QStringLiteral("rx_dse_enable:0,true"), QStringLiteral("rx_nf_enable:0,false")}) {
            f.protocol().handleCommand(command);
            require(f.protocol().pendingNotification().isEmpty(), "unsupported placeholder has no requested status");
            require(f.protocol().handleCommand(command.section(',', 0, 0)).isEmpty(), "confirmed receiver does not invent placeholder GET state");
        }
    }
    {
        Fixture f;
        f.accept = false;
        f.protocol().handleCommand("modulation:0,usb");
        require(f.protocol().pendingNotification().isEmpty(), "refused write has no optimistic echo");
        pump(); f.observe("?");
        require(f.protocol().handleCommand("modulation:0") == "modulation:0,cw;", "refusal preserves last actual receive mode");
        f.accept = true;
        f.backend->disconnectRadio();
        f.writes.clear();
        f.protocol().handleCommand("modulation:0,usb"); pump();
        require(f.writes.isEmpty() && !contains(f.messages, "modulation:0,usb;"), "disconnected request cannot claim success");
    }
    {
        Fixture f;
        f.protocol().handleCommand("modulation:0,usb");
        require(f.protocol().pendingNotification().isEmpty(), "queued mode has no premature success");
        f.replace();
        pump();
        require(!f.writes.contains("OM02;") && f.model.slice(0)->mode() == "CW"
            && !contains(f.messages, "modulation:0,usb;"), "retired queued intent cannot reach a replacement backend/slice");
    }
    {
        Fixture f;
        f.protocol().handleCommand("modulation:0,usb");
        f.model.slice(0)->invalidateFrequencyObservation();
        pump();
        require(!f.writes.contains("OM02;") && !contains(f.messages, "modulation:0,usb;"),
            "same live slice with a retired receiver binding rejects the old queued intent");
    }
    for (const bool physicalVfoChange : {true, false}) {
        Fixture f;
        SliceModel* const originalSlice = f.model.slice(0);
        const quint64 originalEpoch = originalSlice->receiveSessionGeneration();
        f.protocol().handleCommand("rx_apf_enable:0,true");
        require(f.protocol().pendingNotification().isEmpty(), "queued APF has no premature success");
        if (physicalVfoChange) {
            // Same logical receiver, frequency and CW mode; only the physical
            // source changes. Fully qualify B before delivering the old queue.
            f.observe("SF1000140740003");
            f.observe("FR1");
            f.observe("SF1000140740003");
        } else {
            f.observe("SF0000140740002");
            f.observe("SF0000140740003");
        }
        f.observe("TS0");
        f.observe("RX");
        f.observe("AP00");
        require(f.model.slice(0) == originalSlice && originalSlice->mode() == "CW"
            && f.model.backendCapabilities().hasAudioPeakingFilter
            && f.protocol().handleCommand("rx_apf_enable:0") == "rx_apf_enable:0,false;",
            "replacement receive context is fully qualified with actual APF Off before old queue runs");
        require(originalSlice->receiveSessionGeneration() != originalEpoch,
            physicalVfoChange ? "physical source change retires the receiver epoch"
                              : "observed mode cycle retires the receiver epoch");
        f.writes.clear();
        f.messages.clear();
        pump();
        require(!f.writes.contains("AP01;") && !originalSlice->apfOn()
            && !contains(f.messages, "rx_apf_enable:0,true;"),
            physicalVfoChange ? "old queued APF cannot reach a newly qualified physical VFO"
                              : "old queued APF cannot survive a fully qualified CW USB CW cycle");
    }
    {
        Fixture f;
        emit f.client->textMessageReceived(QStringLiteral("rx_ctun_ex:0,true;"));
        require(!contains(f.messages, "rx_ctun_ex:0,true;"), "missing host center-lock callback cannot claim success");
        bool locked = true;
        f.server.setCenterLockHandlers([](int, bool) {}, [&](int) { return locked; });
        f.messages.clear();
        emit f.client->textMessageReceived(QStringLiteral("rx_ctun_ex:0,true;"));
        require(contains(f.messages, "rx_ctun_ex:0,false;") && !contains(f.messages, "rx_ctun_ex:0,true;"),
            "refused host center-lock request announces only the actual queried state");
        f.server.setCenterLockHandlers([&](int, bool value) { locked = value; }, [&](int) { return locked; });
        f.messages.clear();
        emit f.client->textMessageReceived(QStringLiteral("rx_ctun_ex:0,true;"));
        require(contains(f.messages, "rx_ctun_ex:0,true;") && !locked,
            "accepted host center-lock change is announced after its actual query confirms it");
        f.server.setCenterLockHandlers([&](int, bool) { f.replace(); }, [&](int) { return locked; });
        f.messages.clear();
        emit f.client->textMessageReceived(QStringLiteral("rx_ctun_ex:0,false;"));
        require(!contains(f.messages, "rx_ctun_ex:0,false;"), "center-lock callback cannot announce an old session's result after replacement");
    }
    {
        RadioModel model;
        auto owned = std::make_unique<OptimisticBackend>();
        OptimisticBackend* backend = owned.get();
        model.setBackendForTest(std::move(owned), "sim"); backend->announce();
        TciProtocol protocol(&model);
        protocol.handleCommand("modulation:0,usb");
        require(protocol.pendingNotification() == "modulation:0,usb;", "optimistic policy retains its existing immediate notification");
        pump();
        require(protocol.handleCommand("modulation:0") == "modulation:0,usb;", "optimistic model behavior remains unchanged");
    }
    checkDeclaredModes();
    checkConfirmedOnlyStateRelays();
    checkMeters();
    std::cout << "TCI confirmed receive checks passed\n";
}
