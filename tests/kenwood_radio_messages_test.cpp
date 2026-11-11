// Injected transport tests only. Primary PC tables distinguish message sources;
// no socket, softkey operation, automatic mute or physical radio is exercised.
#include "TestSettingsProfile.h"
#include "core/backends/kenwood/KenwoodBackend.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QVariant>
#include <QVariantMap>
#include <QMap>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QtTest>
#include <functional>
#include <memory>

namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& b) {
        b.m_clock.start(); b.m_protocol.start("messages-fixture", "messages-fixture", false, 0);
        b.m_protocol.receive("##CN1;", 1); b.m_protocol.receive("##ID1;", 2); b.ready();
    }
    static void observe(KenwoodBackend& b, const QByteArray& bytes) { b.adopt(bytes); }
    static void batch(KenwoodBackend& b, const QList<QByteArray>& bytes) {
        const quint64 sequence = ++b.m_controlIngressSequence;
        for (const QByteArray& message : bytes) { b.adopt(message, sequence); }
    }
    static void sameBatch(KenwoodBackend& b, const QByteArray& bytes) { b.adopt(bytes, b.m_controlIngressSequence); }
    static QVariantMap status(const KenwoodBackend& b) { return b.radioMessagesStatus(); }
    static void expire(KenwoodBackend& b, qint64 offset) { b.pollRadioMessageWrite(b.m_radioBeepDeadlineMs + offset); }
    static void intent(KenwoodBackend& b, int kind) {
        if (kind == 0) { b.m_keyIntent = true; }
        if (kind == 1) { b.m_cwIntent = true; }
        if (kind == 2) { b.m_atuIntent = true; }
        if (kind == 3) { b.m_tune.emplace(); }
        b.refreshRadioMessagesAvailability();
    }
};
}
namespace {
using namespace AetherSDR;
using namespace AetherSDR::kenwood;
struct Fixture {
    QList<QByteArray> wires;
    QVariantMap state;
    QMap<quint64, QVariant> results;
    QMap<quint64, QString> errors;
    std::function<void(const QByteArray&)> onWrite;
    std::function<void(const QVariantMap&)> onStatus;
    std::function<bool(const QByteArray&)> accept;
    std::unique_ptr<KenwoodBackend> backend;
    quint64 next = 1;
    Fixture() {
        backend = std::make_unique<KenwoodBackend>(KenwoodBackend::Transport{[this](const QByteArray& wire) {
            wires.append(wire);
            const bool accepted = accept ? accept(wire) : true;
            const auto callback = onWrite;
            if (callback) { callback(wire); }
            return accepted;
        }, {}});
        QObject::connect(backend.get(), &IRadioBackend::extensionStatus, backend.get(),
            [this](const QString&, const QString& kind, const QVariantMap& status) {
                if (kind != "radioMessages") { return; }
                state = status; const auto callback = onStatus; if (callback) { callback(status); }
            });
        QObject::connect(backend.get(), &IRadioBackend::extensionResult, backend.get(),
            [this](quint64 id, const QVariant& result) { results.insert(id, result); if (result.toMap().contains("message1")) { state = result.toMap(); } });
        QObject::connect(backend.get(), &IRadioBackend::extensionError, backend.get(),
            [this](quint64 id, const QString& error) { errors.insert(id, error); });
    }
    void observe(const QByteArray& bytes) { KenwoodBackendTestAccess::observe(*backend, bytes); }
    void ready(bool rx = true) {
        KenwoodBackendTestAccess::authenticate(*backend); observe("ID024"); if (rx) { observe("RX"); }
    }
    void load(int beep = 5) { observe("ME0000"); observe("ME1000"); observe("EX00100 " + QByteArray::number(beep).rightJustified(3, '0')); }
    quint64 request(const QString& verb, const QVariant& args = {}) {
        const quint64 id = next++; backend->invokeExtension("kenwood", verb, id, args); return id;
    }
    quint64 get(const QVariant& args = {}) { return request("radio.messages.get", args); }
    QVariantMap beep() const { return state.value("beep").toMap(); }
    QVariantMap message(int source) const { return state.value(source ? "message2" : "message1").toMap(); }
    QVariantMap arguments() const { return {{"context", state.value("context")}, {"revision", beep().value("revision")}}; }
    quint64 set(const QVariant& value) { QVariantMap args = arguments(); args.insert("value", value); return request("radio.beep.set", args); }
    quint64 reset() { return request("radio.beep.reset", arguments()); }
};
QByteArray report(int source, int id) { return "ME" + QByteArray::number(source) + QByteArray::number(id).rightJustified(3, '0'); }
}

class KenwoodRadioMessagesTest : public QObject {
    Q_OBJECT
private slots:
    void queryIsExplicitAndNeverOperatesSoftkeys() {
        Fixture f;
        const QVariantMap initial = KenwoodBackendTestAccess::status(*f.backend);
        QCOMPARE(initial.size(), 8); QCOMPARE(initial.value("statusRevision").typeId(), QMetaType::QString);
        QCOMPARE(initial.value("statusRevision").toString(), QString("0"));
        QVERIFY(!initial.value("message1").toMap().value("known").toBool());
        f.ready();
        for (const QByteArray& wire : f.wires) { QVERIFY(!wire.startsWith("ME")); QVERIFY(!wire.startsWith("EX00100")); }
        f.wires.clear(); const quint64 id = f.get(QVariantMap{});
        QCOMPARE(f.wires, (QList<QByteArray>{"ME0;", "ME1;", "EX00100;"})); QVERIFY(f.results.contains(id));
        QVERIFY(!f.beep().value("known").toBool()); QVERIFY(!f.beep().value("editable").toBool());
        f.wires.clear(); f.observe("ME0003"); f.observe("ME1003"); QVERIFY(f.wires.isEmpty());
        for (const QVariant& args : {QVariant(true), QVariant(1), QVariant(QVariantMap{{"extra", true}})}) {
            f.wires.clear(); QVERIFY(f.errors.contains(f.get(args))); QVERIFY(f.wires.isEmpty());
        }
        f.accept = [](const QByteArray& wire) { return wire != "ME1;"; }; f.wires.clear();
        QVERIFY(f.errors.contains(f.get())); QCOMPARE(f.wires, (QList<QByteArray>{"ME0;", "ME1;"}));
    }
    void officialTablesRemainSeparateForEveryRawId() {
        Fixture f; f.ready();
        QSet<int> known0{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 17, 18, 20, 21, 25, 26, 27, 28,
            34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 57, 58, 59, 60, 61, 62, 63, 64, 65, 67, 68,
            70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91};
        QSet<int> known1{0, 1, 2, 3, 4, 5, 6, 7, 9, 23, 24, 25};
        for (int id = 10; id <= 21; ++id) { known1.insert(id); }
        for (int source = 0; source < 2; ++source) {
            for (int id = 0; id <= 999; ++id) {
                f.observe(report(source, id)); const QVariantMap state = f.message(source);
                QVERIFY(state.value("known").toBool()); QCOMPARE(state.value("id").toInt(), id);
                QCOMPARE(state.value("recognized").toBool(), (source ? known1 : known0).contains(id));
                QVERIFY(!state.value("text").toString().isEmpty());
                if (!(source ? known1 : known0).contains(id)) { QVERIFY(state.value("text").toString().contains(QString::number(id).rightJustified(3, '0'))); }
            }
        }
        f.observe("ME0003"); const QString zero3 = f.message(0).value("text").toString(); f.observe("ME1003");
        QVERIFY(zero3.contains("equalizer", Qt::CaseInsensitive)); QVERIFY(f.message(1).value("text").toString().contains("USB"));
        f.observe("ME0010"); f.observe("ME1010"); QVERIFY(f.message(0).value("text") != f.message(1).value("text"));
        QVERIFY(f.message(0).value("text").toString().contains("Voice")); QVERIFY(f.message(1).value("text").toString().contains("DSP"));
    }
    void unknownPriorityMustNotInventWhichScreenWins() {
        Fixture f; f.ready(); f.observe("ME0003");
        QCOMPARE(f.state.value("activeMessage").toInt(), 0);
        f.observe("ME1000"); QCOMPARE(f.state.value("activeMessage").toInt(), 1);
        f.observe("ME1003"); QCOMPARE(f.state.value("activeMessage").toInt(), 2);
        QCOMPARE(f.message(0).value("id").toInt(), 3);
        f.observe("ME0000"); QCOMPARE(f.state.value("activeMessage").toInt(), 2);
        f.observe("ME1000"); QCOMPARE(f.state.value("activeMessage").toInt(), 0);
        Fixture highest; highest.ready(); highest.observe("ME1007"); QCOMPARE(highest.state.value("activeMessage").toInt(), 2);
        QVERIFY(!highest.message(0).value("known").toBool());
    }
    void malformedMessagesNeverChangeConfirmedSnapshot() {
        Fixture f; f.ready(); f.load();
        const QVariantMap before = f.state;
        for (const QByteArray& wire : {QByteArray("ME003"), QByteArray("ME00003"), QByteArray("ME0+01"), QByteArray("ME1-01"), QByteArray("ME0a03"),
                QByteArray("ME2003"), QByteArray("EX00100005"), QByteArray("EX00100 05"), QByteArray("EX00100 0005"), QByteArray("EX00100 +01"),
                QByteArray("EX00100 021"), QByteArray("EX00100 5.0"), QByteArray("EX00100\t005")}) { f.observe(wire); QCOMPARE(f.state, before); }
    }
    void allBeepIndicesAndInitialResetAreDistinct() {
        for (int value = 0; value <= 20; ++value) {
            Fixture f; f.ready(); f.get(); f.load(); f.wires.clear(); const quint64 id = f.set(value);
            QCOMPARE(f.wires, (QList<QByteArray>{"EX00100 " + QByteArray::number(value).rightJustified(3, '0') + ';', "EX00100;"}));
            QVERIFY(f.results.contains(id)); QVERIFY(f.results[id].toMap().value("sent").toBool()); QVERIFY(!f.results[id].toMap().value("verified").toBool());
            QVERIFY(f.beep().value("pending").toBool()); QCOMPARE(f.beep().value("value").toInt(), 5);
            f.observe("EX00100 " + QByteArray::number(value).rightJustified(3, '0'));
            QCOMPARE(f.beep().value("value").toInt(), value); QVERIFY(!f.beep().value("pending").toBool()); QVERIFY(f.beep().value("editable").toBool());
        }
        Fixture f; f.ready(); f.get(); f.load(9); f.wires.clear(); const quint64 id = f.reset();
        QCOMPARE(f.wires, (QList<QByteArray>{"EX001009;", "EX00100;"})); QVERIFY(f.results.contains(id));
        QCOMPARE(f.beep().value("value").toInt(), 9); QVERIFY(f.beep().value("pending").toBool());
        f.observe("EX00100 013"); QCOMPARE(f.beep().value("value").toInt(), 13); QVERIFY(!f.beep().value("pending").toBool());
    }
    void strictArgumentsAndSessionOwnership() {
        Fixture f; f.ready(); f.get(); f.load();
        for (const QVariant& value : {QVariant(-1), QVariant(21), QVariant(true), QVariant(5.0), QVariant("5"), QVariant()}) {
            f.wires.clear(); QVERIFY(f.errors.contains(f.set(value))); QVERIFY(f.wires.isEmpty());
        }
        QVariantMap old = f.arguments(); old.insert("value", 7); f.observe("EX00100 006"); f.wires.clear();
        QVERIFY(f.errors.contains(f.request("radio.beep.set", old))); QVERIFY(f.wires.isEmpty());
        for (const QString& field : {QString("context"), QString("revision")}) {
            QVariantMap args = f.arguments(); args.insert("value", 7); args[field] = args[field].toString().toULongLong(); f.wires.clear();
            QVERIFY(f.errors.contains(f.request("radio.beep.set", args))); QVERIFY(f.wires.isEmpty());
        }
        QVariantMap reset = f.arguments(); reset.insert("value", 0); f.wires.clear(); QVERIFY(f.errors.contains(f.request("radio.beep.reset", reset))); QVERIFY(f.wires.isEmpty());
        const QVariantMap retired = f.arguments(); f.backend->disconnectRadio(); f.ready(); f.load(); f.wires.clear();
        QVariantMap stale = retired; stale.insert("value", 7); QVERIFY(f.errors.contains(f.request("radio.beep.set", stale))); QVERIFY(f.wires.isEmpty());
    }
    void unsafeReceiveStateBlocksWritesButNotPassiveQueries() {
        for (int kind = 0; kind < 6; ++kind) {
            Fixture f; f.ready(kind != 0); f.get(); f.load();
            if (kind == 1) { f.observe("TX0"); }
            if (kind >= 2) { KenwoodBackendTestAccess::intent(*f.backend, kind - 2); }
            f.wires.clear(); QVERIFY(f.errors.contains(f.set(7))); QVERIFY(f.wires.isEmpty());
            f.get(); QCOMPARE(f.wires, (QList<QByteArray>{"ME0;", "ME1;", "EX00100;"}));
        }
    }
    void sameBatchReadbackCannotAcknowledgeWrite() {
        Fixture f; f.ready(); f.get(); f.load(); f.wires.clear(); f.set(7);
        KenwoodBackendTestAccess::sameBatch(*f.backend, "EX00100 007");
        QVERIFY(f.beep().value("pending").toBool()); QCOMPARE(f.beep().value("value").toInt(), 5);
        f.observe("EX00100 007"); QVERIFY(!f.beep().value("pending").toBool()); QCOMPARE(f.beep().value("value").toInt(), 7);
    }
    void predispatchGetAndUnchangedReportCannotLeakQueriesOrAck() {
        Fixture f; f.ready(); f.get(); f.load(); bool nested = false;
        f.onStatus = [&](const QVariantMap& state) {
            if (nested || !state.value("beep").toMap().value("pending").toBool()) { return; }
            nested = true; f.get(); f.observe("EX00100 005");
        };
        f.wires.clear(); f.set(7); QVERIFY(nested);
        QCOMPARE(f.wires, (QList<QByteArray>{"EX00100 007;", "EX00100;"}));
        QVERIFY(f.beep().value("pending").toBool());
        KenwoodBackendTestAccess::sameBatch(*f.backend, "EX00100 007"); QVERIFY(f.beep().value("pending").toBool());
        f.observe("EX00100 007"); QVERIFY(!f.beep().value("pending").toBool());
    }
    void changedPredispatchObservationRetiresObsoleteProducer() {
        Fixture f; f.ready(); f.get(); f.load(); bool nested = false;
        f.onStatus = [&](const QVariantMap& state) {
            if (nested || !state.value("beep").toMap().value("pending").toBool()) { return; }
            nested = true; f.observe("EX00100 009");
        };
        f.wires.clear(); f.set(7); QVERIFY(nested);
        QVERIFY(!f.wires.contains("EX00100 007;")); QCOMPARE(f.beep().value("value").toInt(), 9); QVERIFY(!f.beep().value("pending").toBool());
    }
    void nestedReplacementTicketSurvivesOldProducerCleanup() {
        Fixture f; f.ready(); f.get(); f.load(); bool nested = false;
        f.onStatus = [&](const QVariantMap& state) {
            if (nested || !state.value("beep").toMap().value("pending").toBool()) { return; }
            nested = true; f.observe("EX00100 009"); f.set(11);
        };
        f.wires.clear(); f.set(7); QVERIFY(nested);
        QCOMPARE(f.wires, (QList<QByteArray>{"EX00100 011;", "EX00100;"}));
        QCOMPARE(f.beep().value("value").toInt(), 9); QVERIFY(f.beep().value("pending").toBool());
        f.observe("EX00100 011"); QCOMPARE(f.beep().value("value").toInt(), 11); QVERIFY(!f.beep().value("pending").toBool());
    }
    void oldResumedBatchCannotReplaceNewerObservations() {
        Fixture f; f.ready(); f.load(); bool nested = false;
        f.onStatus = [&](const QVariantMap& state) {
            if (!nested && state.value("message1").toMap().value("id").toInt() == 1) { nested = true; f.observe("ME0002"); f.observe("EX00100 009"); }
        };
        KenwoodBackendTestAccess::batch(*f.backend, {"ME0001", "ME0003", "EX00100 006"});
        QVERIFY(nested); QCOMPARE(f.message(0).value("id").toInt(), 2); QCOMPARE(f.beep().value("value").toInt(), 9);
    }
    void deadlinesAndReadFailureRemainUnverifiedUntilFreshReadback() {
        Fixture f; f.ready(); f.get(); f.load(); f.set(7); const quint64 revision = f.beep().value("revision").toString().toULongLong();
        KenwoodBackendTestAccess::expire(*f.backend, -1); QVERIFY(f.beep().value("pending").toBool());
        KenwoodBackendTestAccess::expire(*f.backend, 0); QVERIFY(!f.beep().value("pending").toBool()); QVERIFY(!f.beep().value("editable").toBool());
        QCOMPARE(f.beep().value("value").toInt(), 5); QCOMPARE(f.beep().value("revision").toString().toULongLong(), revision);
        f.wires.clear(); QVERIFY(f.errors.contains(f.set(9))); QVERIFY(f.wires.isEmpty());
        f.get(); f.observe("EX00100 007"); QVERIFY(f.beep().value("editable").toBool());
        f.accept = [](const QByteArray& wire) { return wire != "EX00100;"; }; f.wires.clear(); const quint64 id = f.set(8);
        QVERIFY(f.results[id].toMap().value("sent").toBool()); QVERIFY(!f.results[id].toMap().value("verified").toBool());
        QVERIFY(!f.beep().value("pending").toBool()); QVERIFY(!f.beep().value("editable").toBool()); QCOMPARE(f.beep().value("value").toInt(), 7);
        f.accept = {}; f.get(); f.observe("EX00100 008"); QVERIFY(f.beep().value("editable").toBool());
    }
    void failureAndReentrantTeardownDoNotReplayOrContinue() {
        Fixture rejected; rejected.ready(); rejected.get(); rejected.load();
        rejected.accept = [](const QByteArray& wire) { return !wire.startsWith("EX00100 "); }; rejected.wires.clear();
        QVERIFY(rejected.errors.contains(rejected.set(7))); QCOMPARE(rejected.beep().value("value").toInt(), 5); QVERIFY(!rejected.beep().value("pending").toBool());
        Fixture withdrawn; withdrawn.ready(); withdrawn.get(); withdrawn.load(); bool nested = false;
        withdrawn.onStatus = [&](const QVariantMap& state) { if (!nested && state.value("beep").toMap().value("pending").toBool()) { nested = true; withdrawn.observe("TX0"); } };
        withdrawn.wires.clear(); withdrawn.set(7); QVERIFY(nested); QVERIFY(!withdrawn.wires.contains("EX00100 007;"));
        Fixture deleted; deleted.ready(); deleted.get(); deleted.load(); QPointer<KenwoodBackend> guard(deleted.backend.get());
        deleted.onWrite = [&](const QByteArray& wire) { if (wire.startsWith("EX00100 ")) { deleted.backend.reset(); } };
        deleted.wires.clear(); deleted.set(7); QVERIFY(!guard); QCOMPARE(deleted.wires, (QList<QByteArray>{"EX00100 007;"}));
        Fixture disconnected; disconnected.ready(); disconnected.get(); disconnected.load();
        disconnected.onWrite = [&](const QByteArray& wire) { if (wire.startsWith("EX00100 ")) { disconnected.backend->disconnectRadio(); } };
        disconnected.wires.clear(); disconnected.set(7); QVERIFY(!disconnected.wires.contains("EX00100;"));
        disconnected.onWrite = {}; disconnected.wires.clear(); disconnected.ready();
        for (const QByteArray& wire : disconnected.wires) { QVERIFY(!wire.startsWith("EX00100 ")); }
    }
    void producerChronologyDoesNotRegressOnNestedDelivery() {
        Fixture f; f.ready(); f.load(); bool nested = false; QList<QVariantMap> delivered;
        f.onStatus = [&](const QVariantMap& state) { if (!nested && state.value("message1").toMap().value("id").toInt() == 1) { nested = true; f.observe("ME0002"); } };
        QObject::connect(f.backend.get(), &IRadioBackend::extensionStatus, f.backend.get(), [&](const QString&, const QString& kind, const QVariantMap& state) { if (kind == "radioMessages") { delivered.append(state); } });
        f.observe("ME0001"); QVERIFY(nested); QCOMPARE(delivered.size(), 2);
        QVERIFY(delivered.first().value("statusRevision").toString().toULongLong() > delivered.last().value("statusRevision").toString().toULongLong());
        QCOMPARE(KenwoodBackendTestAccess::status(*f.backend).value("message1").toMap().value("id").toInt(), 2);
        const quint64 before = KenwoodBackendTestAccess::status(*f.backend).value("statusRevision").toString().toULongLong();
        f.backend->disconnectRadio(); QVERIFY(KenwoodBackendTestAccess::status(*f.backend).value("statusRevision").toString().toULongLong() >= before);
    }
};
int main(int argc, char** argv) {
    TestSettingsProfile profile("kenwood-radio-messages"); if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv); KenwoodRadioMessagesTest test; return QTest::qExec(&test, argc, argv);
}
#include "kenwood_radio_messages_test.moc"
