// Independent literal DS0/DS1/DS2/DS3 fixtures: PC Reference printed 20-22.
#include "TestSettingsProfile.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include <QByteArray>
#include <QCoreApplication>
#include <QList>
#include <QMap>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QtTest>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& backend) {
        backend.m_clock.start();
        backend.m_protocol.start("display-fixture", "fixture-only", false, 0);
        backend.m_protocol.receive("##CN1;", 1);
        backend.m_protocol.receive("##ID1;", 2);
        backend.ready();
    }
    static void observe(KenwoodBackend& backend, const QByteArray& body) { backend.adopt(body); }
    static void poll(KenwoodBackend& backend, qint64 now) { backend.pollOperatingDisplay(now); }
    static void batch(KenwoodBackend& backend, const QList<QByteArray>& bodies) {
        const quint64 batch = ++backend.m_controlIngressSequence;
        for (const QByteArray& body : bodies) { backend.adopt(body, batch); }
    }
    static std::optional<bool> transverter(const KenwoodBackend& backend) { return backend.m_transverterEnabled; }

};
}
namespace {
using namespace AetherSDR;
using Backend = kenwood::KenwoodBackend;
QByteArray screenReply(int screen) { return "DS1" + QByteArray::number(screen).rightJustified(3, '0'); }
struct Fixture {
    QList<QByteArray> wires;
    QVariantMap state;
    QMap<quint64, QVariant> results;
    QMap<quint64, QString> errors;
    std::function<void(const QVariantMap&)> onStatus;
    std::function<void(const QByteArray&)> onWrite;
    std::unique_ptr<Backend> backend;
    int bank = 0, basic = 0, screen = 0, edit = 0;
    bool xv = false, answerReads = false, accept = true;
    qint64 now = 0;
    quint64 nextRequest = 1;
    Fixture() {
        backend = std::make_unique<Backend>(Backend::Transport{[this](const QByteArray& wire) {
            wires.append(wire); const bool accepted = accept; const auto callback = onWrite;
            if (callback) { callback(wire); }
            if (backend && accepted && answerReads) {
                const QByteArray response = reply(wire);
                if (!response.isEmpty()) { observe(response); }
            }
            return accepted;
        }, {}});
        QObject::connect(backend.get(), &IRadioBackend::extensionStatus, backend.get(),
            [this](const QString&, const QString& kind, const QVariantMap& value) {
                if (kind == "operatingDisplay") { state = value; const auto callback = onStatus; if (callback) { callback(value); } }
            });
        QObject::connect(backend.get(), &IRadioBackend::extensionResult, backend.get(),
            [this](quint64 id, const QVariant& value) { results[id] = value; if (value.toMap().contains("basicChoices")) { state = value.toMap(); } });
        QObject::connect(backend.get(), &IRadioBackend::extensionError, backend.get(),
            [this](quint64 id, const QString& error) { errors[id] = error; });
    }
    QByteArray reply(const QByteArray& query) const {
        if (query == "MF;") { return "MF" + QByteArray::number(bank); }
        if (query == "XV;") { return xv ? "XV1" : "XV0"; }
        if (query == "DS0;") { return "DS0" + QByteArray::number(basic); }
        if (query == "DS1;") { return screenReply(screen); }
        if (query == "DS2;") { return "DS2" + QByteArray::number(edit); }
        return {};
    }
    void ready(bool knownXv = true) {
        kenwood::KenwoodBackendTestAccess::authenticate(*backend);
        observe("ID024"); if (knownXv) { observe("XV0"); } observe("RX"); get();
    }
    void observe(const QByteArray& body) { kenwood::KenwoodBackendTestAccess::observe(*backend, body); }
    quint64 call(const QString& verb, const QVariant& args = {}) {
        const quint64 id = nextRequest++; backend->invokeExtension("kenwood", verb, id, args); return id;
    }
    void get() { call("operating.display.get"); }
    QVariantMap arguments() const { return {{"context", state.value("context")}}; }
    QVariantMap setArguments(int value) const {
        QVariantMap args = arguments(); args["basicRevision"] = state.value("basicRevision"); args["value"] = value; return args;
    }
    QVariantMap escapeArguments() const {
        QVariantMap args = arguments(); args["screenRevision"] = state.value("screenRevision"); args["editRevision"] = state.value("editRevision"); return args;
    }
    QVariantMap progress() const { return state.value("refresh").toMap(); }
    QVariantMap lastWrite() const { return state.value("lastWrite").toMap(); }
    void tick(int count = 1, qint64 advance = 100) {
        for (int index = 0; index < count && backend; ++index) { now += advance; kenwood::KenwoodBackendTestAccess::poll(*backend, now); }
    }
    void read() { answerReads = true; call("operating.display.read", arguments()); tick(8); }
    void cancel() { QVariantMap args = arguments(); args["token"] = progress().value("token"); call("operating.display.cancel", args); }
};
}
class KenwoodOperatingDisplayTest final : public QObject {
    Q_OBJECT
private slots:
    void cachedGetterDoesNotFabricateBasicScreenOrQueryTheRadio() {
        Fixture fixture; fixture.get(); QVERIFY(fixture.wires.isEmpty());
        for (const QString& key : {QString("bankKnown"), QString("xvKnown"), QString("basicKnown"), QString("screenKnown"), QString("editKnown")}) { QCOMPARE(fixture.state.value(key), QVariant(false)); }
        QVERIFY(!fixture.state.contains("basic")); QVERIFY(!fixture.state.value("screen").toMap().contains("id"));
        QVERIFY(!fixture.state.value("edit").toMap().contains("value"));
        QCOMPARE(fixture.state.value("basicChoices").toList().size(), 2);
        QCOMPARE(fixture.state.value("editChoices").toList().size(), 6);
        fixture.ready(); fixture.wires.clear(); fixture.get(); fixture.tick(); QVERIFY(fixture.wires.isEmpty());
        const QVariantMap extension = fixture.backend->capabilities().extensions.value("kenwood").toMap();
        QCOMPARE(extension.value("nativeOperatingDisplayVersion"), QVariant(1));
        QCOMPARE(extension.value("nativeOperatingDisplayVerbs").toStringList().size(), 5);
    }
    void allThousandFunctionIdsPreservePrimaryKnownReservedAndUnlistedRoles() {
        Fixture fixture; fixture.ready();
        int declared = 0, reserved = 0;
        for (int id = 0; id <= 999; ++id) {
            fixture.observe(screenReply(id)); const QVariantMap screen = fixture.state.value("screen").toMap();
            const bool recognized = id <= 186 && id != 175;
            const bool unused = id == 1 || id == 2 || (id >= 49 && id <= 127);
            QCOMPARE(fixture.state.value("screenKnown"), QVariant(true));
            QCOMPARE(screen.value("id"), QVariant(id));
            QCOMPARE(screen.value("raw").toString(), QString::number(id).rightJustified(3, '0'));
            QCOMPARE(screen.value("recognized"), QVariant(recognized)); QCOMPARE(screen.value("reserved"), QVariant(unused));
            QVERIFY(!screen.value("label").toString().isEmpty());
            if (recognized) { ++declared; } if (unused) { ++reserved; }
        }
        QCOMPARE(declared, 186); QCOMPARE(reserved, 81);
        fixture.observe("DS1010"); QVERIFY(fixture.state.value("screen").toMap().value("label").toString().contains("Processor", Qt::CaseInsensitive));
        const QVariantMap processor = fixture.state.value("screen").toMap(); fixture.observe("ME0001");
        QCOMPARE(fixture.state.value("screen").toMap(), processor);
    }
    void bothBasicStatesAndAllSixEditStatesAreNativeObservations() {
        Fixture fixture; fixture.ready();
        for (int value = 0; value < 2; ++value) { fixture.observe("DS0" + QByteArray::number(value)); QCOMPARE(fixture.state.value("basic"), QVariant(value)); QCOMPARE(fixture.state.value("basicKnown"), QVariant(true)); }
        for (int value = 0; value < 6; ++value) { fixture.observe("DS2" + QByteArray::number(value)); QCOMPARE(fixture.state.value("edit").toMap().value("value"), QVariant(value)); QCOMPARE(fixture.state.value("editKnown"), QVariant(true)); }
    }
    void explicitReadHasFiveQueriesAndFailedFieldsRetainLastObservation() {
        Fixture fixture; fixture.ready(); fixture.wires.clear(); fixture.read();
        QCOMPARE(fixture.wires, QList<QByteArray>({"MF;", "XV;", "DS0;", "DS1;", "DS2;"}));
        QCOMPARE(fixture.progress().value("observed"), QVariant(5)); QVERIFY(!fixture.progress().value("busy").toBool());
        fixture.observe("DS25"); fixture.answerReads = false; fixture.call("operating.display.read", fixture.arguments());
        const QList<QByteArray> responses{"MF0", "XV0", "DS00", "DS1175", "DS26"};
        for (const QByteArray& response : responses) { fixture.tick(); fixture.observe(response); }
        QCOMPARE(fixture.progress().value("failed"), QVariant(1));
        QCOMPARE(fixture.state.value("editKnown"), QVariant(false));
        QCOMPARE(fixture.state.value("edit").toMap().value("value"), QVariant(5));
        QVERIFY(fixture.state.value("screenKnown").toBool());
        QVERIFY(!fixture.state.value("screen").toMap().value("recognized").toBool());
    }
    void globalDisplayWorksWithoutVfoModeBankOrDiagnosticPrerequisites() {
        for (const QByteArray& source : {QByteArray{}, QByteArray("FR3"), QByteArray("TS1")}) {
            Fixture fixture; fixture.ready(); if (!source.isEmpty()) { fixture.observe(source); }
            fixture.observe("DS01"); fixture.wires.clear();
            const quint64 id = fixture.call("operating.display.set", fixture.setArguments(0));
            QCOMPARE(fixture.wires, QList<QByteArray>({"DS00;"})); QVERIFY(fixture.results.contains(id));
            QCOMPARE(fixture.results[id].toMap().value("verified"), QVariant(false));
        }
    }
    void swlEntryRequiresKnownOffWhileStandardExitAllowsUnknown() {
        for (int xv : {-1, 0}) {
            Fixture exit; exit.ready(false); exit.observe("DS01"); if (xv >= 0) { exit.observe(xv ? "XV1" : "XV0"); } exit.wires.clear();
            QVERIFY(exit.results.contains(exit.call("operating.display.set", exit.setArguments(0))));
            QCOMPARE(exit.wires, QList<QByteArray>({"DS00;"}));
            Fixture enter; enter.ready(false); enter.observe("DS00"); if (xv >= 0) { enter.observe(xv ? "XV1" : "XV0"); } enter.wires.clear();
            const quint64 id = enter.call("operating.display.set", enter.setArguments(1));
            if (xv == 0) { QVERIFY(enter.results.contains(id)); QCOMPARE(enter.wires, QList<QByteArray>({"DS01;"})); }
            else { QVERIFY(enter.errors.contains(id)); QVERIFY(enter.wires.isEmpty()); }
        }
    }
    void functionAndEditScreensAreNotAnInventedBasicDisplayBan() {
        for (int id : {0, 1, 3, 175, 186, 999}) {
            Fixture fixture; fixture.ready(); fixture.observe("DS01"); fixture.observe(screenReply(id)); fixture.observe("DS25"); fixture.wires.clear();
            QVERIFY(fixture.results.contains(fixture.call("operating.display.set", fixture.setArguments(0))));
            QCOMPARE(fixture.wires, QList<QByteArray>({"DS00;"}));
        }
    }
    void strictArgumentsCannotCoerceOrInjectActions() {
        Fixture fixture; fixture.ready(); fixture.read(); fixture.wires.clear();
        for (const QVariant& value : {QVariant(true), QVariant("1"), QVariant::fromValue(1LL), QVariant(1.0), QVariant(-1), QVariant(2)}) {
            QVariantMap args = fixture.setArguments(0); args["value"] = value;
            QVERIFY(fixture.errors.contains(fixture.call("operating.display.set", args)));
        }
        for (const QString& revision : {QString("01"), QString("18446744073709551616"), QString("0;DS3")}) {
            QVariantMap args = fixture.escapeArguments(); args["screenRevision"] = revision;
            QVERIFY(fixture.errors.contains(fixture.call("operating.display.escape", args)));
        }
        QVariantMap extra = fixture.escapeArguments(); extra["screen"] = 3;
        QVERIFY(fixture.errors.contains(fixture.call("operating.display.escape", extra))); QVERIFY(fixture.wires.isEmpty());
    }
    void basicSetterRetainsObservedStateAndCompletesThreePostreads() {
        for (int target = 0; target < 2; ++target) {
            Fixture fixture; fixture.ready(); fixture.read(); fixture.wires.clear(); fixture.answerReads = false;
            const quint64 id = fixture.call("operating.display.set", fixture.setArguments(target));
            QCOMPARE(fixture.wires, QList<QByteArray>({target ? "DS01;" : "DS00;"}));
            QCOMPARE(fixture.state.value("basic"), QVariant(0));
            QCOMPARE(fixture.results[id].toMap().value("verified"), QVariant(false));
            fixture.basic = target; fixture.screen = 175; fixture.edit = 4; fixture.answerReads = true; fixture.tick(5);
            QCOMPARE(fixture.wires, QList<QByteArray>({target ? "DS01;" : "DS00;", "DS0;", "DS1;", "DS2;"}));
            QCOMPARE(fixture.state.value("basic"), QVariant(target));
            QCOMPARE(fixture.state.value("screen").toMap().value("id"), QVariant(175));
            QCOMPARE(fixture.state.value("edit").toMap().value("value"), QVariant(4));
            QCOMPARE(fixture.progress().value("observed"), QVariant(3));
            QCOMPARE(fixture.lastWrite().value("state").toString(), QString("observed_consistent"));
        }
    }
    void escapeIsExplicitAndPostreadsDoNotAssumeAnAllZeroOutcome() {
        for (int edit = 0; edit < 6; ++edit) {
            Fixture fixture; fixture.ready(); fixture.observe("DS1175"); fixture.observe("DS2" + QByteArray::number(edit)); fixture.wires.clear();
            const quint64 id = fixture.call("operating.display.escape", fixture.escapeArguments());
            QCOMPARE(fixture.wires, QList<QByteArray>({"DS3;"}));
            QVERIFY(fixture.results.contains(id)); QCOMPARE(fixture.results[id].toMap().value("verified"), QVariant(false));
            fixture.screen = 133; fixture.edit = 1; fixture.basic = 1; fixture.answerReads = true; fixture.tick(5);
            QCOMPARE(fixture.wires, QList<QByteArray>({"DS3;", "DS0;", "DS1;", "DS2;"}));
            QCOMPARE(fixture.progress().value("observed"), QVariant(3));
            QCOMPARE(fixture.state.value("screen").toMap().value("id"), QVariant(133));
            QCOMPARE(fixture.state.value("edit").toMap().value("value"), QVariant(1));
            QCOMPARE(fixture.lastWrite().value("state").toString(), QString("sent_unverified"));
        }
    }
    void cancelDeadlineAndRefusalNeverUndoOrRepeatAChange() {
        Fixture fixture; fixture.ready(); fixture.read(); fixture.wires.clear(); fixture.answerReads = false;
        fixture.call("operating.display.set", fixture.setArguments(1)); fixture.cancel(); fixture.tick(10);
        QCOMPARE(fixture.wires, QList<QByteArray>({"DS01;"})); QCOMPARE(fixture.lastWrite().value("state").toString(), QString("indeterminate"));
        Fixture timeout; timeout.ready(); timeout.observe("DS1003"); timeout.observe("DS20"); timeout.wires.clear();
        timeout.call("operating.display.escape", timeout.escapeArguments()); timeout.tick(); timeout.tick(1, 2001);
        QCOMPARE(timeout.lastWrite().value("state").toString(), QString("indeterminate"));
        for (const QByteArray& wire : timeout.wires) { QVERIFY(wire == "DS3;" || wire == "DS0;"); }
        Fixture refused; refused.ready(); refused.observe("DS01"); refused.accept = false; refused.wires.clear();
        const quint64 id = refused.call("operating.display.set", refused.setArguments(0));
        QVERIFY(refused.errors.contains(id)); refused.tick(10); QCOMPARE(refused.wires, QList<QByteArray>({"DS00;"}));
    }
    void gettersAndUnchangedReportsCannotPumpPredispatchReads() {
        Fixture fixture; fixture.ready(); fixture.read(); fixture.wires.clear(); bool nested = false;
        fixture.onStatus = [&](const QVariantMap& status) {
            if (!nested && status.value("lastWrite").toMap().value("state") == "pending") {
                nested = true; fixture.get(); fixture.observe("DS00"); fixture.observe("XV0");
                QVERIFY(fixture.wires.isEmpty());
            }
        };
        const quint64 id = fixture.call("operating.display.set", fixture.setArguments(1));
        QVERIFY(nested); QVERIFY(fixture.results.contains(id)); QCOMPARE(fixture.wires, QList<QByteArray>({"DS01;"}));
    }
    void transientTransmitRestoreRetiresOldDisplayIntentBeforeWire() {
        Fixture fixture; fixture.ready(); fixture.read(); fixture.wires.clear(); bool nested = false, armed = false;
        QObject::connect(fixture.backend.get(), &IRadioBackend::transmitChanged, fixture.backend.get(), [&](const TransmitDelta& delta) {
            if (delta.mox.value_or(false) && !nested) { nested = true; fixture.observe("RX"); }
        });
        fixture.onStatus = [&](const QVariantMap& status) { if (!armed && status.value("lastWrite").toMap().value("state") == "pending") { armed = true; fixture.observe("TX0"); } };
        const quint64 id = fixture.call("operating.display.set", fixture.setArguments(1));
        QVERIFY(nested); QVERIFY(fixture.results.contains(id) || fixture.errors.contains(id));
        for (const QByteArray& wire : fixture.wires) { QVERIFY(wire != "DS01;"); }
    }
    void newerXvObservationSurvivesAnOlderOuterDisplayCallback() {
        Fixture fixture; fixture.ready(); fixture.observe("DS00"); bool nested = false;
        fixture.onStatus = [&](const QVariantMap& state) {
            if (!nested && state.value("basic") == QVariant(1)) {
                nested = true; fixture.observe("XV0");
            }
        };
        fixture.wires.clear();
        kenwood::KenwoodBackendTestAccess::batch(*fixture.backend, {"DS01", "XV1"});
        QVERIFY(nested); QVERIFY(fixture.backend->isConnected());
        const std::optional<bool> xv = kenwood::KenwoodBackendTestAccess::transverter(*fixture.backend);
        QVERIFY(xv.has_value()); QVERIFY(!*xv);
        QCOMPARE(fixture.state.value("xvEnabled"), QVariant(false)); QVERIFY(fixture.state.value("xvKnown").toBool());
        for (const QByteArray& wire : fixture.wires) { QVERIFY(wire != "RX;" && !wire.startsWith("TX") && !wire.startsWith("AC") && !wire.startsWith("KY")); }
        fixture.wires.clear(); const quint64 id = fixture.call("operating.display.set", fixture.setArguments(1));
        QVERIFY(fixture.results.contains(id)); QCOMPARE(fixture.wires, QList<QByteArray>({"DS01;"}));
    }
    void admittedSameBatchXvOnRetiresSessionBeforeLaterOff() {
        for (const QList<QByteArray>& messages : {QList<QByteArray>{"XV0", "XV1", "XV0"}, QList<QByteArray>{"XV1", "XV0"}}) {
            Fixture fixture; fixture.ready(); fixture.observe("DS00");
            const QVariantMap args = fixture.setArguments(1);
            fixture.wires.clear(); kenwood::KenwoodBackendTestAccess::batch(*fixture.backend, messages);
            QVERIFY(!fixture.backend->isConnected());
            fixture.get();
            QVERIFY(!fixture.state.value("xvKnown").toBool());
            QVERIFY(!kenwood::KenwoodBackendTestAccess::transverter(*fixture.backend).has_value());
            const quint64 id = fixture.call("operating.display.set", args);
            QVERIFY(fixture.errors.contains(id)); QVERIFY(fixture.wires.isEmpty());
        }
    }

};
int main(int argc, char** argv) {
    TestSettingsProfile profile("kenwood-operating-display"); if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv); KenwoodOperatingDisplayTest test; return QTest::qExec(&test, argc, argv);
}
#include "kenwood_operating_display_test.moc"
