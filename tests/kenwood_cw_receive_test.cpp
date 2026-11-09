// Independent literal receive-only CD fixtures: PC reference printed 11-12.
#include "TestSettingsProfile.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include <QByteArray>
#include <QCoreApplication>
#include <QList>
#include <QMap>
#include <QVariantMap>
#include <QtTest>
#include <functional>
#include <memory>
#include <utility>

namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& backend) {
        backend.m_clock.start();
        backend.m_protocol.start("cw-receive-fixture", "fixture-only", false, 0);
        backend.m_protocol.receive("##CN1;", 1);
        backend.m_protocol.receive("##ID1;", 2);
        backend.ready();
    }
    static void observe(KenwoodBackend& backend, const QByteArray& body) { backend.adopt(body); }
    static void reset(KenwoodBackend& backend) { backend.resetCwReceive(); }
    static void copied(KenwoodBackend& backend, const QByteArray& body) { backend.adoptCwReceive(body, backend.m_controlIngressSequence); }
    static void poll(KenwoodBackend& backend, qint64 now) { backend.pollCwReceive(now); }
    static void batch(KenwoodBackend& backend, const QList<QByteArray>& bodies) {
        const quint64 batch = ++backend.m_controlIngressSequence;
        for (const QByteArray& body : bodies) { backend.adopt(body, batch); }
    }
};
}
namespace {
using namespace AetherSDR;
using Backend = kenwood::KenwoodBackend;
struct Fixture {
    QList<QByteArray> wires;
    QVariantMap state;
    QList<QVariantMap> textPushes;
    QMap<quint64, QVariant> results;
    QMap<quint64, QString> errors;
    std::function<void(const QVariantMap&)> onStatus;
    std::function<void(const QVariantMap&)> onText;
    std::function<void(const QByteArray&)> onWire;
    std::unique_ptr<Backend> backend;
    int ai = 2, screen = 1, threshold = 15, filter = 1, enabled = 1, basic = 0;
    bool answerReads = false, accept = true, retainTextPushes = true;
    qint64 now = 0;
    quint64 nextRequest = 1;
    Fixture() {
        backend = std::make_unique<Backend>(Backend::Transport{[this](const QByteArray& wire) {
            wires.append(wire); const bool accepted = accept; const auto callback = onWire;
            if (callback) { callback(wire); }
            if (backend && accepted && answerReads) {
                const QByteArray response = reply(wire);
                if (!response.isEmpty()) { observe(response); }
            }
            return accepted;
        }, {}});
        QObject::connect(backend.get(), &IRadioBackend::extensionStatus, backend.get(),
            [this](const QString&, const QString& kind, const QVariantMap& value) {
                if (kind == "cwReceive") { state = value; const auto callback = onStatus; if (callback) { callback(value); } }
                if (kind == "cwReceiveText") { if (retainTextPushes) { textPushes.append(value); } const auto callback = onText; if (callback) { callback(value); } }
            });
        QObject::connect(backend.get(), &IRadioBackend::extensionResult, backend.get(),
            [this](quint64 id, const QVariant& value) { results[id] = value; if (value.toMap().contains("controls")) { state = value.toMap(); } });
        QObject::connect(backend.get(), &IRadioBackend::extensionError, backend.get(),
            [this](quint64 id, const QString& reason) { errors[id] = reason; });
    }
    QByteArray reply(const QByteArray& wire) const {
        if (wire == "AI;") { return "AI" + QByteArray::number(ai); }
        if (wire == "CA;") { return "CA0"; }
        if (wire == "DS0;") { return "DS0" + QByteArray::number(basic); }
        if (wire == "CD0;") { return "CD0" + QByteArray::number(screen); }
        if (wire == "CD1;") { return "CD1" + QByteArray::number(threshold).rightJustified(3, '0'); }
        if (wire == "CD3;") { return "CD3" + QByteArray::number(filter); }
        if (wire == "CD5;") { return "CD5" + QByteArray::number(enabled); }
        return {};
    }
    void ready(bool cw = true) {
        kenwood::KenwoodBackendTestAccess::authenticate(*backend);
        observe("ID024"); observe("RX");
        if (cw) { observe("FR0"); observe("FA00014074000"); observe("SF0000140740003"); }
        get(); wires.clear();
    }
    void observe(const QByteArray& body) { kenwood::KenwoodBackendTestAccess::observe(*backend, body); }
    quint64 call(const QString& verb, const QVariant& args = {}) {
        const quint64 id = nextRequest++; backend->invokeExtension("kenwood", verb, id, args); return id;
    }
    void get() { call("cw.receive.get"); }
    QVariantMap arguments() const { return {{"context", state.value("context")}}; }
    QVariantMap control(const QString& id) const {
        for (const QVariant& row : state.value("controls").toList()) { if (row.toMap().value("id") == id) { return row.toMap(); } }
        return {};
    }
    QVariantMap setArguments(const QString& id, const QVariant& value) const {
        QVariantMap args = arguments(); args["id"] = id; args["revision"] = control(id).value("revision"); args["value"] = value; return args;
    }
    void tick(int count = 1, qint64 advance = 100) {
        for (int index = 0; index < count && backend; ++index) { now += advance; kenwood::KenwoodBackendTestAccess::poll(*backend, now); }
    }
    void read() { answerReads = true; call("cw.receive.read", arguments()); tick(8); }
    void cancel() { QVariantMap args = arguments(); args["token"] = state.value("refresh").toMap().value("token"); call("cw.receive.cancel", args); }
    void clear() { QVariantMap args = arguments(); args["transcriptRevision"] = state.value("transcript").toMap().value("revision"); call("cw.receive.clear", args); }
    QVariantList events() const { return state.value("transcript").toMap().value("events").toList(); }
};
}
class KenwoodCwReceiveTest final : public QObject {
    Q_OBJECT
private slots:
    void autoZeroActivityIsReadOnlyStrictAndSessionFenced() {
        Fixture f; f.ready(false); f.get();
        QVariantMap activity=f.state.value("autoZero").toMap();
        QCOMPARE(activity.value("known"),QVariant(false)); QVERIFY(!activity.contains("active"));
        for (const QByteArray& message : {QByteArray("CA0"),QByteArray("CA1")}) {
            f.observe(message); f.get(); activity=f.state.value("autoZero").toMap();
            QCOMPARE(activity.value("known"),QVariant(true)); QCOMPARE(activity.value("active"),QVariant(message=="CA1"));
        }
        for (const QByteArray& message : {QByteArray("CA"),QByteArray("CA2"),QByteArray("CA01"),QByteArray("CAx")}) {
            f.observe(message); f.get(); activity=f.state.value("autoZero").toMap();
            QCOMPARE(activity.value("known"),QVariant(false)); QCOMPARE(activity.value("active"),QVariant(true));
        }
        const quint64 request=f.call("cw.receive.set",QVariantMap{{"context",f.state.value("context")},{"id","autoZero"},{"revision","0"},{"value",1}});
        QVERIFY(f.errors.contains(request)); QVERIFY(f.wires.isEmpty());
        kenwood::KenwoodBackendTestAccess::reset(*f.backend);
        kenwood::KenwoodBackendTestAccess::copied(*f.backend,"CA1"); f.get();
        activity=f.state.value("autoZero").toMap(); QCOMPARE(activity.value("known"),QVariant(false)); QVERIFY(!activity.contains("active"));
        f.observe("CA0"); f.get(); QCOMPARE(f.state.value("autoZero").toMap().value("active"),QVariant(false));
        QVERIFY(f.wires.isEmpty());
    }
    void autoZeroReadTimeoutAndPredispatchReportDoNotInventActivity() {
        Fixture f; f.ready(); f.observe("CA1"); f.read(); f.answerReads=false;
        bool injected=false;
        f.onStatus=[&](const QVariantMap& status) {
            if (!injected && status.value("refresh").toMap().value("pendingKey")==QVariant("CA")) {
                injected=true; kenwood::KenwoodBackendTestAccess::copied(*f.backend,"CA1");
            }
        };
        f.call("cw.receive.read",f.arguments());
        f.answerReads=true; f.tick(6); f.answerReads=false; f.tick();
        QVERIFY(injected); QCOMPARE(f.state.value("refresh").toMap().value("pendingKey"),QVariant("CA"));
        f.tick(1,2100); f.get();
        QCOMPARE(f.state.value("autoZero").toMap().value("known"),QVariant(false));
        QCOMPARE(f.state.value("autoZero").toMap().value("active"),QVariant(false));
        for (const QByteArray& wire : f.wires) { QVERIFY(wire!="CA0;" && wire!="CA1;"); }
    }
    void newerMalformedAutoZeroReportFencesOlderIngressRemainder() {
        Fixture f; f.ready(); f.observe("CA0");
        bool injected = false;
        f.onStatus = [&](const QVariantMap& status) {
            const QVariantMap activity = status.value("autoZero").toMap();
            if (!injected && activity.value("known").toBool() && activity.value("active").toBool()) {
                injected = true;
                f.observe("CA2");
            }
        };
        kenwood::KenwoodBackendTestAccess::batch(*f.backend, {"CA1", "CA0"}); f.get();
        QVERIFY(injected);
        QCOMPARE(f.state.value("autoZero").toMap().value("known"), QVariant(false));
        QCOMPARE(f.state.value("autoZero").toMap().value("active"), QVariant(true));
        f.observe("CA0"); f.get();
        QCOMPARE(f.state.value("autoZero").toMap().value("known"), QVariant(true));
        QCOMPARE(f.state.value("autoZero").toMap().value("active"), QVariant(false));
        QVERIFY(f.wires.isEmpty());
    }
    void autoZeroQueryTransportRefusalRetainsOnlyHistoricalActivity() {
        Fixture f; f.ready(); f.observe("CA1");
        f.answerReads = true;
        f.call("cw.receive.read", f.arguments());
        f.tick(6);
        f.accept = false;
        f.tick(); f.get();
        QCOMPARE(f.wires.count("CA;"), 1);
        const QVariantMap activity = f.state.value("autoZero").toMap();
        QCOMPARE(activity.value("known"), QVariant(false));
        QCOMPARE(activity.value("active"), QVariant(true));
        QCOMPARE(f.state.value("refresh").toMap().value("failed"), QVariant(1));
        QVERIFY(!f.state.value("refresh").toMap().value("busy").toBool());
        f.tick(3, 2500);
        QCOMPARE(f.wires.count("CA;"), 1);
        for (const QByteArray& wire : f.wires) { QVERIFY(wire != "CA0;" && wire != "CA1;"); }
    }
    void cachedGetterDoesNotQueryOrInventNativeValues() {
        Fixture f; f.get(); QVERIFY(f.wires.isEmpty()); QCOMPARE(f.state.value("controls").toList().size(), 4);
        for (const QVariant& raw : f.state.value("controls").toList()) {
            const QVariantMap row = raw.toMap(); QCOMPARE(row.value("known"), QVariant(false)); QVERIFY(!row.contains("value")); QVERIFY(!row.value("editable").toBool());
        }
        QCOMPARE(f.state.value("stream").toMap().value("aiKnown"), QVariant(false)); QVERIFY(f.events().isEmpty());
    }
    void explicitReadUsesOnlySevenDocumentedQueriesInAnyMode() {
        for (const QByteArray& mode : {QByteArray{}, QByteArray("FR3"), QByteArray("SF0000140740002")}) {
            Fixture f; f.ready(false); if (!mode.isEmpty()) { f.observe(mode); } f.wires.clear(); f.read();
            QCOMPARE(f.wires, QList<QByteArray>({"AI;", "CD0;", "CD1;", "CD3;", "CD5;", "DS0;", "CA;"}));
            QCOMPARE(f.state.value("refresh").toMap().value("observed"), QVariant(7));
            for (const QString& id : {QString("screen"), QString("threshold"), QString("filter"), QString("enabled")}) { QVERIFY(f.control(id).value("known").toBool()); }
        }
    }
    void everyNativeChoiceAndThresholdIndexIsAnObservation() {
        Fixture f; f.ready();
        for (int value = 1; value <= 30; ++value) {
            f.observe("CD1" + QByteArray::number(value).rightJustified(3, '0')); f.get();
            QCOMPARE(f.control("threshold").value("value"), QVariant(value));
        }
        for (const QByteArray& prefix : {QByteArray("CD0"), QByteArray("CD3"), QByteArray("CD5")}) {
            const QString id = prefix == "CD0" ? "screen" : prefix == "CD3" ? "filter" : "enabled";
            for (int value = 0; value <= (prefix == "CD3" ? 2 : 1); ++value) {
                f.observe(prefix + QByteArray::number(value)); f.get(); QCOMPARE(f.control(id).value("value"), QVariant(value));
            }
        }
        QVERIFY(f.wires.isEmpty());
    }
    void strictArgumentsRejectCoercionSentinelsAndInjection() {
        Fixture f; f.ready(); f.read(); f.wires.clear();
        for (const QVariant& value : {QVariant(true), QVariant("15"), QVariant(15.0), QVariant::fromValue<qlonglong>(15), QVariant(0), QVariant(31), QVariant(999)}) {
            const quint64 request = f.call("cw.receive.set", f.setArguments("threshold", value)); QVERIFY(f.errors.contains(request));
        }
        QVariantMap args = f.setArguments("threshold", 15); args["id"] = "threshold;TX"; QVERIFY(f.errors.contains(f.call("cw.receive.set", args)));
        args = f.setArguments("threshold", 15); args["revision"] = "01"; QVERIFY(f.errors.contains(f.call("cw.receive.set", args)));
        args = f.setArguments("threshold", 15); args["extra"] = true; QVERIFY(f.errors.contains(f.call("cw.receive.set", args)));
        QVERIFY(f.wires.isEmpty());
    }
    void settersRetainObservedValuesUntilMatchingReadback() {
        struct Change { QString id; QByteArray setter; QByteArray query; int value; };
        const QList<Change> changes{{"screen", "CD00;", "CD0;", 0}, {"screen", "CD01;", "CD0;", 1}, {"enabled", "CD50;", "CD5;", 0}, {"enabled", "CD51;", "CD5;", 1}, {"filter", "CD30;", "CD3;", 0}, {"filter", "CD31;", "CD3;", 1}, {"filter", "CD32;", "CD3;", 2}, {"threshold", "CD1001;", "CD1;", 1}, {"threshold", "CD1015;", "CD1;", 15}, {"threshold", "CD1030;", "CD1;", 30}};
        for (const Change& change : changes) {
            Fixture f; f.ready(); f.read(); f.wires.clear(); const QVariant prior = f.control(change.id).value("value");
            if (change.id == "screen") { f.screen = change.value; } else if (change.id == "enabled") { f.enabled = change.value; }
            else if (change.id == "filter") { f.filter = change.value; } else { f.threshold = change.value; }
            const quint64 request = f.call("cw.receive.set", f.setArguments(change.id, change.value));
            QVERIFY(f.results.contains(request)); QCOMPARE(f.results[request].toMap().value("verified"), QVariant(false));
            QCOMPARE(f.control(change.id).value("value"), prior); QCOMPARE(f.wires, QList<QByteArray>({change.setter}));
            f.tick(3); f.get(); QCOMPARE(f.wires, QList<QByteArray>({change.setter, change.query}));
            QCOMPARE(f.control(change.id).value("value"), QVariant(change.value));
            QCOMPARE(f.state.value("lastWrite").toMap().value("state"), QVariant("observed_consistent"));
        }
    }
    void onlyPositiveScreenAndDecodeRequireCwAndKnownSwlOnBlocksDecode() {
        for (const QString& id : {QString("screen"), QString("enabled")}) {
            Fixture f; f.ready(false); f.read(); f.wires.clear();
            QVERIFY(f.errors.contains(f.call("cw.receive.set", f.setArguments(id, 1))));
            QVERIFY(f.results.contains(f.call("cw.receive.set", f.setArguments(id, 0))));
        }
        for (const QByteArray& context : {QByteArray{}, QByteArray("FR3"), QByteArray("TS1")}) {
            for (const QString& id : {QString("threshold"), QString("filter")}) {
                Fixture f; f.ready(false); if (!context.isEmpty()) { f.observe(context); } f.read(); f.wires.clear();
                QVERIFY(f.results.contains(f.call("cw.receive.set", f.setArguments(id, id == "threshold" ? 17 : 2))));
                QCOMPARE(f.wires.first(), id == "threshold" ? QByteArray("CD1017;") : QByteArray("CD32;"));
            }
        }
        Fixture f; f.ready(); f.read(); f.observe("DS01"); f.get(); f.wires.clear();
        QVERIFY(f.errors.contains(f.call("cw.receive.set", f.setArguments("enabled", 1)))); QVERIFY(f.wires.isEmpty());
        QVERIFY(f.results.contains(f.call("cw.receive.set", f.setArguments("enabled", 0))));
    }
    void nativeCharactersDoNotRequireScreenOrEnableReadback() {
        Fixture f; f.ready(); f.wires.clear(); f.observe("CD2A"); f.observe("CD2 "); f.observe("CD2_"); f.get();
        QCOMPARE(f.events().size(), 3); QCOMPARE(f.events()[0].toMap().value("text"), QVariant("A"));
        QCOMPARE(f.events()[1].toMap().value("text"), QVariant(" ")); QCOMPARE(f.events()[2].toMap().value("kind"), QVariant("undecodable"));
        QVERIFY(!f.control("screen").value("known").toBool()); QVERIFY(!f.control("enabled").value("known").toBool()); QVERIFY(f.wires.isEmpty());
    }
    void bothCwDirectionsSupportPositiveFunctionsAndNativeText() {
        for (const QByteArray& mode : {QByteArray("3"), QByteArray("7")}) {
            Fixture f; f.ready(); f.observe("SF000014074000" + mode); f.read(); f.wires.clear();
            for (const QString& id : {QString("screen"), QString("enabled")}) {
                QVERIFY(f.results.contains(f.call("cw.receive.set", f.setArguments(id, 1)))); f.tick(3); f.get();
                QCOMPARE(f.state.value("lastWrite").toMap().value("state"), QVariant("observed_consistent"));
            }
            QVERIFY(f.wires.contains("CD01;")); QVERIFY(f.wires.contains("CD51;"));
            f.observe("CD2Z"); f.get(); QCOMPARE(f.events().size(), 1); QCOMPARE(f.events().first().toMap().value("text"), QVariant("Z"));
        }
    }
    void allDocumentedProsignsAreAtomicNativeEvents() {
        Fixture f; f.ready(); f.observe("AI2");
        const QList<QByteArray> signs{"BT", "AR", "AS", "HH", "SK", "KN", "BK", "SN"};
        for (const QByteArray& sign : signs) { f.observe("CD2" + sign); }
        f.get(); QCOMPARE(f.events().size(), 8);
        for (int index = 0; index < signs.size(); ++index) {
            const QVariantMap event = f.events()[index].toMap(); QCOMPARE(event.value("kind"), QVariant("prosign"));
            QCOMPARE(event.value("text").toString(), "<" + QString::fromLatin1(signs[index]) + ">"); QCOMPARE(event.value("rawAsciiHex").toString().toLower(), QString::fromLatin1(signs[index].toHex()));
        }
        QVERIFY(f.wires.isEmpty());
    }
    void everyAcceptedSingleAsciiFramePreservesItsLiteralByte() {
        Fixture f; f.ready(); f.observe("AI2"); QList<QByteArray> expected;
        for (int code = 0x20; code <= 0x7e; ++code) {
            if (code == ';') { continue; }
            const QByteArray byte(1, static_cast<char>(code)); expected.append(byte); f.observe("CD2" + byte);
        }
        f.get(); QCOMPARE(f.events().size(), expected.size());
        for (int index = 0; index < expected.size(); ++index) {
            const QVariantMap event = f.events()[index].toMap(); QCOMPARE(event.value("text").toString(), QString::fromLatin1(expected[index]));
            QCOMPARE(event.value("rawAsciiHex").toString().toLower(), QString::fromLatin1(expected[index].toHex()));
        }
        QVERIFY(f.wires.isEmpty());
    }
    void malformedCharactersAreCountedWithoutTextOrRadioActions() {
        Fixture f; f.ready(); f.observe("AI2"); f.get();
        const QList<QByteArray> invalid{"CD2", "CD2ZZ", "CD2ABC", "CD2;", QByteArray("CD2\x01", 4), QByteArray("CD2\x7f", 4), QByteArray("CD2\xc3\xa9", 5)};
        for (const QByteArray& body : invalid) { f.observe(body); }
        f.get(); QVERIFY(f.events().isEmpty()); QCOMPARE(f.state.value("transcript").toMap().value("invalid"), QVariant(invalid.size())); QVERIFY(f.wires.isEmpty());
    }
    void aiReservedRepliesWithdrawQualificationAndDoNotEnableAnything() {
        for (int invalid : {1, 3, 5}) {
            Fixture f; f.ready(); f.read(); f.ai = invalid; f.call("cw.receive.read", f.arguments()); f.tick(8); f.get();
            const QVariantMap stream = f.state.value("stream").toMap(); QCOMPARE(stream.value("aiKnown"), QVariant(false)); QCOMPARE(stream.value("ai"), QVariant(2)); QVERIFY(!stream.value("available").toBool());
            QVERIFY(!f.wires.contains("AI1;")); QVERIFY(!f.wires.contains("AI3;"));
        }
    }
    void allValidAiStatesRemainObservationsWithoutAnAiSetter() {
        for (int ai : {0, 2, 4}) {
            Fixture f; f.ready(); f.ai = ai; f.read(); f.get(); const QVariantMap stream = f.state.value("stream").toMap();
            QCOMPARE(stream.value("aiKnown"), QVariant(true)); QCOMPARE(stream.value("ai"), QVariant(ai));
            QCOMPARE(stream.value("available"), QVariant(ai != 0));
            for (const QByteArray& wire : f.wires) { QVERIFY(wire == "AI;" || !wire.startsWith("AI")); }
        }
    }
    void aiOffSwlAndNonCwParkNewTextWithoutErasingHistory() {
        for (const QByteArray& withdrawal : {QByteArray("AI0"), QByteArray("DS01"), QByteArray("SF0000140740002"), QByteArray("TX0")}) {
            Fixture f; f.ready(); f.observe("AI2"); f.observe("CD2A"); f.get(); QCOMPARE(f.events().size(), 1);
            f.observe(withdrawal); f.observe("CD2B"); f.get(); QCOMPARE(f.events().size(), 1); QVERIFY(!f.state.value("stream").toMap().value("available").toBool());
        }
    }
    void localClearHasNoRadioGateOrWireAndOrdersSameBatchText() {
        Fixture f; f.ready(); f.observe("CD2A"); f.get(); const quint64 old = f.state.value("transcript").toMap().value("revision").toString().toULongLong();
        f.observe("TX0"); f.get(); f.wires.clear(); f.clear(); f.get(); QVERIFY(f.events().isEmpty()); QVERIFY(f.wires.isEmpty());
        QVERIFY(f.state.value("transcript").toMap().value("revision").toString().toULongLong() > old);
        f.observe("RX"); f.observe("AI2");
        kenwood::KenwoodBackendTestAccess::batch(*f.backend, {"CD2B", "CD2C"}); f.get(); QCOMPARE(f.events().size(), 2);
    }
    void textChannelsShareOneRevisionAndBoundHistory() {
        Fixture f; f.ready(); f.observe("AI2"); f.retainTextPushes = false;
        quint64 last = f.state.value("stream").toMap().value("revision").toString().toULongLong(); int pushes = 0;
        f.onText = [&](const QVariantMap& push) {
            QCOMPARE(push.size(), 3); const QVariant revision = push.value("statusRevision");
            QCOMPARE(revision, push.value("stream").toMap().value("revision")); QCOMPARE(revision, push.value("transcript").toMap().value("revision"));
            const quint64 current = revision.toString().toULongLong(); QVERIFY(current > last); last = current; ++pushes;
        };
        for (int index = 0; index < 1030; ++index) { f.observe("CD2A"); }
        f.get(); QCOMPARE(f.events().size(), 1024); QCOMPARE(f.state.value("transcript").toMap().value("dropped"), QVariant(6));
        QCOMPARE(f.state.value("stream").toMap().value("revision"), f.state.value("transcript").toMap().value("revision"));
        QVERIFY(pushes >= 1030);
        QVERIFY(f.wires.isEmpty());
    }
    void cancelDeadlineAndRefusalNeverReplayOrUndo() {
        Fixture f; f.ready(); f.read(); f.wires.clear(); f.answerReads = false;
        f.call("cw.receive.set", f.setArguments("threshold", 17)); f.cancel(); f.tick(4, 2500); f.get();
        QCOMPARE(f.wires.count("CD1017;"), 1); QVERIFY(!f.wires.contains("CD1015;"));
        QCOMPARE(f.state.value("lastWrite").toMap().value("state"), QVariant("indeterminate"));
        Fixture refused; refused.ready(); refused.read(); refused.wires.clear(); refused.accept = false;
        refused.call("cw.receive.set", refused.setArguments("filter", 2)); refused.tick(4, 2500); refused.get();
        QCOMPARE(refused.wires.count("CD32;"), 1); QVERIFY(!refused.wires.contains("CD31;"));
        QVERIFY(refused.state.value("lastWrite").toMap().value("state") != QVariant("observed_consistent"));
    }
    void getterAndUnchangedReportsCannotPumpPredispatchVerification() {
        Fixture f; f.ready(); f.read(); f.wires.clear(); bool once = false;
        f.onStatus = [&](const QVariantMap& status) {
            if (!once && status.value("lastWrite").toMap().value("state") == "pending") {
                once = true; f.get(); f.tick(3); f.observe("CD1015"); QVERIFY(f.wires.isEmpty());
            }
        };
        const quint64 request = f.call("cw.receive.set", f.setArguments("threshold", 17));
        QVERIFY(once); QVERIFY(f.results.contains(request)); QCOMPARE(f.wires, QList<QByteArray>({"CD1017;"}));
    }
    void transientModeRoundTripRetiresOldUnsentControlIntent() {
        Fixture f; f.ready(); f.read(); f.wires.clear(); bool once = false;
        f.onStatus = [&](const QVariantMap& status) {
            if (!once && status.value("lastWrite").toMap().value("state") == "pending") {
                once = true; f.observe("SF0000140740002"); f.observe("SF0000140740003");
            }
        };
        const quint64 request = f.call("cw.receive.set", f.setArguments("threshold", 17));
        QVERIFY(once); QVERIFY(f.errors.contains(request)); QVERIFY(!f.wires.contains("CD1017;"));
    }
    void nestedClearAndNewTextCannotResurrectTheOldIngressRemainder() {
        Fixture f; f.ready(); f.observe("AI2"); bool once = false;
        f.onText = [&](const QVariantMap& push) {
            if (!once && !push.value("transcript").toMap().value("events").toList().isEmpty()) {
                once = true; f.get(); f.clear(); f.observe("CD2B");
            }
        };
        kenwood::KenwoodBackendTestAccess::batch(*f.backend, {"CD2A", "CD2C"}); f.get();
        QVERIFY(once); QCOMPARE(f.events().size(), 1); QCOMPARE(f.events().first().toMap().value("text"), QVariant("B")); QVERIFY(f.wires.isEmpty());
        const QVariantMap last = f.textPushes.last(); QCOMPARE(last.value("transcript").toMap().value("events"), QVariant(f.events()));
    }
    void sameFrequencyReceiveMemoryChangeKeepsHistoricalProvenance() {
        Fixture f; f.ready(); f.observe("AI2"); f.observe("CD2A"); f.get(); const QVariant original = f.events().first();
        f.observe("FB00014074000"); f.observe("SF1000140740003"); f.observe("FR1"); f.observe("CD2B"); f.get();
        QCOMPARE(f.events().size(), 2); QCOMPARE(f.events().first(), original);
        QVERIFY(f.events()[0].toMap().value("context") != f.events()[1].toMap().value("context"));
        QVERIFY(!f.events()[1].toMap().value("label").toString().isEmpty());
        QCOMPARE(f.wires, QList<QByteArray>({"SF1;"}));
    }
    void contextLostInsideSendKeepsTheOutcomeIndeterminate() {
        Fixture f; f.ready(); f.read(); f.wires.clear(); bool once = false;
        f.onWire = [&](const QByteArray& wire) {
            if (!once && wire == "CD1017;") { once = true; f.observe("TX0"); f.observe("RX"); }
        };
        const quint64 request = f.call("cw.receive.set", f.setArguments("threshold", 17));
        QVERIFY(once); QVERIFY(f.results.contains(request)); f.get();
        QCOMPARE(f.state.value("lastWrite").toMap().value("state"), QVariant("indeterminate"));
        f.threshold = 17; f.tick(3); f.get();
        QCOMPARE(f.state.value("lastWrite").toMap().value("state"), QVariant("indeterminate"));
        QCOMPARE(f.wires.count("CD1017;"), 1); QVERIFY(!f.wires.contains("CD1015;"));
    }
};
int main(int argc, char** argv) {
    TestSettingsProfile profile("kenwood-cw-receive"); if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv); KenwoodCwReceiveTest test; return QTest::qExec(&test, argc, argv);
}
#include "kenwood_cw_receive_test.moc"
