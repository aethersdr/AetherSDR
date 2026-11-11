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
    static QVariantMap status(const KenwoodBackend& b) { return b.receiveAgcStatus(); }
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
    static void intent(KenwoodBackend& b, bool active) { b.m_keyIntent = active; b.adoptReceiveAgc({}, ++b.m_controlIngressSequence); }
    static void tune(KenwoodBackend& b, bool active) {
        if (active) { b.m_tune.emplace(); } else { b.m_tune.reset(); }
        ++b.m_tuneSerial; b.adoptReceiveAgc({}, ++b.m_controlIngressSequence);
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
            [this](const QString&, const QString& kind, const QVariantMap& value) { if (kind == "receiveAgc") { state = value; } });
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
    quint64 get(const QVariant& argument = {}) { const quint64 id = sequence++; backend->invokeExtension("kenwood", "receive.agc.get", id, argument); return id; }
    void load() { observe("GT141209"); observe("AQ01"); observe("AQ105"); observe("GC0"); }
    QVariantMap operation(const QString& id) const { return state.value("operations").toMap().value(id).toMap(); }
    QVariantMap arguments(const QString& bank = {}) const {
        QVariantMap fields{{"context", state.value("context")}};
        if (!bank.isEmpty()) { fields.insert("bank", bank); fields.insert("revision", state.value("presets").toMap().value("revision")); }
        return fields;
    }
    quint64 request(const QString& verb, const QVariantMap& fields) {
        const quint64 id = sequence++; backend->invokeExtension("kenwood", "receive.agc." + verb, id, fields); return id;
    }
};
void modesAndValues() {
    for (char code : QByteArray("12345679ABCDEF")) {
        Fixture f; f.ready(); f.mode(code); f.get(); f.load();
        const bool supported = code != '4' && code != 'E';
        require(f.operation("slow").value("editable").toBool() == supported, "all fourteen modes have exact AGC admission");
        if (!supported) { continue; }
        for (const QString& bank : {QString("slow"), QString("mid"), QString("fast")}) {
            for (int value = 1; value <= 20; ++value) {
                f.load(); f.commands.clear(); QVariantMap fields = f.arguments(bank); fields.insert("value", value);
                f.request("preset.set", fields);
                QByteArray expected("GT141209"); expected.replace(2 + (bank == "slow" ? 0 : bank == "mid" ? 2 : 4), 2, QByteArray::number(value).rightJustified(2, '0'));
                require(f.commands == QList<QByteArray>{expected + ';', "GT;"}, "preset writes exact full vector and query");
                require(f.state.value("presets").toMap().value("slow").toInt() == 14, "preset remains last confirmed before readback");
                f.observe(expected); require(!f.operation(bank).value("pending").toBool(), "fresh vector acknowledges pending preset");
            }
            f.load(); f.commands.clear(); f.request("preset.reset", f.arguments(bank));
            QByteArray expected("GT141209"); expected.replace(2 + (bank == "slow" ? 0 : bank == "mid" ? 2 : 4), 2, "99");
            require(f.commands == QList<QByteArray>{expected + ';', "GT;"}, "reset99 preserves both fresh companion indices");
        }
    }
}
void independentFieldsAndValidation() {
    Fixture f; f.ready(); f.get(); f.load();
    for (int value = 1; value <= 2; ++value) {
        f.commands.clear(); auto fields = f.arguments(); fields.insert("value", value); f.request("quick.enabled.set", fields);
        require(f.commands == QList<QByteArray>{"AQ0" + QByteArray::number(value) + ';', "AQ0;"}, "AQ0 uses wire enum1/2"); f.observe("AQ0" + QByteArray::number(value));
    }
    for (int value = 1; value <= 10; ++value) {
        f.commands.clear(); auto fields = f.arguments(); fields.insert("value", value); f.request("quick.threshold.set", fields);
        const QByteArray raw = QByteArray::number(value).rightJustified(2, '0');
        require(f.commands == QList<QByteArray>{"AQ1" + raw + ';', "AQ1;"}, "AQ1 raw threshold complete range"); f.observe("AQ1" + raw);
    }
    f.commands.clear(); f.request("quick.threshold.reset", f.arguments());
    require(f.commands == QList<QByteArray>{"AQ199;", "AQ1;"}, "AQ1 reset99 has readback"); f.observe("AQ105");
    const QVariantMap prior = f.state.value("presets").toMap();
    for (const QByteArray& bad : {QByteArray("GT001209"), QByteArray("GT211209"), QByteArray("GT1412x9"), QByteArray("GT1412090"), QByteArray("AQ00"), QByteArray("AQ111")}) { f.observe(bad); }
    require(f.state.value("presets").toMap() == prior && f.state.value("quickEnabled").toInt() == 2 && f.state.value("quickThreshold").toInt() == 5, "malformed readbacks cannot alter authority");
    for (const QVariant& bad : {QVariant(0), QVariant(21), QVariant(true), QVariant("2"), QVariant(2.0)}) {
        f.commands.clear(); auto fields = f.arguments("slow"); fields.insert("value", bad); const auto id = f.request("preset.set", fields);
        require(f.errors.contains(id) && f.commands.isEmpty(), "typed/ranged malformed setter rejected before wire");
    }
    auto stale = f.arguments("slow"); stale.insert("value", 3); f.observe("GT151209"); f.commands.clear();
    require(f.errors.contains(f.request("preset.set", stale)) && f.commands.isEmpty(), "stale vector revision never replays companions");
}
void pendingAndReentry() {
    Fixture f; f.ready(); f.get(); f.load(); bool nested = false;
    QObject::connect(f.backend.get(), &IRadioBackend::extensionStatus, f.backend.get(), [&](const QString&, const QString& kind, const QVariantMap&) {
        if (kind != "receiveAgc" || nested || !f.operation("slow").value("pending").toBool()) { return; }
        nested = true; f.get(); f.observe("GT141209");
    });
    f.commands.clear(); auto fields = f.arguments("slow"); fields.insert("value", 10); f.request("preset.set", fields);
    require(f.commands == QList<QByteArray>{"GT101209;", "GT;"}, "reentrant GET and old report cannot query before setter");
    KenwoodBackendTestAccess::sameBatch(*f.backend, "GT101209");
    require(f.operation("slow").value("pending").toBool() && f.state.value("presets").toMap().value("slow").toInt() == 14, "old publication batch cannot settle dispatched producer");
    f.observe("GT101209"); require(!f.operation("slow").value("pending").toBool(), "later readback settles vector");
    f.load(); f.commands.clear(); fields = f.arguments("slow"); fields.insert("value", 2); f.request("preset.set", fields);
    auto quick = f.arguments(); quick.insert("value", 2); f.request("quick.enabled.set", quick);
    require(f.commands.contains("AQ02;"), "AQ field remains independent of pending GT vector");
    f.observe("AQ02"); require(f.operation("slow").value("pending").toBool(), "AQ acknowledgement cannot clear GT producer");
    f.observe("GT021209");
    auto old = f.arguments("slow"); old.insert("value", 4); f.observe("FR1"); f.commands.clear();
    require(f.errors.contains(f.request("preset.set", old)) && f.commands.isEmpty(), "physical source change retires stale AGC request");
}
void sharedGcOrdering() {
    RadioModel model; QList<QByteArray> commands; QVariantMap status;
    auto owned = std::make_unique<KenwoodBackend>(KenwoodBackend::Transport{[&](const QByteArray& bytes) { commands.append(bytes); return true; }, {}});
    KenwoodBackend* backend = owned.get(); model.setBackendForTest(std::move(owned), QStringLiteral("kenwood"));
    QObject::connect(backend, &IRadioBackend::extensionStatus, &model, [&](const QString&, const QString& kind, const QVariantMap& state) { if (kind == "receiveAgc") { status = state; } });
    KenwoodBackendTestAccess::authenticate(*backend);
    for (const QByteArray& value : {QByteArray("ID024"), QByteArray("FR0"), QByteArray("FA00014074000"), QByteArray("OM03"), QByteArray("SF0000140740003"), QByteArray("TS0"), QByteArray("RX")}) { KenwoodBackendTestAccess::message(*backend, value); }
    backend->invokeExtension("kenwood", "receive.agc.get", 100, QVariantMap{});
    KenwoodBackendTestAccess::message(*backend, "GC0");
    require(model.slice(0), "real normalized backend creates receive slice");
    int callbacks = 0;
    QObject::connect(model.slice(0), &SliceModel::agcModeChanged, &model, [&](const QString&) {
        ++callbacks;
        backend->invokeExtension("kenwood", "receive.agc.restore", 200 + callbacks,
            QVariantMap{{"context", status.value("context")}, {"restoreRevision", status.value("restoreRevision")}});
    });
    for (const QByteArray& bank : {QByteArray("GC1"), QByteArray("GC2"), QByteArray("GC3")}) {
        commands.clear(); KenwoodBackendTestAccess::message(*backend, bank);
        require(!commands.contains("GC4;"), "native GC authority retires Off before shared model callback can restore");
    }
    commands.clear(); KenwoodBackendTestAccess::message(*backend, "GC0");
    require(callbacks == 4 && commands == QList<QByteArray>{"GC4;", "GC;"}, "shared Off callback sees freshly adopted restore authority");
}
void gcPredispatchAndIntents() {
    Fixture f; f.ready(); f.get(); f.load(); bool nested = false;
    QObject::connect(f.backend.get(), &IRadioBackend::extensionStatus, f.backend.get(), [&](const QString&, const QString& kind, const QVariantMap&) {
        if (kind != "receiveAgc" || nested || !f.operation("restore").value("pending").toBool()) { return; }
        nested = true; f.get(); f.observe("GC0");
    });
    f.commands.clear(); auto fields = f.arguments(); fields.insert("restoreRevision", f.state.value("restoreRevision")); f.request("restore", fields);
    require(f.commands == QList<QByteArray>{"GC4;", "GC;"}, "GC old predispatch report preserves producer and GET barrier");
    const QVariant authority = f.state.value("restoreRevision");
    KenwoodBackendTestAccess::sameBatch(*f.backend, "GC1");
    require(f.state.value("selected").toInt() == 0 && f.state.value("restoreRevision") == authority, "same ingress GC cannot mutate native restore authority");
    require(f.operation("restore").value("pending").toBool(), "same ingress GC report cannot acknowledge restore ticket");
    f.observe("GC1"); require(!f.operation("restore").value("pending").toBool(), "later GC report acknowledges restore");
    for (int kind = 0; kind < 4; ++kind) {
        Fixture blocked; blocked.ready(); blocked.get(); blocked.load();
        if (kind == 0) { KenwoodBackendTestAccess::intent(*blocked.backend, true); }
        if (kind == 1) { KenwoodBackendTestAccess::tune(*blocked.backend, true); }
        if (kind == 2) { blocked.observe("TS1"); }
        if (kind == 3) { blocked.backend->disconnectRadio(); }
        blocked.commands.clear(); auto edit = blocked.arguments("slow"); edit.insert("value", 2);
        require(blocked.errors.contains(blocked.request("preset.set", edit)) && blocked.commands.isEmpty(), "local key/tune TF and disconnect block AGC mutation");
    }
}

void producerStatusChronology() {
    Fixture f;
    const QVariant initial = KenwoodBackendTestAccess::status(*f.backend).value("statusRevision");
    require(initial.typeId() == QMetaType::QString && initial.toString() == "0", "initial AGC status revision is canonical decimal QString zero");
    f.ready(); f.get(); f.load();
    const QString observed = f.state.value("statusRevision").toString();
    require(!observed.isEmpty() && observed == QString::number(observed.toULongLong()), "AGC publication carries canonical decimal producer revision");
    const quint64 query = f.get();
    require(f.results.value(query).toMap().value("statusRevision").toString() == observed, "GET carries latest revision without inventing publication");
    bool nested = false; QList<QVariantMap> delivered;
    QObject::connect(f.backend.get(), &IRadioBackend::extensionStatus, f.backend.get(), [&](const QString&, const QString& kind, const QVariantMap& state) {
        if (kind != "receiveAgc" || nested || !state.value("operations").toMap().value("slow").toMap().value("pending").toBool()) { return; }
        nested = true; f.observe("FR1");
    });
    QObject::connect(f.backend.get(), &IRadioBackend::extensionStatus, f.backend.get(), [&](const QString&, const QString& kind, const QVariantMap& state) {
        if (kind == "receiveAgc") { delivered.append(state); }
    });
    auto fields = f.arguments("slow"); fields.insert("value", 2); f.request("preset.set", fields);
    require(nested && delivered.size() == 2, "pending AGC publication permits real nested source retirement publication");
    const quint64 newer = delivered[0].value("statusRevision").toString().toULongLong();
    const quint64 older = delivered[1].value("statusRevision").toString().toULongLong();
    require(newer > older && older > observed.toULongLong(), "producer stamps outer snapshot before callbacks so nested newer push precedes distinguishable older delivery");
    require(!delivered[0].value("presets").toMap().contains("slow")
        && delivered[1].value("presets").toMap().contains("slow"), "newer source-retirement snapshot is unknown while stale outer pending snapshot retains old values");
    const QVariantMap latest = KenwoodBackendTestAccess::status(*f.backend);
    require(latest.value("statusRevision").toString().toULongLong() == newer, "getter keeps newest producer revision after stale outer delivery");
    f.backend->disconnectRadio();
    require(KenwoodBackendTestAccess::status(*f.backend).value("statusRevision").toString().toULongLong() >= newer, "disconnect never resets lifetime producer counter");
    const quint64 afterDisconnect = f.get();
    require(f.results.value(afterDisconnect).toMap().value("statusRevision").toString().toULongLong() > newer, "post-reset publication advances producer chronology");
}

void restoreAndLifetime() {
    Fixture f; f.ready(); f.get(); f.load(); f.commands.clear();
    auto fields = f.arguments(); fields.insert("restoreRevision", f.state.value("restoreRevision")); f.request("restore", fields);
    require(f.commands == QList<QByteArray>{"GC4;", "GC;"} && f.state.value("selected").toInt() == 0, "restore is write/query with no invented selection");
    f.observe("GC4"); require(f.operation("restore").value("pending").toBool(), "GC4 is not accepted as state");
    f.observe("GC2"); require(!f.operation("restore").value("editable").toBool() && f.state.value("selected").toInt() == 2, "GC actual readback owns restored bank");
    f.commands.clear(); require(f.errors.contains(f.request("restore", fields)) && f.commands.isEmpty(), "stale/non-Off restore cannot dispatch");
    f.observe("TX0"); f.commands.clear(); auto preset = f.arguments("slow"); preset.insert("value", 2);
    require(f.errors.contains(f.request("preset.set", preset)) && f.commands.isEmpty(), "observed TX blocks native receive AGC");
    Fixture deleted; deleted.ready(); deleted.get(); deleted.load(); QPointer<KenwoodBackend> guard(deleted.backend.get());
    deleted.onWrite = [&](const QByteArray& command) { if (command.startsWith("GT02")) { deleted.backend.reset(); } };
    auto edit = deleted.arguments("slow"); edit.insert("value", 2); deleted.request("preset.set", edit);
    require(!guard && deleted.commands.last().startsWith("GT02"), "setter callback deletion prevents continuation");
}
}
int main(int argc, char** argv) {
    TestSettingsProfile profile(QStringLiteral("kenwood-receive-agc")); QCoreApplication app(argc, argv);
    require(profile.isValid(), "isolated settings"); modesAndValues(); independentFieldsAndValidation(); pendingAndReentry(); sharedGcOrdering(); gcPredispatchAndIntents(); producerStatusChronology(); restoreAndLifetime(); return 0;
}
