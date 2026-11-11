#include "TestSettingsProfile.h"
#include "core/backends/kenwood/KenwoodBackend.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QPair>
#include <QMap>
#include <QVariantMap>

#include <cstdlib>
#include <functional>
#include <iostream>

namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static qint64 relativeFrequency(const KenwoodBackend& b) { return b.m_frequency[0]; }
    static QVariantMap relativeStatus(const KenwoodBackend& b) { return b.relativeTuningStatus(); }
    static void relativePoll(KenwoodBackend& b, qint64 now) { b.pollRelativeTuning(now); }
    static void relativeIntent(KenwoodBackend& b, bool active) { b.m_keyIntent = active; b.refreshReceiverOptionsContext(); b.refreshRelativeTuningContext(); }
    static QVariantMap chronologyStatus(const KenwoodBackend& b) { return b.tuningControlStatus(); }
    static void chronologyPublish(KenwoodBackend& b) { b.publishTuningControls(); }
    static void chronologyFeed(KenwoodBackend& b, const QByteArray& bytes) { b.adopt(bytes); }
    static void authenticate(KenwoodBackend& backend)
    {
        backend.m_clock.start();
        backend.m_protocol.start("fixture-user", "fixture-password", false, 0);
        backend.m_protocol.receive("##CN1;", 1);
        backend.m_protocol.receive("##ID1;", 2);
        backend.ready();
    }
    static void message(KenwoodBackend& backend, const QByteArray& message) { backend.adopt(message); }
    static void batch(KenwoodBackend& backend, const QList<QByteArray>& messages)
    {
        const quint64 batch = ++backend.m_controlIngressSequence;
        for (const QByteArray& message : messages) { backend.adopt(message, batch); }
    }
    static void intent(KenwoodBackend& backend, bool active)
    {
        backend.m_keyIntent = active;
        backend.adoptTuningControl({}, backend.m_controlIngressSequence);
    }
};
}
namespace {
using namespace AetherSDR;
using namespace AetherSDR::kenwood;
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
struct Fixture {
    QList<QByteArray> commands;
    bool accept = true;
    QVariantMap state;
    std::function<void(const QByteArray&)> onWrite;
    QMap<quint64, QVariant> results;
    QMap<quint64, QString> errors;
    KenwoodBackend backend{KenwoodBackend::Transport{[this](const QByteArray& bytes) {
        commands.append(bytes);
        const std::function<void(const QByteArray&)> callback = onWrite;
        if (callback) { callback(bytes); }
        return accept;
    }, {}}};
    quint64 request = 1;
    Fixture()
    {
        QObject::connect(&backend, &IRadioBackend::extensionStatus, &backend,
            [this](const QString& space, const QString& kind, const QVariantMap& status) {
                if (space == "kenwood" && kind == "tuning") { state = status; }
            });
        QObject::connect(&backend, &IRadioBackend::extensionResult, &backend,
            [this](quint64 id, const QVariant& value) {
                results.insert(id, value);
                if (value.typeId() == QMetaType::QVariantMap && value.toMap().contains("editable")) { state = value.toMap(); }
            });
        QObject::connect(&backend, &IRadioBackend::extensionError, &backend,
            [this](quint64 id, const QString& error) { errors.insert(id, error); });
    }
    void observe(const QByteArray& message) { KenwoodBackendTestAccess::message(backend, message); }
    void ready()
    {
        KenwoodBackendTestAccess::authenticate(backend);
        for (const QByteArray& message : {QByteArray("ID024"), QByteArray("XV0"), QByteArray("FR0"), QByteArray("FT1"),
                QByteArray("FA00014074000"), QByteArray("FB00007074000"), QByteArray("OM03"),
                QByteArray("SF0000140740003"), QByteArray("SF1000070740003"), QByteArray("TS0"), QByteArray("RX")}) {
            observe(message);
        }
    }
    void get() { backend.invokeExtension("kenwood", "tuning.get", request++); }
    quint64 set(const QString& field, const QVariant& value, const QVariant& context = {})
    {
        const quint64 id = request++;
        backend.invokeExtension("kenwood", "tuning.set", id, QVariantMap{{"context", context.isValid() ? context : state.value("context")}, {"field", field}, {"value", value}});
        return id;
    }
    void readbacks() { observe("FS00"); observe("MH0"); observe("LK0"); }
};
void run()
{
    Fixture initialInterest;
    initialInterest.get(); initialInterest.commands.clear(); initialInterest.ready();
    require(initialInterest.commands.count("FS;") == 1 && initialInterest.commands.count("MH;") == 1
        && initialInterest.commands.count("LK;") == 1, "initial interested context dispatches each tuning read only once");
    Fixture f;
    f.ready();
    require(!f.commands.contains("FS;") && !f.commands.contains("MH;") && !f.commands.contains("LK;"), "tuning queries require explicit interest");
    f.commands.clear(); f.get();
    require(f.commands == QList<QByteArray>{"FS;", "MH;", "LK;"}, "one bounded initial query per tuning field");
    f.commands.clear(); f.get();
    require(f.commands == QList<QByteArray>{"FS;", "MH;", "LK;"}, "explicit same-context get performs bounded fresh reads");
    for (const QByteArray& bits : {QByteArray("00"), QByteArray("01"), QByteArray("10"), QByteArray("11")}) {
        f.observe("FS" + bits);
        require(f.state.value("receiveFine").toBool() == (bits[0] == '1'), "FS receive bit adopts independently");
        require(f.state.value("transmitFine").toBool() == (bits[1] == '1'), "FS transmit bit adopts independently");
    }
    const QVariantMap valid = f.state;
    for (const QByteArray& invalid : {QByteArray("FS1"), QByteArray("FS000"), QByteArray("FS20"), QByteArray("FS0X"), QByteArray("MH2"), QByteArray("LK10")}) { f.observe(invalid); }
    require(f.state == valid, "malformed or setter-shaped frames cannot replace observed tuning state");
    f.observe("FS01"); f.observe("MH1"); f.observe("LK1");
    require(!f.state.value("receiveFine").toBool() && f.state.value("transmitFine").toBool()
        && f.state.value("mhzStep").toBool() && f.state.value("locked").toBool(),
        "unsolicited same-context front-panel observations remain adoptable");
    f.readbacks(); f.commands.clear();
    const quint64 enabled = f.set("receiveFine", true);
    require(f.results.value(enabled).toBool() && f.commands == QList<QByteArray>{"FS1;", "FS;"}, "receive FINE dispatch is one setter bit plus query");
    require(!f.state.value("receiveFine").toBool() && f.state.value("pending").toMap().value("receiveFine").toBool(), "pending retains last confirmed RX FINE");
    f.observe("FS10");
    require(f.state.value("receiveFine").toBool() && !f.state.value("transmitFine").toBool() && f.state.value("pending").toMap().isEmpty(), "two-bit readback completes only confirmed FINE request");
    f.commands.clear();
    const quint64 disabled = f.set("receiveFine", false);
    require(f.results.value(disabled).toBool() && f.commands == QList<QByteArray>{"FS0;", "FS;"},
        "receive FINE Off dispatch is one setter bit plus query");
    require(f.state.value("receiveFine").toBool() && f.state.value("pending").toMap().value("receiveFine").toBool(),
        "pending FINE Off retains confirmed On");
    f.observe("FS01");
    require(!f.state.value("receiveFine").toBool() && f.state.value("transmitFine").toBool()
        && f.state.value("pending").toMap().isEmpty(), "RX Off answer preserves independent TX bit without writing it");
    for (const auto& item : {qMakePair(QString("mhzStep"), QByteArray("MH")), qMakePair(QString("locked"), QByteArray("LK"))}) {
        f.commands.clear(); f.set(item.first, true);
        require(f.commands == QList<QByteArray>{item.second + "1;", item.second + ";"}, "MHz/lock uses native bool setter and query");
        require(!f.state.value(item.first).toBool(), "native field is not optimistic");
        f.observe(item.second + '1');
        require(f.state.value(item.first).toBool(), "native field adopts radio readback");
        f.commands.clear(); f.set(item.first, false);
        require(f.commands == QList<QByteArray>{item.second + "0;", item.second + ";"},
            "MHz/lock confirmed On-to-Off uses native zero setter plus query");
        require(f.state.value(item.first).toBool() && f.state.value("pending").toMap().value(item.first).toBool(),
            "pending native Off retains confirmed On");
        f.observe(item.second + '0');
        require(!f.state.value(item.first).toBool() && !f.state.value("pending").toMap().contains(item.first),
            "native Off is adopted only on valid zero readback");
    }
    f.commands.clear();
    for (const QString& field : {QString("transmitFine"), QString("FS"), QString("unknown")}) { require(f.errors.contains(f.set(field, true)), "unsupported fields refused"); }
    for (const QVariant& value : {QVariant("1"), QVariant(1), QVariant(QVariantMap{{"raw", "FS1"}})}) { require(f.errors.contains(f.set("receiveFine", value)), "only actual boolean admitted"); }
    require(f.errors.contains(f.set("receiveFine", true, "stale-context")), "stale context refused");
    f.backend.invokeExtension("other", "tuning.set", f.request++, QVariantMap{{"context", f.state.value("context")}, {"field", "locked"}, {"value", false}});
    f.backend.invokeExtension("kenwood", "tuning.set", f.request++, "FS1;");
    f.backend.invokeExtension("kenwood", "tuning.get", f.request++, true);
    require(f.commands.isEmpty(), "malformed and wrong namespace requests have no wire effects");
    const QVariant oldContext = f.state.value("context");
    KenwoodBackendTestAccess::batch(f.backend, {"FR1", "FS00", "MH0", "LK0"});
    require(!f.state.contains("receiveFine") && !f.state.contains("mhzStep") && !f.state.contains("locked"), "role switch retires cached values and old-batch reports");
    f.commands.clear(); require(f.errors.contains(f.set("locked", false, oldContext)), "old VFO context edit refused");
    require(f.commands.isEmpty(), "stale role request cannot write native controls");
    f.observe("SF1000070740003"); f.readbacks();
    f.observe("TS1"); f.commands.clear();
    require(!f.state.value("editable").toBool() && f.errors.contains(f.set("receiveFine", true)), "TF-SET withdraws tuning admission");
    require(f.commands.isEmpty(), "TF-SET refused before setter");
    f.observe("TS0"); f.readbacks();
    KenwoodBackendTestAccess::intent(f.backend, true);
    f.commands.clear(); require(f.errors.contains(f.set("mhzStep", true)), "local TX intent withdraws tuning admission");
    require(f.commands.isEmpty(), "no tuning during local TX intent");
    KenwoodBackendTestAccess::intent(f.backend, false); f.readbacks();
    bool once = false;
    const QMetaObject::Connection reentrant = QObject::connect(&f.backend, &IRadioBackend::extensionStatus, &f.backend,
        [&](const QString&, const QString& kind, const QVariantMap& state) {
            if (!once && kind == "tuning" && state.value("pending").toMap().contains("receiveFine")) { once = true; f.observe("FR0"); }
        });
    f.commands.clear(); f.set("receiveFine", true);
    require(once && !f.commands.contains("FS1;"), "reentrant VFO switch cancels old producer before mutation");
    QObject::disconnect(reentrant);
    f.backend.disconnectRadio(); f.commands.clear();
    require(f.errors.contains(f.set("locked", true)) && f.commands.isEmpty(), "disconnected edit cannot restore prior tuning authority");
    f.ready(); f.commands.clear();
    require(!f.commands.contains("FS1;"), "reconnect never replays requested native settings");
    f.get(); require(!f.state.contains("receiveFine"), "reconnect begins without old FINE readback");
    Fixture rejected;
    rejected.ready(); rejected.commands.clear(); rejected.accept = false; rejected.get();
    require(rejected.commands == QList<QByteArray>{"FS;"}, "rejected initial query stops bounded query batch");
    rejected.accept = true; rejected.commands.clear(); rejected.get();
    require(rejected.commands == QList<QByteArray>{"FS;", "MH;", "LK;"}, "explicit get retries a rejected initial query");
    Fixture queryChange;
    queryChange.ready(); queryChange.get(); queryChange.readbacks();
    queryChange.onWrite = [&](const QByteArray& bytes) {
        if (bytes == "FS;") {
            queryChange.onWrite = {};
            queryChange.observe("FR1");
        }
    };
    const quint64 interrupted = queryChange.set("receiveFine", true);
    require(queryChange.errors.contains(interrupted) && !queryChange.results.contains(interrupted),
        "reentrant context change during final query gets terminal error, not false success or silence");
}
}

void verifyTuningProducerChronology()
{
    using Backend = AetherSDR::kenwood::KenwoodBackend;
    using Probe = AetherSDR::kenwood::KenwoodBackendTestAccess;
    Backend backend(Backend::Transport{[](const QByteArray&) { return true; }, {}});
    const QVariant initial = Probe::chronologyStatus(backend).value("statusRevision");
    require(initial.typeId() == QMetaType::QString && initial.toString() == "0", "tuning initial getter has canonical QString revision zero");
    Probe::authenticate(backend);
    for (const QByteArray& bytes : {QByteArray("ID024"), QByteArray("XV0"), QByteArray("FR0"), QByteArray("FA00014074000"), QByteArray("SF0000140740003"), QByteArray("TS0"), QByteArray("RX")}) { Probe::chronologyFeed(backend, bytes); }
    bool nested = false; QList<QVariantMap> delivered;
    QObject::connect(&backend, &AetherSDR::IRadioBackend::extensionStatus, &backend,
        [&](const QString&, const QString& statusKind, const QVariantMap&) {
            if (statusKind != "tuning" || nested) { return; }
            nested = true; Probe::chronologyFeed(backend, "TS1");
        });
    QObject::connect(&backend, &AetherSDR::IRadioBackend::extensionStatus, &backend,
        [&](const QString&, const QString& statusKind, const QVariantMap& state) {
            if (statusKind == "tuning") { delivered.append(state); }
        });
    Probe::chronologyPublish(backend);
    require(nested && delivered.size() >= 2, "tuning real TF withdrawal publishes newer status during outer delivery");
    const quint64 newer = delivered.first().value("statusRevision").toString().toULongLong();
    const quint64 older = delivered.last().value("statusRevision").toString().toULongLong();
    require(newer > older, "tuning producer stamps snapshot before nested newer delivery");
    for (const QVariantMap& state : delivered) {
        const QVariant revision = state.value("statusRevision");
        require(revision.typeId() == QMetaType::QString && revision.toString() == QString::number(revision.toString().toULongLong()), "tuning every revision is canonical decimal QString");
    }
    QVariantMap queried;
    QObject::connect(&backend, &AetherSDR::IRadioBackend::extensionResult, &backend,
        [&](quint64 request, const QVariant& result) { if (request == 901) { queried = result.toMap(); } });
    backend.invokeExtension("kenwood", "tuning.get", 901);
    require(queried.value("statusRevision") == Probe::chronologyStatus(backend).value("statusRevision")
        && queried.value("statusRevision").toString().toULongLong() >= newer, "tuning GET carries latest producer revision after stale outer delivery");
    const quint64 beforeReset = queried.value("statusRevision").toString().toULongLong();
    backend.disconnectRadio(); Probe::chronologyPublish(backend);
    require(Probe::chronologyStatus(backend).value("statusRevision").toString().toULongLong() > beforeReset, "tuning disconnect never resets lifetime publication chronology");
}

void finishRelativeRead(Fixture& f, int target = 0)
{
    for (const QByteArray& reply : QList<QByteArray>{"FR" + QByteArray::number(target), "FT" + QByteArray::number(1 - target), "TS0", "XV0", "MV0", "QR00",
        "FA00014074000", "FB00007074000", "SF0000140740003", "SF1000070740003", "FS00", "MH0", "LK0"}) { f.observe(reply); }
}
void prepareRelative(Fixture& f, int target = 0)
{
    f.ready(); f.observe("FR" + QByteArray::number(target));
    f.backend.invokeExtension("kenwood", "tuning.relative.read", 700); finishRelativeRead(f, target);
    require(KenwoodBackendTestAccess::relativeStatus(f.backend).value("editable").toBool(), "owned native relative survey qualifies physical receiver");
}
QVariantMap relativeArgs(Fixture& f, int direction)
{
    const QVariantMap state = KenwoodBackendTestAccess::relativeStatus(f.backend);
    return {{"context", state.value("context")}, {"revision", state.value("revision")}, {"direction", direction}};
}
void nativeRelativeEarlyFrequencyResult()
{
    for (const QByteArray& early : {QByteArray("FA00014074010"), QByteArray("SF0000140740103")}) {
        Fixture f; prepareRelative(f);
        QVariantMap args = relativeArgs(f, 0); args.insert("multiplier", 0);
        f.backend.invokeExtension("kenwood", "tuning.relative.fc", 800, args);
        f.observe(early);
        require(KenwoodBackendTestAccess::relativeStatus(f.backend).value("read").toMap().value("busy").toBool(),
            "early AI2 target frequency does not cancel the owned post-step survey");
        for (const QByteArray& reply : QList<QByteArray>{"FR0", "FT1", "TS0", "XV0", "MV0", "QR00",
                "FA00014074010", "FB00007074000", "SF0000140740103", "SF1000070740003", "FS00", "MH0", "LK0"}) { f.observe(reply); }
        const QVariantMap state = KenwoodBackendTestAccess::relativeStatus(f.backend);
        require(state.value("lastAction").toMap().value("state") == "observed"
                && state.value("frequencyHz").toLongLong() == 14074010 && state.value("editable").toBool(),
            "only completed coherent survey qualifies an early actual native step result");
    }
}

void nativeRelativeChangedSafetyStillRetires()
{
    for (const QByteArray& changed : {QByteArray("FB00007074010"), QByteArray("SF0000140740102"),
            QByteArray("FAinvalid"), QByteArray("FR1"), QByteArray("FT0"), QByteArray("FS10"), QByteArray("MH1"), QByteArray("LK1")}) {
        Fixture f; prepareRelative(f);
        QVariantMap args = relativeArgs(f, 0); args.insert("multiplier", 0);
        f.backend.invokeExtension("kenwood", "tuning.relative.fc", 801, args);
        f.observe(changed);
        const QVariantMap state = KenwoodBackendTestAccess::relativeStatus(f.backend);
        require(state.value("lastAction").toMap().value("state") == "indeterminate"
                && !state.value("read").toMap().value("busy").toBool(),
            "malformed, other-VFO, mode and matching safety changes still retire native action");
    }
}

void nativeRelativeLateIncoherentFrequency()
{
    Fixture f; prepareRelative(f);
    QVariantMap args = relativeArgs(f, 0); args.insert("multiplier", 0);
    f.backend.invokeExtension("kenwood", "tuning.relative.fc", 802, args);
    for (const QByteArray& reply : QList<QByteArray>{"FR0", "FT1", "TS0", "XV0", "MV0", "QR00",
            "FA00014074010", "FB00007074000", "SF0000140740103", "SF1000070740003"}) { f.observe(reply); }
    f.observe("FA00014074020");
    for (const QByteArray& reply : {QByteArray("FS00"), QByteArray("MH0"), QByteArray("LK0")}) { f.observe(reply); }
    const QVariantMap state = KenwoodBackendTestAccess::relativeStatus(f.backend);
    require(state.value("lastAction").toMap().value("state") == "indeterminate" && !state.value("editable").toBool(),
        "late inconsistent FA/SF observations cannot qualify a finished native survey");
}

void allNativeRelativeEncodings()
{
    for (int target = 0; target < 2; ++target) {
        for (int direction = 0; direction < 2; ++direction) {
            Fixture f; prepareRelative(f, target);
            for (int multiplier = 0; multiplier < 6; ++multiplier) {
                QVariantMap args = relativeArgs(f, direction); args.insert("multiplier", multiplier); f.commands.clear();
                f.backend.invokeExtension("kenwood", "tuning.relative.fc", 701, args);
                require(f.commands.first() == "FC" + QByteArray::number(direction) + QByteArray::number(multiplier) + ';', "FC encodes native index, never numeric multiplier or Hz arithmetic");
                require(!f.results.value(701).toMap().value("verified").toBool(), "native FC handoff never claims causal/effect verification");
                finishRelativeRead(f, target);
                require(KenwoodBackendTestAccess::relativeStatus(f.backend).value("editable").toBool(), "unchanged actual frequency is a valid observation, no guessed step required");
            }
            for (int count = 0; count < 100; ++count) {
                QVariantMap args = relativeArgs(f, direction); args.insert("target", target); args.insert("steps", count); f.commands.clear();
                f.backend.invokeExtension("kenwood", "tuning.relative.ud", 702, args);
                require(f.commands.first() == "UD" + QByteArray::number(target) + QByteArray::number(direction) + QByteArray::number(count).rightJustified(2, '0') + ';', "all UD counts preserve exact two-digit native count including00");
                finishRelativeRead(f, target);
            }
            QVariantMap args = relativeArgs(f, direction); args.insert("target", target); f.commands.clear();
            f.backend.invokeExtension("kenwood", "tuning.relative.ud", 703, args);
            require(f.commands.first() == "UD" + QByteArray::number(target) + QByteArray::number(direction) + ';', "UD omitted count remains native single-step form");
        }
    }
}
void nativeRelativeCoalescedFrequency()
{
    Fixture f; prepareRelative(f); bool nested = false;
    QObject::connect(&f.backend, &IRadioBackend::sliceChanged, &f.backend, [&](int, const SliceDelta& delta) {
        if (nested || !delta.frequency || *delta.frequency != 14.075) { return; }
        nested = true;
        f.backend.invokeExtension("kenwood", "tuning.relative.read", 720); finishRelativeRead(f);
        QVariantMap args = relativeArgs(f, 0); args.insert("multiplier", 0);
        f.onWrite = [&](const QByteArray& command) { if (command == "FC00;") { f.observe("FA00014076000"); } };
        f.backend.invokeExtension("kenwood", "tuning.relative.fc", 721, args);
    });
    KenwoodBackendTestAccess::batch(f.backend, {"FA00014075000", "FA00014075020"});
    require(nested && f.commands.contains("FC00;"), "coalesced native action chronology witness executes real action and nested readback");
    require(KenwoodBackendTestAccess::relativeFrequency(f.backend) == 14076000, "old coalesced frequency cannot overwrite newer owned native action readback");
}

void nativeRelativeTransverter()
{
    Fixture f; prepareRelative(f);
    QVariantMap fc = relativeArgs(f, 0); fc.insert("multiplier", 5);
    QVariantMap ud = relativeArgs(f, 1); ud.insert("target", 0); ud.insert("steps", 99);
    f.observe("XV1");
    require(!f.backend.isConnected()
        && !KenwoodBackendTestAccess::relativeStatus(f.backend).value("editable").toBool(),
        "transverter On retires the unsupported session and native step authority");
    f.commands.clear();
    f.backend.invokeExtension("kenwood", "tuning.relative.fc", 730, fc);
    f.backend.invokeExtension("kenwood", "tuning.relative.ud", 731, ud);
    require(f.errors.contains(730) && f.errors.contains(731) && f.commands.isEmpty(),
        "retired XV session refuses both native step verbs without writing radio settings");
    f.observe("XV0");
    f.backend.invokeExtension("kenwood", "tuning.relative.fc", 732, fc);
    require(!f.backend.isConnected() && f.errors.contains(732) && f.commands.isEmpty(),
        "late XV Off cannot revive a retired session or its native step draft");
    prepareRelative(f);
    f.commands.clear();
    f.backend.invokeExtension("kenwood", "tuning.relative.fc", 733, fc);
    require(f.errors.contains(733) && f.commands.isEmpty(),
        "reconnected XV Off session cannot reuse the previous session's native step draft");
}

void nativeRelativeGuards()
{
    {
        Fixture f; f.ready(); f.commands.clear(); f.backend.invokeExtension("kenwood", "tuning.relative.get", 710);
        require(f.commands.isEmpty(), "relative cached getter performs no IO");
    }
    for (const QVariant& invalid : {QVariant(true), QVariant(1.0), QVariant("1"), QVariant(-1), QVariant(6)}) {
        Fixture f; prepareRelative(f); QVariantMap args = relativeArgs(f, 0); args.insert("multiplier", invalid); f.commands.clear();
        f.backend.invokeExtension("kenwood", "tuning.relative.fc", 711, args);
        require(f.errors.contains(711) && f.commands.isEmpty(), "FC refuses coercion and invalid native index without IO");
    }
    for (const QByteArray& context : {QByteArray("TS1"), QByteArray("XV1"), QByteArray("FR3"), QByteArray("MV1"), QByteArray("QR10"), QByteArray("TX0"), QByteArray("FS10"), QByteArray("LK1")}) {
        Fixture f; prepareRelative(f); QVariantMap args = relativeArgs(f, 0); args.insert("multiplier", 0); f.observe(context); f.commands.clear();
        f.backend.invokeExtension("kenwood", "tuning.relative.fc", 712, args);
        require(f.errors.contains(712) && f.commands.isEmpty(), "changed native context retires previous step draft");
    }
    for (bool safety : {false, true}) {
        Fixture f; prepareRelative(f); QVariantMap args = relativeArgs(f, 0); args.insert("multiplier", 0); bool nested = false;
        QObject::connect(&f.backend, &IRadioBackend::extensionStatus, &f.backend, [&](const QString&, const QString& kind, const QVariantMap& state) {
            if (nested || kind != "relativeTuning" || state.value("lastAction").toMap().value("state") != "pending") { return; }
            nested = true;
            if (safety) { KenwoodBackendTestAccess::relativeIntent(f.backend, true); KenwoodBackendTestAccess::relativeIntent(f.backend, false); }
            else { f.observe("FA00014075000"); f.observe("FA00014074000"); }
        });
        f.commands.clear(); f.backend.invokeExtension("kenwood", "tuning.relative.fc", 713, args);
        require(nested && !f.commands.contains("FC00;") && f.errors.contains(713), "frequency and local-intent ABA prevent pending native action");
    }
    for (const QVariant& invalid : {QVariant(true), QVariant(1.0), QVariant("1"), QVariant(-1), QVariant(100)}) {
        Fixture f; prepareRelative(f); QVariantMap args = relativeArgs(f, 0); args.insert("target", 0); args.insert("steps", invalid); f.commands.clear();
        f.backend.invokeExtension("kenwood", "tuning.relative.ud", 719, args);
        require(f.errors.contains(719) && f.commands.isEmpty(), "UD rejects count coercion and out-of-domain count");
    }
    {
        Fixture f; prepareRelative(f); QVariantMap args = relativeArgs(f, 1); args.insert("target", 1); args.insert("steps", 1); f.commands.clear();
        f.backend.invokeExtension("kenwood", "tuning.relative.ud", 714, args);
        require(f.errors.contains(714) && f.commands.isEmpty(), "inactive physical VFO cannot receive RX-only UD write");
    }
    {
        Fixture f; prepareRelative(f); QVariantMap args = relativeArgs(f, 0); args.insert("multiplier", 0); f.accept = false; f.commands.clear();
        f.backend.invokeExtension("kenwood", "tuning.relative.fc", 715, args);
        require(f.commands == QList<QByteArray>{"FC00;"} && !f.results.value(715).toMap().value("sent").toBool(), "native transport refusal never triggers query, retry or inverse");
    }
    {
        Fixture f; f.ready(); f.backend.invokeExtension("kenwood", "tuning.relative.read", 716); KenwoodBackendTestAccess::relativePoll(f.backend, 1000000);
        require(!KenwoodBackendTestAccess::relativeStatus(f.backend).value("read").toMap().value("busy").toBool(), "relative survey timeout is bounded");
    }
}

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("kenwood-tuning-controls"));
    require(settings.isValid(), "isolated settings profile");
    QCoreApplication app(argc, argv);
    nativeRelativeEarlyFrequencyResult(); nativeRelativeChangedSafetyStillRetires(); nativeRelativeLateIncoherentFrequency(); allNativeRelativeEncodings(); nativeRelativeGuards(); nativeRelativeCoalescedFrequency(); nativeRelativeTransverter();
    verifyTuningProducerChronology();
    run();
    return 0;
}
