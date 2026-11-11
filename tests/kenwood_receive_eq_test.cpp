#include "TestSettingsProfile.h"
#include "core/backends/kenwood/KenwoodBackend.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QList>
#include <QMap>
#include <QMetaType>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
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
    static QVariantMap status(const KenwoodBackend& b) { return b.receiveEqStatus(); }
    static void batch(KenwoodBackend& b, const QList<QByteArray>& bytes) {
        const quint64 batch = ++b.m_controlIngressSequence;
        for (const QByteArray& value : bytes) { b.adopt(value, batch); }
    }
    static void intent(KenwoodBackend& b, bool active) { b.m_keyIntent = active; b.adoptReceiveEq({}, ++b.m_controlIngressSequence); }
    static void tune(KenwoodBackend& b, bool active) {
        if (active) { b.m_tune.emplace(); } else { b.m_tune.reset(); }
        ++b.m_tuneSerial; b.adoptReceiveEq({}, ++b.m_controlIngressSequence);
    }
};
}
namespace {
using namespace AetherSDR;
using namespace AetherSDR::kenwood;
void require(bool value, const char* message) { if (!value) { std::cerr << message << '\n'; std::exit(1); } }
QVariantList gradient() { QVariantList values; for (int band = 0; band < 18; ++band) { values.append(band); } return values; }
QByteArray wireCurve(const QVariantList& raw) {
    QByteArray bytes("UR"); for (const QVariant& value : raw) { bytes += QByteArray::number(value.toInt()).rightJustified(2, '0'); } return bytes;
}
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
            [this](const QString&, const QString& kind, const QVariantMap& value) { if (kind == "receiveEq") { state = value; } });
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
    quint64 get(const QVariant& argument = {}) { const quint64 id = sequence++; backend->invokeExtension("kenwood", "receive.eq.get", id, argument); return id; }
    void readbacks(int effect = 0, const QVariantList& raw = gradient()) { observe("EQR00"); observe("EQR1" + QByteArray::number(effect)); observe(wireCurve(raw)); }
    QVariantMap operation(const QString& id) const { return state.value("operations").toMap().value(id).toMap(); }
    quint64 request(const QString& verb, QVariantMap arguments) {
        const quint64 id = sequence++; backend->invokeExtension("kenwood", verb, id, arguments); return id;
    }
    quint64 edit(const QString& suffix, const QVariant& value) {
        QVariantMap arguments{{"context", state.value("context")}};
        if (suffix == "enabled.set") { arguments.insert("enabled", value); }
        else if (suffix == "effect.set") { arguments.insert("effect", value); }
        else {
            arguments.insert("curveRevision", state.value("curveRevision"));
            arguments.insert(suffix == "copy" ? "destination" : suffix == "curve.reset" ? "bands" : "curveRaw", value);
        }
        return request(QStringLiteral("receive.eq.") + suffix, arguments);
    }
    QVariantMap inspect(const QString& token) { return results.value(request("receive.eq.inspection.get", {{"copyToken", token}})).toMap(); }
};
void modesAndChoices() {
    Fixture f; f.ready(); f.commands.clear(); f.get();
    require(f.commands == QList<QByteArray>{"EQR0;", "EQR1;"}, "initial query reads scalars without querying an unowned curve");
    f.observe("UR" + QByteArray("06").repeated(18));
    require(!f.state.contains("curveRaw"), "curve requires confirmed effect ownership");
    f.readbacks();
    for (char mode : QByteArray("12345679ABCDEF")) {
        f.mode(mode); f.get(); f.readbacks();
        require(f.operation("enabled").value("editable").toBool() && f.operation("curve").value("editable").toBool(), "all fourteen native RX modes remain eligible without TX EQ exclusions");
        for (bool enabled : {true, false}) {
            f.commands.clear(); const bool old = f.state.value("enabled").toBool();
            const quint64 id = f.edit("enabled.set", enabled);
            require(f.results.value(id).toBool() && f.commands == QList<QByteArray>{enabled ? "EQR01;" : "EQR00;", "EQR0;"}, "enable uses exact native setter and readback");
            require(f.state.value("enabled").toBool() == old && f.operation("enabled").value("pending").toBool(), "enable remains radio authoritative while pending");
            f.observe(enabled ? "EQR01" : "EQR00");
        }
        for (int effect = 0; effect < 9; ++effect) {
            f.commands.clear(); const int old = f.state.value("effect").toInt(); f.edit("effect.set", effect);
            require(f.commands == QList<QByteArray>{"EQR1" + QByteArray::number(effect) + ';', "EQR1;", "EQR0;"}, "all nine preset choices query effect and possible enable side effect");
            require(f.state.value("effect").toInt() == old && !f.operation("curve").value("editable").toBool(), "effect pending never retargets old curve");
            const QVariantList oldCurve = f.state.value("curveRaw").toList();
            f.observe(wireCurve(gradient()));
            require(f.state.value("curveRaw").toList() == oldCurve && !f.operation("curve").value("editable").toBool(), "UR cannot qualify before selected effect confirmation");
            KenwoodBackendTestAccess::batch(*f.backend, {"EQR1" + QByteArray::number(effect), wireCurve(gradient())});
            require(!f.operation("curve").value("editable").toBool(), "same-batch curve cannot restore effect ownership");
            f.observe(wireCurve(gradient()));
            require(f.state.value("effect").toInt() == effect && f.operation("curve").value("editable").toBool(), "later curve readback qualifies exact selected effect");
        }
    }
}
void curvesAndResets() {
    Fixture f; f.ready(); f.get(); f.readbacks();
    for (int value = 0; value <= 30; ++value) {
        QVariantList raw; for (int band = 0; band < 18; ++band) { raw.append(value); }
        f.commands.clear(); f.edit("curve.set", raw);
        require(f.commands == QList<QByteArray>{wireCurve(raw) + ';', "UR;"} && f.commands.first().size() == 39, "all native raw values retain exact full-vector decimal precision");
        f.observe(wireCurve(raw));
        require(f.state.value("curveRaw").toList() == raw, "confirmed vector adopts all eighteen exact native settings");
    }
    f.observe(wireCurve(gradient()));
    for (int band = 0; band < 18; ++band) {
        QVariantList expected = gradient(); expected[band] = 99;
        f.commands.clear(); f.edit("curve.reset", QVariantList{band});
        require(f.commands == QList<QByteArray>{wireCurve(expected) + ';', "UR;"}, "single-band reset preserves other seventeen raw fields exactly");
        f.observe(wireCurve(expected));
        require(f.operation("reset").value("pending").toBool() && f.state.value("curveRaw").toList() == gradient(), "99 answer is rejected without inventing reset defaults");
        f.observe(wireCurve(gradient()));
    }
    QVariantList all; for (int band = 0; band < 18; ++band) { all.append(band); }
    f.commands.clear(); f.edit("curve.reset", all);
    require(f.commands == QList<QByteArray>{"UR" + QByteArray("99").repeated(18) + ';', "UR;"}, "reset all emits eighteen native sentinels rather than a synthetic flat curve");
    f.observe(wireCurve(gradient()));
    const QVariantMap prior = f.state;
    for (const QByteArray& invalid : {QByteArray("EQR02"), QByteArray("EQR19"), QByteArray("EQR1"), QByteArray("EQR100"),
        QByteArray("UR") + QByteArray("06").repeated(17), QByteArray("UR") + QByteArray("31").repeated(18),
        QByteArray("UR") + QByteArray("A0").repeated(18), QByteArray("UR") + QByteArray("99").repeated(18)}) { f.observe(invalid); }
    require(f.state == prior, "malformed regular answers do not overwrite confirmed EQ state");
    f.commands.clear();
    for (const QVariant& bad : {QVariant(1), QVariant("1"), QVariant(QVariantList{})}) { require(f.errors.contains(f.edit("enabled.set", bad)), "enable accepts bool only"); }
    for (const QVariant& bad : {QVariant(-1), QVariant(9), QVariant(1.0), QVariant("1")}) { require(f.errors.contains(f.edit("effect.set", bad)), "preset accepts exact native integer enum only"); }
    for (const QVariantList& bad : {QVariantList{}, QVariantList{0, 0}, QVariantList{18}, QVariantList{-1}, QVariantList{QString("0")}}) { require(f.errors.contains(f.edit("curve.reset", bad)), "reset requires nonempty unique exact band indices"); }
    QVariantList invalidRaw = gradient(); invalidRaw[3] = 99;
    require(f.errors.contains(f.edit("curve.set", invalidRaw)), "curve setter cannot smuggle native reset sentinel");
    invalidRaw = gradient(); invalidRaw[3] = 1.0;
    require(f.errors.contains(f.edit("curve.set", invalidRaw)), "full-vector setter rejects floating element types");
    require(f.errors.contains(f.get(true)), "query rejects nonempty malformed arguments");
    require(f.commands.isEmpty(), "invalid EQ requests have no radio side effects");
}
void copiesAndInspection() {
    for (int destination = 0; destination < 3; ++destination) {
        for (bool self : {false, true}) {
            Fixture f; f.ready(); f.get(); f.readbacks(self ? destination + 6 : 0);
            f.commands.clear(); const quint64 id = f.edit("copy", destination);
            const QVariantMap result = f.results.value(id).toMap(); const QString token = result.value("copyToken").toString();
            require(result.value("sent").toBool() && !result.value("verified").toBool() && !token.isEmpty()
                && f.commands == QList<QByteArray>{"EQR2" + QByteArray::number(destination) + ';', "EQR0;", "EQR1;"}, "copy to every user destination including self completes sent/unverified with scalar observation but no invented copy query or automatic selection");
            require(!f.operation("copy").value("pending").toBool() && f.operation("effect").value("editable").toBool(), "copy does not wait for nonexistent acknowledgement");
            const QVariantMap awaiting = f.inspect(token);
            require(awaiting.value("copyToken").toString() == token && awaiting.value("state") == "awaiting", "inspection correlates immutable token and needs later destination curve");
            if (!self) { f.edit("effect.set", destination + 6); f.observe("EQR1" + QByteArray::number(destination + 6)); }
            QVariantList different = gradient(); different[0] = 30; f.observe(wireCurve(different));
            require(f.inspect(token).value("state") == "differs", "concurrent destination mismatch is observed indeterminate outcome");
            f.observe(wireCurve(gradient()));
            require(f.inspect(token).value("state") == "matched", "explicit effect6/7/8 and later owned raw vector verifies captured source");
            f.commands.clear(); require(f.errors.contains(f.request("receive.eq.inspection.get", {{"copyToken", "stale"}})), "malformed or retired inspection token rejected");
            require(f.commands.isEmpty(), "inspection never hides selection, reset, restore or queries");
            f.observe("FR1"); f.observe("SF1000070740003");
            require(f.errors.contains(f.request("receive.eq.inspection.get", {{"copyToken", token}})), "physical source change retires immutable inspection record");
        }
    }
}
void copyHandoffOwnership() {
    for (bool replacement : {false, true}) {
        Fixture f; f.ready(); f.get(); f.readbacks(); bool once = false; quint64 newer = 0;
        f.onWrite = [&](const QByteArray& bytes) {
            if (!once && bytes == "EQR20;") {
                once = true; f.observe("FR1"); f.observe("SF1000070740003");
                if (replacement) {
                    f.observe("FR0"); f.observe("SF0000140740003"); f.get(); f.readbacks(2);
                    newer = f.edit("copy", 1);
                }
            }
        };
        const quint64 old = f.edit("copy", 0);
        require(once && f.errors.contains(old) && !f.results.contains(old), "copy handoff source change cannot adopt stale source snapshot as a valid copy record");
        if (replacement) {
            const QString token = f.results.value(newer).toMap().value("copyToken").toString();
            require(!token.isEmpty() && f.state.value("copyRecord").toMap().value("token").toString() == token
                && f.state.value("copyRecord").toMap().value("destination").toInt() == 1,
                    "obsolete same-base round-trip copy handoff cannot overwrite replacement immutable record");
        } else { require(!f.state.contains("copyRecord"), "retired source copy never publishes a valid old record into new source"); }
    }
}
void reentryAndLifetime() {
    for (const QString& suffix : {QString("enabled.set"), QString("effect.set"), QString("curve.set")}) {
        Fixture f; f.ready(); f.get(); f.readbacks(); bool injected = false; bool noEarlyQuery = false;
        const QByteArray old = suffix == "enabled.set" ? QByteArray("EQR00") : suffix == "effect.set" ? QByteArray("EQR10") : wireCurve(gradient());
        QObject::connect(f.backend.get(), &IRadioBackend::extensionStatus, f.backend.get(), [&](const QString&, const QString& kind, const QVariantMap&) {
            if (!injected && kind == "receiveEq") { injected = true; f.get(); f.observe(old); noEarlyQuery = f.commands.isEmpty(); }
        });
        f.commands.clear(); f.edit(suffix, suffix == "enabled.set" ? QVariant(true) : suffix == "effect.set" ? QVariant(1) : QVariant(QVariantList(18, QVariant(6))));
        require(injected && noEarlyQuery && !f.commands.isEmpty(), "reentrant GET and old matching report cannot query or acknowledge before setter");
        require(f.operation(suffix == "enabled.set" ? "enabled" : suffix == "effect.set" ? "effect" : "curve").value("pending").toBool(), "old predispatch report cannot erase producer pending ownership");
    }
    {
        Fixture f; f.ready(); f.get(); f.readbacks(); bool once = false; quint64 replacement = 0;
        QObject::connect(f.backend.get(), &IRadioBackend::extensionStatus, f.backend.get(), [&](const QString&, const QString& kind, const QVariantMap&) {
            if (!once && kind == "receiveEq") {
                once = true; QVariantList external = gradient(); external[0] = 30; f.observe(wireCurve(external));
                replacement = f.edit("curve.set", QVariantList(18, QVariant(6)));
            }
        });
        f.commands.clear(); const quint64 old = f.edit("curve.set", QVariantList(18, QVariant(10)));
        require(f.errors.contains(old) && f.results.value(replacement).toBool() && f.operation("curve").value("pending").toBool()
            && !f.commands.contains(wireCurve(QVariantList(18, QVariant(10))) + ';'), "obsolete cleanup cannot clear nested replacement producer");
    }
    {
        Fixture f; f.ready(); f.get(); f.readbacks(); bool deleted = false;
        QObject::connect(f.backend.get(), &IRadioBackend::extensionStatus, f.backend.get(), [&](const QString&, const QString& kind, const QVariantMap&) {
            if (!deleted && kind == "receiveEq") { deleted = true; f.backend.reset(); }
        });
        f.commands.clear(); f.edit("enabled.set", true);
        require(deleted && !f.backend && f.commands.isEmpty(), "pending publication may delete backend without stale setter continuation");
    }
    {
        Fixture f; f.ready(); f.get(); f.readbacks(); bool deleted = false;
        f.onWrite = [&](const QByteArray& bytes) {
            if (!deleted && bytes == "UR;") { deleted = true; f.backend.reset(); }
        };
        f.edit("curve.set", QVariantList(18, QVariant(6)));
        require(deleted && !f.backend, "readback query callback may delete backend without stale completion access");
    }
}
void producerStatusChronology() {
    Fixture f;
    require(KenwoodBackendTestAccess::status(*f.backend).value("statusRevision") == QVariant(QStringLiteral("0")),
            "unpublished EQ producer starts at canonical revision zero");
    f.ready(); f.get(); f.readbacks();
    const auto revision = [](const QVariantMap& state) {
        const QVariant raw = state.value("statusRevision");
        bool valid = false;
        const quint64 number = raw.toString().toULongLong(&valid);
        require(raw.typeId() == QMetaType::QString && valid && QString::number(number) == raw.toString(),
                "EQ status uses a canonical decimal publication revision");
        return number;
    };
    const quint64 before = revision(f.state);
    f.get(); require(revision(f.state) == before, "GET carries the latest EQ publication without inventing a change");
    bool nested = false;
    QList<quint64> delivered;
    const QMetaObject::Connection withdrawal = QObject::connect(f.backend.get(), &IRadioBackend::extensionStatus,
        f.backend.get(), [&](const QString&, const QString& kind, const QVariantMap& state) {
            if (kind == "receiveEq" && !nested && state.value("operations").toMap().value("enabled").toMap().value("pending").toBool()) {
                nested = true; f.observe("FR1");
            }
        });
    const QMetaObject::Connection consumer = QObject::connect(f.backend.get(), &IRadioBackend::extensionStatus,
        f.backend.get(), [&](const QString&, const QString& kind, const QVariantMap& state) {
            if (kind == "receiveEq") { delivered.append(revision(state)); }
        });
    f.commands.clear(); const quint64 request = f.edit("enabled.set", true);
    require(nested && f.errors.contains(request) && !f.commands.contains("EQR01;"),
            "nested source withdrawal rejects EQ setter before dispatch");
    require(delivered.size() >= 2 && delivered[delivered.size() - 2] > delivered.last(),
            "late signal consumer can distinguish newer nested EQ snapshot from older outer delivery");
    const quint64 newest = revision(KenwoodBackendTestAccess::status(*f.backend));
    require(newest == delivered[delivered.size() - 2], "EQ getter retains the producer's newest publication");
    QObject::disconnect(withdrawal); QObject::disconnect(consumer);
    f.backend->disconnectRadio();
    require(revision(KenwoodBackendTestAccess::status(*f.backend)) >= newest, "reset cannot roll back EQ producer chronology");
    f.ready(); f.get();
    require(revision(f.state) > newest, "same backend reconnect keeps EQ publication chronology monotonic");
}
void contextsAndFailures() {
    Fixture f; f.ready(); f.get(); f.readbacks();
    const QVariant oldContext = f.state.value("context"); const QVariant oldCurve = f.state.value("curveRevision");
    f.mode('5'); f.get(); f.readbacks(); f.commands.clear();
    require(f.errors.contains(f.request("receive.eq.curve.set", {{"context", oldContext}, {"curveRevision", oldCurve}, {"curveRaw", gradient()}})) && f.commands.isEmpty(), "mode cycle cannot upload old stored curve into new effect bank");
    f.observe("TS1"); f.commands.clear(); f.edit("enabled.set", true);
    require(f.commands.isEmpty(), "TF-SET withdraws EQ write authority");
    f.observe("TS0"); f.get(); f.readbacks(); KenwoodBackendTestAccess::intent(*f.backend, true); f.commands.clear(); f.edit("enabled.set", true);
    require(f.commands.isEmpty(), "local key intent withdraws EQ before TX reply");
    KenwoodBackendTestAccess::intent(*f.backend, false); f.get(); f.readbacks(); KenwoodBackendTestAccess::tune(*f.backend, true); f.commands.clear(); f.edit("enabled.set", true);
    require(f.commands.isEmpty(), "pending tune preflight blocks EQ without requiring key flag");
    KenwoodBackendTestAccess::tune(*f.backend, false); f.get(); f.readbacks(); f.accept = false;
    const quint64 rejected = f.edit("enabled.set", true);
    require(f.errors.contains(rejected) && !f.operation("enabled").value("pending").toBool(), "dispatch rejection retires exact local ticket without invented state");
    f.accept = true; f.commands.clear(); f.get(QVariantMap{});
    require(f.commands.contains("EQR0;") && f.commands.contains("EQR1;"), "explicit GET recovers after failed dispatch and accepts empty query map");
    f.backend->disconnectRadio(); f.commands.clear(); f.edit("enabled.set", true);
    require(f.commands.isEmpty(), "disconnect never replays prior native EQ intent");
}
}
int main(int argc, char** argv) {
    TestSettingsProfile profile(QStringLiteral("kenwood-receive-eq")); QCoreApplication app(argc, argv);
    require(profile.isValid(), "isolated settings"); modesAndChoices(); curvesAndResets(); copiesAndInspection(); copyHandoffOwnership(); reentryAndLifetime(); producerStatusChronology(); contextsAndFailures(); return 0;
}
