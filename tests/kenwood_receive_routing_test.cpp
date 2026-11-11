#include "TestSettingsProfile.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QMap>
#include <QPointer>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>

namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& b) {
        b.m_clock.start(); b.m_protocol.start("fixture-user", "fixture-password", false, 0);
        b.m_protocol.receive("##CN1;", 1); b.m_protocol.receive("##ID1;", 2); b.ready();
    }
    static void message(KenwoodBackend& b, const QByteArray& bytes) { b.adopt(bytes); }
    static void batch(KenwoodBackend& b, const QList<QByteArray>& bytes) {
        const quint64 batch = ++b.m_controlIngressSequence;
        for (const QByteArray& value : bytes) { b.adopt(value, batch); }
    }
    static void sameBatch(KenwoodBackend& b, const QByteArray& value) { b.adopt(value, b.m_controlIngressSequence); }
    static void intent(KenwoodBackend& b, bool active) { b.m_keyIntent = active; b.adoptReceiveRoutingContext(); }
    static std::optional<bool> drive(const KenwoodBackend& b) { return b.m_driveOutput; }
    static void extraIntent(KenwoodBackend& b, bool cw) {
        if (cw) { b.m_cwIntent = true; } else { b.m_atuIntent = true; }
        b.adoptReceiveRoutingContext();
    }
    static void tune(KenwoodBackend& b, bool active) {
        if (active) { b.m_tune.emplace(); } else { b.m_tune.reset(); }
        ++b.m_tuneSerial; b.adoptReceiveRoutingContext();
    }
};
}
namespace {
using namespace AetherSDR;
using namespace AetherSDR::kenwood;
void require(bool value, const char* message) { if (!value) { std::cerr << message << '\n'; std::exit(1); } }
struct Fixture {
    QList<QByteArray> commands;
    QVariantMap state;
    QMap<quint64, QVariant> results;
    QMap<quint64, QString> errors;
    std::function<void(const QByteArray&)> onWrite;
    bool accept = true;
    std::unique_ptr<KenwoodBackend> backend;
    quint64 sequence = 1;
    Fixture() {
        backend = std::make_unique<KenwoodBackend>(KenwoodBackend::Transport{[this](const QByteArray& bytes) {
            commands.append(bytes); const bool accepted = accept; const auto callback = onWrite;
            if (callback) { callback(bytes); } return accepted;
        }, {}});
        QObject::connect(backend.get(), &IRadioBackend::extensionStatus, backend.get(),
            [this](const QString&, const QString& kind, const QVariantMap& value) { if (kind == "receiveRouting") { state = value; } });
        QObject::connect(backend.get(), &IRadioBackend::extensionResult, backend.get(), [this](quint64 id, const QVariant& value) {
            results.insert(id, value); if (value.typeId() == QMetaType::QVariantMap && value.toMap().contains("operations")) { state = value.toMap(); }
        });
        QObject::connect(backend.get(), &IRadioBackend::extensionError, backend.get(), [this](quint64 id, const QString& error) { errors.insert(id, error); });
    }
    void observe(const QByteArray& value) { KenwoodBackendTestAccess::message(*backend, value); }
    void mode(char code) { observe(QByteArray("OM0") + code); observe(QByteArray("SF000014074000") + code); }
    void ready() {
        KenwoodBackendTestAccess::authenticate(*backend);
        for (const QByteArray& value : {QByteArray("ID024"), QByteArray("FR0"), QByteArray("FT1"), QByteArray("FA00014074000"),
            QByteArray("FB00007074000"), QByteArray("OM03"), QByteArray("SF0000140740003"), QByteArray("SF1000070740003"), QByteArray("TS0"), QByteArray("RX")}) { observe(value); }
    }
    quint64 get(const QVariant& argument = {}) { const quint64 id = sequence++; backend->invokeExtension("kenwood", "receive.routing.get", id, argument); return id; }
    void load() { observe("AN1000"); }
    QVariantMap operation(const QString& id) const { return state.value("operations").toMap().value(id).toMap(); }
    QVariantMap arguments() const { return QVariantMap{{"context", state.value("context")}}; }
    quint64 request(const QString& verb, const QVariantMap& fields) {
        const quint64 id = sequence++; backend->invokeExtension("kenwood", "receive.routing." + verb, id, fields); return id;
    }
    quint64 set(const QString& field, const QVariant& value) { auto fields = arguments(); fields.insert("value", value); return request(field + ".set", fields); }

};
void modesAndTuples() {
    for (char mode : QByteArray("12345679ABCDEF")) {
        Fixture f; f.ready(); f.mode(mode); f.get(); f.load();
        for (const QString& field : {QString("rxAntenna"), QString("externalOutput")}) {
            for (bool value : {false, true}) {
                f.load(); f.commands.clear(); f.set(field, value);
                const QByteArray command = field == "rxAntenna" ? (value ? "AN9199;" : "AN9099;") : (value ? "AN9991;" : "AN9990;");
                require(f.commands == QList<QByteArray>{command, "AN;"}, "all14 modes isolate exact requested routing bit with preserve9");
                require(!f.state.value(field).toBool(), "routing value stays last confirmed while pending");
                const QByteArray reply = field == "rxAntenna" ? (value ? "AN1100" : "AN1000") : (value ? "AN1001" : "AN1000");
                f.observe(reply); require(f.state.value(field).toBool() == value && !f.operation(field).value("pending").toBool(), "fresh AN owns routing value");
            }
        }
        for (int main = 1; main <= 2; ++main) { for (int rx = 0; rx <= 1; ++rx) { for (int drive = 0; drive <= 1; ++drive) { for (int external = 0; external <= 1; ++external) {
            f.observe("AN" + QByteArray::number(main) + QByteArray::number(rx) + QByteArray::number(drive) + QByteArray::number(external));
            require(f.state.value("rxAntenna").toBool() == bool(rx) && f.state.value("externalOutput").toBool() == bool(external), "all16 complete AN tuples validated independently");
        } } } }
    }
}
void malformedAndFrequency() {
    Fixture f; f.ready(); f.get(); f.load();
    for (int position = 2; position < 6; ++position) { for (char bad : QByteArray("239x")) {
        if (position == 2 && bad == '2') { continue; }
        QByteArray report("AN1000"); report[position] = bad; f.observe(report);
        require(!f.state.value("rxAntenna").toBool() && !f.state.value("externalOutput").toBool(), "any invalid field rejects entire AN snapshot");
    } }
    for (const QByteArray& bad : {QByteArray("AN100"), QByteArray("AN10000"), QByteArray("AN9000"), QByteArray("AN1191")}) { f.observe(bad); }
    for (const QVariant& bad : {QVariant(0), QVariant(1), QVariant("true"), QVariant(1.0), QVariant()}) {
        f.commands.clear(); require(f.errors.contains(f.set("rxAntenna", bad)) && f.commands.isEmpty(), "routing setter accepts strict bool only");
    }
    auto old = f.arguments(); old.insert("value", true); f.commands.clear(); f.observe("FA00014075000");
    require(f.commands == QList<QByteArray>{"AN;"} && !f.state.contains("rxAntenna") && !f.state.contains("externalOutput"), "changed exactfrequency withdraws native authority and queriesAN once");
    f.commands.clear(); require(f.errors.contains(f.request("rxAntenna.set", old)) && f.commands.isEmpty(), "previous frequency context cannot dispatch");
    f.observe("AN1111"); require(f.operation("rxAntenna").value("editable").toBool(), "fresh currentfrequency AN restores controls");
    f.commands.clear(); f.observe("FA00014075000"); require(f.commands.isEmpty(), "unchanged frequency poll does not refresh or retire");
    f.observe("FB00007075000"); require(f.commands.isEmpty(), "inactive VFO frequency does not retire routing");
    f.observe("SF0000140760003"); require(f.commands == QList<QByteArray>{"AN;"} && !f.state.contains("externalOutput"), "SF actual frequency also invalidates native routing");
}
void pendingOwnershipAndReentry() {
    Fixture f; f.ready(); f.get(); f.load(); bool nested = false;
    QObject::connect(f.backend.get(), &IRadioBackend::extensionStatus, f.backend.get(), [&](const QString&, const QString& kind, const QVariantMap&) {
        if (kind != "receiveRouting" || nested || !f.operation("rxAntenna").value("pending").toBool()) { return; }
        nested = true; f.get(); f.observe("AN1000");
    });
    f.commands.clear(); f.set("rxAntenna", true);
    require(f.commands == QList<QByteArray>{"AN9199;", "AN;"}, "predispatch GET and old report preserve exact producer without earlyquery");
    KenwoodBackendTestAccess::sameBatch(*f.backend, "AN1101");
    require(f.operation("rxAntenna").value("pending").toBool() && !f.state.value("rxAntenna").toBool() && !f.state.value("externalOutput").toBool(), "same ingress rejects entire stale AN without acknowledgement");
    f.observe("AN1100"); require(!f.operation("rxAntenna").value("pending").toBool(), "later AN settles producer");
    f.load(); f.commands.clear(); f.set("rxAntenna", true); f.set("externalOutput", true);
    require(f.commands == QList<QByteArray>{"AN9199;", "AN;", "AN9991;", "AN;"}, "independent routing fields never replay cached companions");
    f.observe("AN1101"); require(!f.operation("externalOutput").value("pending").toBool(), "complete fresh report settles both fields");
    Fixture changed; changed.ready(); changed.get(); changed.load(); bool replaced = false;
    QObject::connect(changed.backend.get(), &IRadioBackend::extensionStatus, changed.backend.get(), [&](const QString&, const QString& kind, const QVariantMap&) {
        if (kind != "receiveRouting" || replaced || !changed.operation("rxAntenna").value("pending").toBool()) { return; }
        replaced = true; changed.observe("AN1100"); changed.set("rxAntenna", false);
    });
    changed.commands.clear(); changed.set("rxAntenna", true);
    require(changed.commands == QList<QByteArray>{"AN9099;", "AN;"} && changed.operation("rxAntenna").value("pending").toBool(), "changed external report supersedes old producer without losing nested replacement");
}
void sourceBatchAndNestedObservation() {
    Fixture f; f.ready(); f.get(); f.load(); int antennaPublications = 0;
    QObject::connect(f.backend.get(), &IRadioBackend::sliceChanged, f.backend.get(), [&](int, const SliceDelta& delta) {
        if (delta.rxAntenna && !delta.rxAntenna->isEmpty()) { ++antennaPublications; }
    });
    KenwoodBackendTestAccess::batch(*f.backend, {"FA00014075000", "AN1111"});
    require(!f.state.contains("rxAntenna") && antennaPublications == 0, "frequency-transition ingress cannot adopt old AN into native or shared state");
    f.observe("AN1000"); require(antennaPublications == 1 && f.state.contains("rxAntenna"), "later complete AN requalifies both paths");
    bool nested = false;
    QObject::connect(f.backend.get(), &IRadioBackend::extensionStatus, f.backend.get(), [&](const QString&, const QString& kind, const QVariantMap& state) {
        if (kind != "receiveRouting" || nested || !state.value("rxAntenna").toBool()) { return; }
        nested = true; f.observe("AN1001");
    });
    QList<QString> shared;
    QObject::connect(f.backend.get(), &IRadioBackend::sliceChanged, f.backend.get(), [&](int, const SliceDelta& delta) { if (delta.rxAntenna && !delta.rxAntenna->isEmpty()) { shared.append(*delta.rxAntenna); } });
    f.observe("AN1100");
    require(shared == QList<QString>{"ANT1"} && !f.state.value("rxAntenna").toBool() && f.state.value("externalOutput").toBool(), "nested newer native report prevents outer stale shared publication");
}

void coalescedStartupWithoutNativeInterest() {
    Fixture f; KenwoodBackendTestAccess::authenticate(*f.backend); f.observe("ID024"); f.commands.clear();
    QString receive, transmit;
    QObject::connect(f.backend.get(), &IRadioBackend::sliceChanged, f.backend.get(), [&](int, const SliceDelta& delta) {
        if (delta.rxAntenna && !delta.rxAntenna->isEmpty()) { receive = *delta.rxAntenna; }
        if (delta.txAntenna) { transmit = *delta.txAntenna; }
    });
    KenwoodBackendTestAccess::batch(*f.backend, {"FR0", "FT1", "FA00014074000", "FB00007074000", "SF0000140740003", "SF1000070740003", "TS0", "RX", "AN2111"});
    const bool adopted = receive == "RX-ANT" && transmit == "ANT2" && KenwoodBackendTestAccess::drive(*f.backend) == true;
    require(adopted || f.commands.count("AN;") == 1, "coalesced startup without native interest must adopt shared AN or request one bounded fresh AN");
    f.observe("AN2111");
    require(receive == "RX-ANT" && transmit == "ANT2" && KenwoodBackendTestAccess::drive(*f.backend) == true && f.state.value("rxAntenna").toBool() && f.state.value("externalOutput").toBool(), "fresh startup AN eventually qualifies shared and native routing");
}

void boundedStartupRecovery() {
    Fixture f; KenwoodBackendTestAccess::authenticate(*f.backend); f.observe("ID024"); f.commands.clear();
    f.onWrite = [&](const QByteArray& command) { if (command == "AN;") { KenwoodBackendTestAccess::sameBatch(*f.backend, "AN2111"); } };
    KenwoodBackendTestAccess::batch(*f.backend, {"FR0", "FT1", "FA00014074000", "SF0000140740003", "TS0", "RX", "AN2111", "AN2111"});
    require(f.commands.count("AN;") == 1, "same-ingress startup recovery is bounded despite synchronous duplicate replies");
    f.onWrite = {}; f.observe("AN2111");
    require(f.state.value("rxAntenna").toBool(), "later recovery response qualifies routing");
    Fixture interested; interested.ready(); interested.get(); interested.load(); interested.commands.clear();
    KenwoodBackendTestAccess::batch(*interested.backend, {"FA00014075000", "AN2111", "AN2111"});
    require(interested.commands.count("AN;") == 1, "normal interested context query also satisfies recovery without duplicate");
    Fixture resumed; resumed.ready(); resumed.get(); resumed.load(); resumed.commands.clear(); bool advanced = false;
    QObject::connect(resumed.backend.get(), &IRadioBackend::capabilitiesChanged, resumed.backend.get(), [&] {
        if (advanced) { return; } advanced = true; resumed.observe("FA00014075000");
    });
    KenwoodBackendTestAccess::batch(*resumed.backend, {"SF0000140740003", "AN2111", "AN2111"});
    require(resumed.commands.count("AN;") == 1, "newer context query covers older resumed ingress without duplicate recovery");

    Fixture pending; pending.ready(); pending.get(); pending.load(); pending.commands.clear(); pending.set("rxAntenna", true);
    const int requests = pending.commands.count("AN;");
    KenwoodBackendTestAccess::sameBatch(*pending.backend, "AN2111");
    require(pending.commands.count("AN;") == requests && pending.operation("rxAntenna").value("pending").toBool(), "producer stale rejection never creates query feedback loop");
    bool nested = false; QList<quint64> delivered;
    QObject::connect(interested.backend.get(), &IRadioBackend::extensionStatus, interested.backend.get(), [&](const QString&, const QString& kind, const QVariantMap&) {
        if (kind == "receiveRouting" && !nested) { nested = true; interested.observe("AN1000"); }
    });
    QObject::connect(interested.backend.get(), &IRadioBackend::extensionStatus, interested.backend.get(), [&](const QString&, const QString& kind, const QVariantMap& state) {
        if (kind == "receiveRouting") { delivered.append(state.value("statusRevision").toString().toULongLong()); }
    });
    interested.observe("AN1111");
    require(delivered.size() == 2 && delivered[0] > delivered[1], "producer stamps outer snapshot before callback so nested newer status is distinguishable");
}

void reentrantContextRetirementPublication() {
    Fixture f; f.ready(); f.get(); f.observe("AN1111"); bool nested = false;
    QObject::connect(f.backend.get(), &IRadioBackend::capabilitiesChanged, f.backend.get(), [&] {
        if (nested) { return; }
        nested = true; f.accept = false; f.observe("AN1111");
    });
    f.commands.clear(); f.observe("SF0000140750003");
    require(nested && !f.state.contains("rxAntenna") && !f.state.contains("externalOutput")
        && !f.operation("rxAntenna").value("editable").toBool(), "inner AN context retirement publishes unknown even if recovery read fails before outer tail");
}

void earlyStartupReports() {
    for (bool antennaBeforeTf : {false, true}) {
        Fixture f; KenwoodBackendTestAccess::authenticate(*f.backend); f.observe("ID024"); f.commands.clear();
        QList<QByteArray> chunk{"FR0", "FT1", "FA00014074000", "SF0000140740003"};
        if (antennaBeforeTf) { chunk.append("AN2111"); chunk.append("TS0"); chunk.append("RX"); }
        else { chunk.append("TS0"); chunk.append("AN2111"); chunk.append("RX"); }
        KenwoodBackendTestAccess::batch(*f.backend, chunk);
        require(f.commands.count("AN;") == 1, "early startup AN rejection is refreshed once RX and TF authority becomes ready");
        f.observe("AN2111"); require(f.state.value("rxAntenna").toBool(), "early report recovery eventually confirms native state");
    }
}
void freshSharedObservationSurvivesFrequencyPublication() {
    for (bool sf : {false, true}) {
        RadioModel model; QList<QByteArray> commands;
        auto owned = std::make_unique<KenwoodBackend>(KenwoodBackend::Transport{[&](const QByteArray& bytes) { commands.append(bytes); return true; }, {}});
        KenwoodBackend* backend = owned.get(); model.setBackendForTest(std::move(owned), QStringLiteral("kenwood"));
        KenwoodBackendTestAccess::authenticate(*backend);
        for (const QByteArray& bytes : {QByteArray("ID024"), QByteArray("FR0"), QByteArray("FT0"), QByteArray("FA00014074000"), QByteArray("SF0000140740003"), QByteArray("TS0"), QByteArray("RX"), QByteArray("AN1100")}) { KenwoodBackendTestAccess::message(*backend, bytes); }
        bool nested = false;
        const auto adoptFresh = [&] {
            if (nested) { return; } nested = true;
            KenwoodBackendTestAccess::message(*backend, "AN1100");
            KenwoodBackendTestAccess::message(*backend, "AN1100");
        };
        if (sf) { QObject::connect(backend, &IRadioBackend::capabilitiesChanged, &model, adoptFresh); }
        else { QObject::connect(backend, &IRadioBackend::transmitChanged, &model, [&](const TransmitDelta&) { adoptFresh(); }); }
        KenwoodBackendTestAccess::message(*backend, sf ? "SF0000140750003" : "FA00014075000");
        require(nested && model.slice(0), "frequency publication invokes real model reentrant observation route");
        commands.clear(); model.slice(0)->setRxAntenna("RX-ANT");
        require(commands.isEmpty(), "fresh currentfrequency AN remains known after outer physicalfrequency publication");
    }
}

void guardsFailuresAndLifetime() {
    for (int kind = 0; kind < 8; ++kind) {
        Fixture f; f.ready(); f.get(); f.load();
        if (kind == 0) { f.observe("TX0"); } if (kind == 1) { f.observe("TS1"); }
        if (kind == 2) { KenwoodBackendTestAccess::intent(*f.backend, true); }
        if (kind == 3) { KenwoodBackendTestAccess::tune(*f.backend, true); }
        if (kind == 4) { f.observe("FR1"); } if (kind == 5) { f.backend->disconnectRadio(); }
        if (kind == 6) { KenwoodBackendTestAccess::extraIntent(*f.backend, true); }
        if (kind == 7) { KenwoodBackendTestAccess::extraIntent(*f.backend, false); }
        f.commands.clear(); require(f.errors.contains(f.set("rxAntenna", true)) && f.commands.isEmpty(), "unsafe or unqualified routing dispatch rejected");
    }
    Fixture cold; cold.get(); cold.commands.clear(); require(cold.errors.contains(cold.set("externalOutput", true)) && cold.commands.isEmpty(), "unknown session cannot route");
    Fixture failed; failed.ready(); failed.get(); failed.load(); failed.accept = false; failed.commands.clear();
    require(failed.errors.contains(failed.set("externalOutput", true)) && !failed.state.contains("externalOutput") && failed.state.contains("rxAntenna"), "transport rejection withdraws only affected field authority");
    failed.accept = true; failed.commands.clear(); failed.get(); require(failed.commands == QList<QByteArray>{"AN;"}, "explicit GET recovers failed dispatch");
    Fixture removed; removed.ready(); removed.get(); removed.load(); QPointer<KenwoodBackend> guard(removed.backend.get()); removed.commands.clear();
    removed.onWrite = [&](const QByteArray& command) { if (command == "AN9199;") { removed.backend.reset(); } };
    removed.set("rxAntenna", true); require(!guard && removed.commands == QList<QByteArray>{"AN9199;"}, "transport deletion prevents followup query");
}
}
int main(int argc, char** argv) {
    TestSettingsProfile profile(QStringLiteral("kenwood-receive-routing")); QCoreApplication app(argc, argv);
    require(profile.isValid(), "isolated settings"); modesAndTuples(); malformedAndFrequency(); pendingOwnershipAndReentry(); sourceBatchAndNestedObservation(); coalescedStartupWithoutNativeInterest(); boundedStartupRecovery(); reentrantContextRetirementPublication(); earlyStartupReports(); freshSharedObservationSurvivesFrequencyPublication(); guardsFailuresAndLifetime(); return 0;
}
