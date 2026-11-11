// Official PC reference 2019-01-30 pp.4,6,7,25,49,50,57;
// Instruction Manual EN-6-5..6-9. Injected transport, no sockets/devices/RF.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include <QCoreApplication>
#include <QList>
#include <QString>
#include <QVariantMap>
#include <QVariantList>
#include <QPair>
#include <QMetaType>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace AetherSDR;
namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static QVariantMap chronologyStatus(const KenwoodBackend& b) { return b.receiveControlsStatus(); }
    static void chronologyPublish(KenwoodBackend& b) { b.publishReceiveControls(); }
    static void chronologyFeed(KenwoodBackend& b, const QByteArray& bytes) { b.adopt(bytes); }
    static void authenticate(KenwoodBackend& backend)
    {
        backend.m_clock.start();
        backend.m_protocol.start("fixture", "fixture", false, 0);
        backend.m_protocol.receive("##CN1;", 1);
        backend.m_protocol.receive("##ID1;", 2);
        backend.ready();
    }
    static quint64 feed(KenwoodBackend& backend, const QByteArray& message)
    { backend.adopt(message); return backend.m_controlIngressSequence; }
    static QVariantMap status(const KenwoodBackend& backend) { return backend.receiveControlsStatus(); }
    static void withdrawRx(KenwoodBackend& backend) { backend.m_txStateKnown = false; }
    static void keyIntent(KenwoodBackend& backend, bool active) { backend.m_keyIntent = active; }
    static void stale(KenwoodBackend& backend, const QByteArray& message, quint64 batch)
    { backend.adopt(message, batch); }
};
}
namespace {
void require(bool condition, const char* claim)
{
    if (!condition) { std::cerr << claim << '\n'; std::exit(1); }
}
using Access = kenwood::KenwoodBackendTestAccess;
struct Fixture {
    QList<QByteArray> writes;
    kenwood::KenwoodBackend backend{kenwood::KenwoodBackend::Transport{
        [this](const QByteArray& bytes) { writes.append(bytes); return true; },
        [](const QByteArray&) { return false; }}};
    QVariantMap state;
    bool accepted = false;
    bool rejected = false;
    int failures = 0;
    bool genericNr = false;
    bool genericNb = false;
    bool genericAnf = false;
    Fixture()
    {
        QObject::connect(&backend, &IRadioBackend::extensionResult, &backend,
            [this](quint64, const QVariant& result) {
                if (result.typeId() == QMetaType::QVariantMap) { state = result.toMap(); }
                else { accepted = result.toBool(); }
            });
        QObject::connect(&backend, &IRadioBackend::extensionError, &backend,
            [this](quint64, const QString&) { rejected = true; });
        QObject::connect(&backend, &IRadioBackend::sliceChanged, &backend,
            [this](int, const SliceDelta& delta) {
                if (delta.nr) { genericNr = *delta.nr; }
                if (delta.nb) { genericNb = *delta.nb; }
                if (delta.anf) { genericAnf = *delta.anf; }
            });
    }
    QVariantMap status() { return Access::status(backend); }
    void refresh() { backend.invokeExtension("kenwood", "receive.controls.get", 1); }
    QVariantMap row(const QString& id)
    {
        for (const QVariant& entry : status().value("controls").toList()) {
            if (entry.toMap().value("id").toString() == id) { return entry.toMap(); }
        }
        return {};
    }
    void connectCw()
    {
        Access::authenticate(backend);
        for (const QByteArray& message : {QByteArray("ID024"), QByteArray("FR0"), QByteArray("FT0"),
             QByteArray("TS0"), QByteArray("SF0000140740003"), QByteArray("RX"), QByteArray("MO20")}) { Access::feed(backend, message); }
        require(writes.contains("AP0;") && !writes.contains("AP1;") && !writes.contains("NBD;"),
            "shared APF enable is discovered while extended parameter queries remain lazy");
        refresh();
    }
    void seed()
    {
        for (const QByteArray& message : {QByteArray("AP00"), QByteArray("AP140"), QByteArray("AP21"),
             QByteArray("AP33"), QByteArray("NT1"), QByteArray("NW0"), QByteArray("BP127"),
             QByteArray("NR1"), QByteArray("RL105"), QByteArray("RL209"), QByteArray("NB11"),
             QByteArray("NL1010"), QByteArray("NB21"), QByteArray("NBT1"), QByteArray("NL2006"),
             QByteArray("NBD020"), QByteArray("NBW010")}) { Access::feed(backend, message); }
    }
    bool set(const QString& id, const QVariant& value, const QString& context = {})
    {
        const QString current = context.isEmpty() ? status().value("context").toString() : context;
        accepted = rejected = false;
        backend.invokeExtension("kenwood", "receive.controls.set", 2,
            QVariantMap{{"context", current}, {"id", id}, {"value", value}});
        return accepted && !rejected;
    }
};
}

void verifyReceiveControlsProducerChronology()
{
    using Backend = AetherSDR::kenwood::KenwoodBackend;
    using Probe = AetherSDR::kenwood::KenwoodBackendTestAccess;
    Backend backend(Backend::Transport{[](const QByteArray&) { return true; }, {}});
    const QVariant initial = Probe::chronologyStatus(backend).value("statusRevision");
    require(initial.typeId() == QMetaType::QString && initial.toString() == "0", "receiveControls initial getter has canonical QString revision zero");
    Probe::authenticate(backend);
    for (const QByteArray& bytes : {QByteArray("ID024"), QByteArray("FR0"), QByteArray("FA00014074000"), QByteArray("SF0000140740003"), QByteArray("TS0"), QByteArray("RX")}) { Probe::chronologyFeed(backend, bytes); }
    bool nested = false; QList<QVariantMap> delivered;
    QObject::connect(&backend, &AetherSDR::IRadioBackend::extensionStatus, &backend,
        [&](const QString&, const QString& statusKind, const QVariantMap&) {
            if (statusKind != "receiveControls" || nested) { return; }
            nested = true; Probe::chronologyFeed(backend, "TS1");
        });
    QObject::connect(&backend, &AetherSDR::IRadioBackend::extensionStatus, &backend,
        [&](const QString&, const QString& statusKind, const QVariantMap& state) {
            if (statusKind == "receiveControls") { delivered.append(state); }
        });
    Probe::chronologyPublish(backend);
    require(nested && delivered.size() >= 2, "receiveControls real TF withdrawal publishes newer status during outer delivery");
    const quint64 newer = delivered.first().value("statusRevision").toString().toULongLong();
    const quint64 older = delivered.last().value("statusRevision").toString().toULongLong();
    require(newer > older, "receiveControls producer stamps snapshot before nested newer delivery");
    for (const QVariantMap& state : delivered) {
        const QVariant revision = state.value("statusRevision");
        require(revision.typeId() == QMetaType::QString && revision.toString() == QString::number(revision.toString().toULongLong()), "receiveControls every revision is canonical decimal QString");
    }
    QVariantMap queried;
    QObject::connect(&backend, &AetherSDR::IRadioBackend::extensionResult, &backend,
        [&](quint64 request, const QVariant& result) { if (request == 901) { queried = result.toMap(); } });
    backend.invokeExtension("kenwood", "receive.controls.get", 901);
    require(queried.value("statusRevision") == Probe::chronologyStatus(backend).value("statusRevision")
        && queried.value("statusRevision").toString().toULongLong() >= newer, "receiveControls GET carries latest producer revision after stale outer delivery");
    const quint64 beforeReset = queried.value("statusRevision").toString().toULongLong();
    backend.disconnectRadio(); Probe::chronologyPublish(backend);
    require(Probe::chronologyStatus(backend).value("statusRevision").toString().toULongLong() > beforeReset, "receiveControls disconnect never resets lifetime publication chronology");
}

void firmwareApfZeroReadback()
{
    Fixture f; f.connectCw(); Access::feed(f.backend, "MO20"); Access::feed(f.backend, "AP00");
    require(f.row("apf.enabled").contains("value") && f.row("apf.enabled").value("value").toInt() == 1
        && f.row("apf.enabled").value("editable").toBool(), "live firmware AP00 must populate confirmed Off and enable APF control");
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("kenwood-receive-controls"));
    require(profile.isValid(), "isolated settings");
    QCoreApplication application(argc, argv);
    firmwareApfZeroReadback();
    verifyReceiveControlsProducerChronology();
    AppSettings::instance().load();
    Fixture f;
    require(f.status().value("controls").toList().size() == 20 && !f.status().value("editable").toBool(),
        "all native controls remain visible but unavailable while disconnected");
    f.connectCw();
    require(f.status().value("editable").toBool() && !f.row("apf.enabled").contains("value"),
        "confirmed RX context does not invent native parameter values");
    require(f.writes.contains("AP0;") && f.writes.contains("RL2;") && f.writes.contains("NBD;"),
        "context entry queries actual native controls");
    f.seed();
    require(f.row("apf.enabled").value("value") == 1 && f.row("apf.enabled").value("editable").toBool(),
        "live AP00 maps to the existing Off choice ID");
    const qsizetype before = f.writes.size();
    const QString originalContext = f.status().value("context").toString();
    require(f.set("apf.enabled", 2), "native APF On dispatches with current context");
    require(f.writes.size() == before + 2 && f.writes[before] == "AP01;" && f.writes[before + 1] == "AP0;",
        "APF writes firmware On command followed by query");
    require(f.row("apf.enabled").value("value") == 1 && !f.row("apf.enabled").value("editable").toBool()
        && f.row("apf.enabled").value("pending").toBool(), "dispatch retains confirmed Off without inventing On");
    Access::feed(f.backend, "?");
    require(f.row("apf.enabled").value("value") == 1 && f.row("apf.enabled").value("pending").toBool(),
        "uncorrelated refusal preserves confirmed value while awaiting readback");
    const qsizetype refreshStart = f.writes.size();
    f.refresh();
    require(f.writes.size() > refreshStart && f.writes.mid(refreshStart).contains("AP0;"),
        "deliberate Refresh queries pending controls without inventing a value");
    Access::feed(f.backend, "AP01");
    require(f.row("apf.enabled").value("value") == 2, "confirmed APF readback owns visible state");
    require(!f.set("apf.enabled", 1, originalContext), "an old descriptor context cannot dispatch another setter");
    for (const QVariant& value : {QVariant(true), QVariant("2"), QVariant(1.5),
         QVariant(std::numeric_limits<double>::quiet_NaN()), QVariant(std::numeric_limits<double>::infinity()), QVariant(9)}) {
        const qsizetype count = f.writes.size();
        require(!f.set("apf.enabled", value) && f.writes.size() == count, "invalid numeric types/ranges/sentinels never write");
    }
    Access::feed(f.backend, "AP099");
    Access::feed(f.backend, "AP02");
    require(f.row("apf.enabled").value("value") == 2, "invalid reset/two replies do not replace accepted AP0 state");
    require(f.set("apf.shift", 80) && f.writes.contains("AP180;"), "APF shift upper endpoint encodes two wire digits");
    Access::feed(f.backend, "AP180");
    require(f.row("apf.shift").value("choices").toList().last().toMap().value("label") == "+200 Hz"
        || f.row("apf.shift").value("choices").toList().last().toMap().value("label") == "200 Hz",
        "APF shift label maps index80 to200Hz without changing wire units");
    require(!f.row("nr2.time").value("editable").toBool(), "NR2 time cannot edit while NR1 selected");
    require(f.set("nr.mode", 2), "native NR2 selection is a distinct operation");
    require(!f.row("nr1.level").value("editable").toBool() && !f.row("nr2.time").value("editable").toBool(),
        "pending NR selection makes dependent parameter banks unavailable");
    Access::feed(f.backend, "NR2");
    require(!f.row("nr2.time").contains("value"), "variant transition requires a fresh active-parameter readback");
    Access::feed(f.backend, "RL209");
    require(f.genericNr && f.row("nr2.time").value("editable").toBool()
        && !f.row("nr1.level").value("editable").toBool(), "native NR variant remains coherent with generic NR state");
    require(f.set("nr2.time", 0) && f.writes.contains("RL200;"), "NR2 two-ms endpoint uses index00");
    Access::feed(f.backend, "RL200");
    require(!f.set("nr2.time", 99), "NR2 reset sentinel is not a normal time constant");
    Access::feed(f.backend, "NBT0");
    require(!f.row("nb2.depth").value("editable").toBool() && !f.row("nb2.width").value("editable").toBool(),
        "type-B parameters remain unavailable in type A");
    const quint64 typeBatch = Access::feed(f.backend, "NBT1");
    require(!f.row("nb2.level").contains("value"), "NB2 type change invalidates the differently interpreted NL2 bank");
    Access::stale(f.backend, "NL2003", typeBatch);
    require(!f.row("nb2.level").contains("value"), "same-batch prior-type NL2 cannot populate the new bank");
    Access::feed(f.backend, "NL2007");
    require(f.row("nb2.level").value("value") == 7, "fresh new-bank NL2 readback owns its parameter");
    require(f.set("nb2.depth", 1) && f.writes.contains("NBD001;"), "NB2 depth lower endpoint uses three digits");
    Access::feed(f.backend, "NBD001");
    require(f.set("nb2.width", 20) && f.writes.contains("NBW020;"), "NB2 width upper endpoint uses three digits");
    Access::feed(f.backend, "NBW020");
    Access::feed(f.backend, "NB10");
    require(!f.genericNb && f.row("nb2.enabled").value("value") == 1, "NB2 does not impersonate shared NB1 status");
    const QList<QPair<QString, QByteArray>> resets{{"apf.shift", "AP199;"}, {"apf.bandwidth", "AP29;"},
        {"apf.gain", "AP39;"}, {"nr1.level", "RL199;"}, {"nr2.time", "RL299;"},
        {"nb1.level", "NL1999;"}, {"nb2.level", "NL2999;"}, {"nb2.depth", "NBD999;"}, {"nb2.width", "NBW999;"}};
    for (const QPair<QString, QByteArray>& reset : resets) {
        f.seed();
        if (reset.first == "nr2.time") { Access::feed(f.backend, "NR2"); Access::feed(f.backend, "RL209"); }
        const QVariant confirmed = f.row(reset.first).value("value");
        const qsizetype count = f.writes.size();
        f.accepted = f.rejected = false;
        f.backend.invokeExtension("kenwood", "receive.controls.reset", 3,
            QVariantMap{{"context", f.status().value("context")}, {"id", reset.first}});
        require(f.accepted && !f.rejected && f.writes.size() == count + 2 && f.writes[count] == reset.second,
            "each documented RX reset uses its separate sentinel operation and followup query");
        require(f.row(reset.first).value("value") == confirmed && f.row(reset.first).value("pending").toBool(),
            "reset does not assume or display an unobserved default");
        Access::feed(f.backend, reset.second.chopped(1));
        require(f.row(reset.first).value("value") == confirmed && f.row(reset.first).value("pending").toBool(),
            "set-only reset sentinel is not accepted as a normal radio readback");
    }
    // Every native control exercises both numeric endpoints; small discrete
    // choices are enumerated. This catches prefix, width and bounds drift.
    struct ControlCase { QString id; QByteArray prefix; int low; int high; int digits; };
    const QList<ControlCase> cases{
        {"apf.enabled", "AP0", 1, 2, 1}, {"apf.shift", "AP1", 0, 80, 2},
        {"apf.bandwidth", "AP2", 0, 2, 1}, {"apf.gain", "AP3", 0, 6, 1},
        {"notch.enabled", "NT", 0, 1, 1}, {"notch.width", "NW", 0, 2, 1},
        {"notch.position", "BP", 0, 255, 3}, {"nr.mode", "NR", 0, 2, 1},
        {"nr1.level", "RL1", 1, 10, 2}, {"nr2.time", "RL2", 0, 9, 2},
        {"nb1.enabled", "NB1", 0, 1, 1}, {"nb1.level", "NL1", 1, 20, 3},
        {"nb2.enabled", "NB2", 0, 1, 1}, {"nb2.level", "NL2", 1, 10, 3},
        {"nb2.type", "NBT", 0, 1, 1}, {"nb2.depth", "NBD", 1, 20, 3},
        {"nb2.width", "NBW", 1, 20, 3}, {"bc.mode", "BC", 0, 2, 1}};
    for (const ControlCase& control : cases) {
        QList<int> values{control.low, (control.low + control.high) / 2, control.high};
        if (control.high - control.low <= 9) {
            values.clear();
            for (int value = control.low; value <= control.high; ++value) { values.append(value); }
        }
        for (int value : values) {
            Access::feed(f.backend, control.id == "bc.mode" ? "SF0000140740002" : "SF0000140740003");
            f.seed();
            if (control.id == "bc.mode") { Access::feed(f.backend, "BC0"); }
            if (control.id == "nr2.time") { Access::feed(f.backend, "NR2"); Access::feed(f.backend, "RL209"); }
            const qsizetype count = f.writes.size();
            const int wireValue = control.id == "apf.enabled" ? value - 1 : value;
            const QByteArray body = control.prefix + QByteArray::number(wireValue).rightJustified(control.digits, '0');
            require(f.set(control.id, value) && f.writes.size() == count + 2
                && f.writes[count] == body + ';' && f.writes[count + 1] == control.prefix + ';',
                "all native endpoints/discrete choices use the correct setter/query encoding");
            Access::feed(f.backend, body);
            require(f.row(control.id).value("value") == value && !f.row(control.id).value("pending").toBool(),
                "actual readback completes each native parameter request");
            require(!f.set(control.id, control.low - 1) && !f.set(control.id, control.high + 1),
                "each native control rejects values outside its ordinary range");
        }
    }
    Access::feed(f.backend, "SF0000140740003");
    f.seed();
    for (const QString& target : {QStringLiteral("nb2.depth"), QStringLiteral("nr1.level")}) {
        f.seed();
        bool change = true;
        const QMetaObject::Connection dependency = QObject::connect(&f.backend, &IRadioBackend::extensionStatus, &f.backend,
            [&](const QString&, const QString& kind, const QVariantMap& status) {
                if (!change || kind != "receiveControls") { return; }
                for (const QVariant& value : status.value("controls").toList()) {
                    if (value.toMap().value("id") == target && value.toMap().value("pending").toBool()) {
                        change = false;
                        Access::feed(f.backend, target == "nb2.depth" ? "NBT0" : "NR2");
                    }
                }
            });
        const qsizetype count = f.writes.size();
        const bool dispatched = f.set(target, 5);
        const QList<QByteArray> expected = target == "nb2.depth" ? QList<QByteArray>{"NL2;"} : QList<QByteArray>{"RL2;"};
        require(!dispatched && f.writes.mid(count) == expected,
            "synchronous prerequisite change permits only its new-bank query, never the obsolete setter");
        QObject::disconnect(dependency);
    }
    f.seed();
    const QString nr1Context = f.status().value("context").toString();
    Access::feed(f.backend, "NR2");
    Access::feed(f.backend, "RL209");
    Access::feed(f.backend, "NR1");
    Access::feed(f.backend, "RL105");
    require(f.row("nr1.level").value("editable").toBool() && !f.set("nr1.level", 6, nr1Context),
        "NR1-to-NR2-to-NR1 cycle cannot requalify a delayed old descriptor request");
    const QString nbTypeContext = f.status().value("context").toString();
    Access::feed(f.backend, "NBT0");
    Access::feed(f.backend, "NL2003");
    Access::feed(f.backend, "NBT1");
    Access::feed(f.backend, "NL2007");
    require(f.row("nb2.level").value("editable").toBool() && !f.set("nb2.level", 5, nbTypeContext),
        "NB2 type cycle cannot requalify a request for the old NL2 bank");
    const quint64 oldBatch = Access::feed(f.backend, "AP140");
    const QString cwContext = f.status().value("context").toString();
    Access::feed(f.backend, "SF0000140740002"); // USB
    require(!f.row("apf.shift").contains("value") && !f.row("apf.shift").value("editable").toBool(),
        "mode transition retires old mode's native values");
    Access::stale(f.backend, "AP180", oldBatch);
    require(!f.row("apf.shift").contains("value") && !f.set("apf.enabled", 1, cwContext),
        "out-of-order prior-context observation/request cannot revive retired state");
    Access::feed(f.backend, "BC2");
    require(f.genericAnf && f.row("bc.mode").value("value") == 2 && f.set("bc.mode", 1),
        "BC2 variant is visible while generic ANF remains coherent");
    Access::feed(f.backend, "BC1");
    Access::feed(f.backend, "SF0000140740004"); // FM
    Access::feed(f.backend, "NR1");
    require(!f.row("nb1.enabled").value("editable").toBool()
        && !f.row("notch.enabled").value("editable").toBool() && !f.set("nr.mode", 2),
        "FM rejects NB/notch and NR2 while preserving NR1");
    Access::feed(f.backend, "SF0000140740006"); // RTTY
    Access::feed(f.backend, "AP00");
    require(!f.row("apf.enabled").value("editable").toBool(), "unknown FSK spacing cannot enable APF");
    Access::feed(f.backend, "EX00206 000");
    Access::feed(f.backend, "AP00");
    require(f.row("apf.enabled").value("editable").toBool() && f.set("apf.enabled", 2),
        "confirmed 170 Hz FSK spacing permits native APF");
    Access::feed(f.backend, "AP01");
    Access::feed(f.backend, "EX00206 001");
    Access::feed(f.backend, "AP00");
    require(!f.row("apf.enabled").value("editable").toBool() && !f.set("apf.enabled", 2),
        "200 Hz FSK spacing rejects APF despite a known parameter value");
    Access::feed(f.backend, "SF0000140740003");
    f.seed();
    bool withdraw = true;
    const QMetaObject::Connection reentry = QObject::connect(&f.backend, &IRadioBackend::extensionStatus, &f.backend,
        [&](const QString&, const QString& kind, const QVariantMap& status) {
            if (withdraw && kind == "receiveControls") {
                for (const QVariant& value : status.value("controls").toList()) {
                    if (value.toMap().value("id") == "apf.enabled" && value.toMap().value("pending").toBool()) {
                        withdraw = false;
                        Access::withdrawRx(f.backend);
                    }
                }
            }
        });
    const qsizetype reentryStart = f.writes.size();
    require(!f.set("apf.enabled", 2) && f.writes.size() == reentryStart,
        "synchronous status callback withdrawing RX prevents setter handoff");
    QObject::disconnect(reentry);
    Access::feed(f.backend, "RX");
    f.seed();
    Access::feed(f.backend, "TS1");
    require(!f.status().value("editable").toBool() && !f.row("apf.enabled").contains("value"),
        "TF-SET context invalidates native receive editing");
    Access::feed(f.backend, "TS0");
    Access::feed(f.backend, "SL006");
    Access::feed(f.backend, "SH0080");
    QVariantMap filter;
    QObject::connect(&f.backend, &IRadioBackend::extensionResult, &f.backend,
        [&](quint64 ticket, const QVariant& result) { if (ticket == 99) { filter = result.toMap(); } });
    const auto filterStatus = [&] {
        f.backend.invokeExtension("kenwood", "filter.get", 99);
        return filter;
    };
    const auto refuseFilter = [&] {
        const QVariantMap before = filterStatus();
        const qsizetype count = f.writes.size();
        f.backend.setSliceFilter(0, -100, 100);
        f.backend.invokeExtension("kenwood", "filter.set", 100,
            QVariantMap{{"context", before.value("context")}, {"axis", "low"}, {"index", 1}});
        require(f.writes.size() == count && !filterStatus().value("editable").toBool()
            && filterStatus().value("axes") == before.value("axes"),
            "unsafe filter edits are refused while confirmed axes remain visible");
    };
    Access::withdrawRx(f.backend);
    refuseFilter();
    Access::feed(f.backend, "RX");
    Access::feed(f.backend, "TS1");
    refuseFilter();
    Access::feed(f.backend, "TS0");
    Access::keyIntent(f.backend, true);
    refuseFilter();
    Access::keyIntent(f.backend, false);
    f.backend.disconnectRadio();
    require(!f.status().value("editable").toBool() && !f.row("apf.enabled").contains("value"),
        "disconnect retires all native control values");
    Fixture lateIdentity;
    Access::authenticate(lateIdentity.backend);
    lateIdentity.refresh(); // Page subscribed before identity confirmation.
    for (const QByteArray& message : {QByteArray("FR0"), QByteArray("FT0"), QByteArray("TS0"),
         QByteArray("SF0000140740003"), QByteArray("RX")}) { Access::feed(lateIdentity.backend, message); }
    require(!lateIdentity.writes.contains("AP0;") && !lateIdentity.status().value("editable").toBool(),
        "out-of-order RX/mode state cannot query/edit before TS-890S identity");
    Access::feed(lateIdentity.backend, "ID024");
    require(lateIdentity.writes.contains("AP0;") && lateIdentity.writes.contains("NBD;")
        && lateIdentity.status().value("editable").toBool(), "ID arriving last converges subscribed native controls without manual refresh");

    Fixture matrix;
    matrix.connectCw();
    const QList<QPair<QString, char>> modes{{"LSB", '1'}, {"USB", '2'}, {"CW", '3'}, {"FM", '4'},
        {"AM", '5'}, {"RTTY", '6'}, {"CWR", '7'}, {"RTTYR", '9'}, {"PSK", 'A'}, {"PSKR", 'B'},
        {"DIGL", 'C'}, {"DIGU", 'D'}, {"DFM", 'E'}, {"DAM", 'F'}};
    for (const QPair<QString, char>& mode : modes) {
        Access::feed(matrix.backend, QByteArray("SF000014074000") + mode.second);
        const bool fm = mode.first == "FM" || mode.first == "DFM";
        const bool cw = mode.first == "CW" || mode.first == "CWR";
        const bool fsk = mode.first == "RTTY" || mode.first == "RTTYR";
        const bool voice = mode.first == "LSB" || mode.first == "USB" || mode.first == "AM"
            || mode.first == "FM" || mode.first == "DIGL" || mode.first == "DIGU"
            || mode.first == "DAM" || mode.first == "DFM";
        if (fsk) {
            matrix.seed();
            require(!matrix.row("apf.enabled").value("editable").toBool(), "both FSK directions require observed spacing");
            for (int spacing = 1; spacing <= 3; ++spacing) {
                Access::feed(matrix.backend, "EX00206 000");
                Access::feed(matrix.backend, "AP00");
                require(matrix.row("apf.enabled").value("editable").toBool(),
                    "fresh 170 Hz observation enables APF before each independent spacing transition");
                Access::feed(matrix.backend, "EX00206 00" + QByteArray::number(spacing));
                Access::feed(matrix.backend, "AP00");
                require(!matrix.row("apf.enabled").value("editable").toBool(),
                    "both FSK directions reject APF for all native 200/425/850 Hz spacing choices");
            }
            Access::feed(matrix.backend, "EX00206 000");
        }
        matrix.seed();
        if (voice) { Access::feed(matrix.backend, "BC1"); }
        require(matrix.status().value("mode") == mode.first, "every official mode code reaches the actual descriptor context");
        for (const ControlCase& control : cases) {
            bool editable = true;
            if (control.id == "apf.enabled") { editable = cw || fsk; }
            else if (control.id.startsWith("apf.")) { editable = cw; }
            else if (control.id.startsWith("notch.") || control.id.startsWith("nb")) { editable = !fm; }
            else if (control.id == "bc.mode") { editable = voice; }
            else if (control.id == "nr2.time") { editable = false; } // NR1 selected by seed.
            require(matrix.row(control.id).value("editable").toBool() == editable,
                qPrintable(QStringLiteral("mode %1 / control %2 has the official availability").arg(mode.first, control.id)));
            if (!editable) {
                require(!matrix.row(control.id).value("reason").toString().isEmpty(), "unavailable controls always explain why");
            }
        }
        const QVariantList choices = matrix.row("nr.mode").value("choices").toList();
        require(choices.size() == (fm ? 2 : 3), "FM and data-FM omit the illegal NR2 choice");
        if (fm) {
            require(!matrix.set("nr.mode", 2), "both FM variants refuse NR2 at the backend");
        } else {
            Access::feed(matrix.backend, "NR2");
            Access::feed(matrix.backend, "RL209");
            require(matrix.row("nr2.time").value("editable").toBool()
                && !matrix.row("nr1.level").value("editable").toBool(), "every non-FM mode selects the correct NR2 parameter bank");
        }
        Access::feed(matrix.backend, "MO10");
        Access::feed(matrix.backend, "MO20");
        for (const auto& [id, prefix] : {std::pair{"monitor.rx", "MO1"}, std::pair{"monitor.dsp", "MO2"}}) {
            for (int value : {1, 0}) {
                matrix.writes.clear();
                require(matrix.set(id, value), "both monitor choices are supported in each native mode");
                require(matrix.writes.contains(QByteArray(prefix) + QByteArray::number(value) + ";"),
                    "each native mode uses the exact monitor command family");
                Access::feed(matrix.backend, QByteArray(prefix) + QByteArray::number(value));
                require(matrix.row(id).value("value").toInt() == value,
                    "each native mode observes the native monitor choice");
            }
        }
    }
    {
        Fixture cancelled;
        cancelled.connectCw();
        cancelled.seed();
        bool changed = false;
        QObject::connect(&cancelled.backend, &IRadioBackend::extensionStatus, &application,
            [&](const QString&, const QString& kind, const QVariantMap&) {
                if (kind == "receiveControls" && !changed
                    && cancelled.row("apf.enabled").value("pending").toBool()) {
                    changed = true;
                    Access::feed(cancelled.backend, "NR2");
                }
            });
        cancelled.writes.clear();
        require(!cancelled.set("apf.enabled", 2) && !cancelled.writes.contains("AP01;"),
            "a descriptor epoch change before dispatch cancels the obsolete APF mutation");
        require(!cancelled.row("apf.enabled").value("pending").toBool()
            && cancelled.row("apf.enabled").value("editable").toBool()
            && cancelled.row("apf.enabled").value("value").toInt() == 1,
            "an unsent cancelled request retires only its pending marker and retains confirmed state");
    }
    {
        Fixture replacement;
        replacement.connectCw();
        replacement.seed();
        bool replaced = false;
        bool newerAccepted = false;
        QObject::connect(&replacement.backend, &IRadioBackend::extensionStatus, &application,
            [&](const QString&, const QString& kind, const QVariantMap&) {
                if (kind == "receiveControls" && !replaced
                    && replacement.row("apf.enabled").value("pending").toBool()) {
                    replaced = true;
                    Access::feed(replacement.backend, "AP00");
                    newerAccepted = replacement.set("apf.enabled", 2);
                }
            });
        replacement.writes.clear();
        require(!replacement.set("apf.enabled", 2) && newerAccepted
            && replacement.writes.count("AP01;") == 1,
            "a reentrant same-control replacement owns the single dispatched mutation");
        require(replacement.row("apf.enabled").value("pending").toBool()
            && !replacement.row("apf.enabled").value("editable").toBool(),
            "retiring an obsolete request cannot clear the newer request's pending marker");
        Access::feed(replacement.backend, "AP01");
        require(!replacement.row("apf.enabled").value("pending").toBool()
            && replacement.row("apf.enabled").value("value").toInt() == 2,
            "fresh readback completes the replacement request");
    }
    {
        Fixture monitor;
        monitor.connectCw();
        monitor.writes.clear();
        monitor.refresh();
        require(monitor.writes.contains("MO1;") && monitor.writes.contains("MO2;"),
            "explicit receive refresh queries both native monitor domains");
        Access::feed(monitor.backend, "MO10");
        Access::feed(monitor.backend, "MO20");
        for (const auto& [id, prefix] : {std::pair{"monitor.rx", "MO1"}, std::pair{"monitor.dsp", "MO2"}}) {
            for (int value : {1, 0}) {
                monitor.writes.clear();
                const int previous = monitor.row(id).value("value").toInt();
                require(monitor.set(id, value), "native monitor accepts both explicit choices");
                require(monitor.writes.contains(QByteArray(prefix) + QByteArray::number(value) + ";")
                    && monitor.writes.contains(QByteArray(prefix) + ";"), "native monitor writes exact domain and reads back");
                require(monitor.row(id).value("value").toInt() == previous,
                    "native monitor does not optimistically publish requested value");
                Access::feed(monitor.backend, QByteArray(prefix) + QByteArray::number(value));
                require(monitor.row(id).value("value").toInt() == value
                    && !monitor.row(id).value("pending").toBool(), "native monitor converges on actual observation");
            }
            monitor.writes.clear();
            require(!monitor.set(id, -1) && !monitor.set(id, 2) && !monitor.set(id, true)
                && !monitor.set(id, 1.0) && !monitor.set(id, QStringLiteral("1")),
                "native monitor rejects invalid domains and coercions");
            require(monitor.writes.isEmpty(), "invalid monitor values emit no CAT");
            require(!monitor.row(id).value("resettable").toBool(), "native monitors have no invented reset");
        }
        const quint64 old = Access::feed(monitor.backend, "MO10");
        Access::feed(monitor.backend, "TS1");
        Access::feed(monitor.backend, "TS0");
        Access::stale(monitor.backend, "MO11", old);
        require(!monitor.row("monitor.rx").contains("value"), "old-context monitor reply cannot restore authority");
    }
    std::cout << "Kenwood native receive controls checks passed\n";
}
