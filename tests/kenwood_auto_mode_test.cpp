// Literal socket-free AM/AS expectations follow PC Reference printed 3/5.
#include "TestSettingsProfile.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include <QByteArray>
#include <QCoreApplication>
#include <QList>
#include <QMap>
#include <QPointer>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtTest>
#include <functional>
#include <memory>

namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& backend) {
        backend.m_clock.start();
        backend.m_protocol.start("auto-mode-fixture", "fixture-only", false, 0);
        backend.m_protocol.receive("##CN1;", 1);
        backend.m_protocol.receive("##ID1;", 2);
        backend.ready();
    }
    static void observe(KenwoodBackend& backend, const QByteArray& body) { backend.adopt(body); }
    static void poll(KenwoodBackend& backend, qint64 now) { backend.pollAutoMode(now); }
    static void batch(KenwoodBackend& backend, const QList<QByteArray>& bodies) {
        const quint64 ingress = ++backend.m_controlIngressSequence;
        for (const QByteArray& body : bodies) { backend.adopt(body, ingress); }
    }
    static int source(const KenwoodBackend& backend) { return backend.m_receiveVfo; }
};
}
namespace {
using namespace AetherSDR;
using Backend = kenwood::KenwoodBackend;
struct Entry { qint64 hz = 0; char mode = '0'; };
QByteArray division(int index, qint64 hz = 0, char mode = '0') {
    return "AS2" + QByteArray::number(index).rightJustified(2, '0')
        + QByteArray::number(hz).rightJustified(11, '0') + mode;
}
struct Fixture {
    QList<QByteArray> wires;
    QVariantMap state;
    QMap<quint64, QVariant> results;
    QMap<quint64, QString> errors;
    std::function<void(const QVariantMap&)> onStatus;
    std::function<void(const QByteArray&)> onWrite;
    QList<Entry> table = QList<Entry>(32);
    bool enabled = false;
    bool answerReads = false;
    bool accept = true;
    int bank = 0;
    qint64 now = 0;
    quint64 nextRequest = 1;
    std::unique_ptr<Backend> backend;
    Fixture() {
        backend = std::make_unique<Backend>(Backend::Transport{[this](const QByteArray& wire) {
            wires.append(wire);
            const bool accepted = accept;
            const auto callback = onWrite;
            if (callback) { callback(wire); }
            if (accepted && answerReads && backend) {
                const QByteArray reply = replyFor(wire);
                if (!reply.isEmpty()) { observe(reply); }
            }
            return accepted;
        }, {}});
        QObject::connect(backend.get(), &IRadioBackend::extensionStatus, backend.get(),
            [this](const QString&, const QString& kind, const QVariantMap& value) {
                if (kind == "autoMode") {
                    state = value;
                    const auto callback = onStatus;
                    if (callback) { callback(value); }
                }
            });
        QObject::connect(backend.get(), &IRadioBackend::extensionResult, backend.get(),
            [this](quint64 id, const QVariant& result) {
                results[id] = result;
                if (result.toMap().contains("rows")) { state = result.toMap(); }
            });
        QObject::connect(backend.get(), &IRadioBackend::extensionError, backend.get(),
            [this](quint64 id, const QString& error) { errors[id] = error; });
    }
    QByteArray replyFor(const QByteArray& wire) const {
        if (wire == "MF;") { return "MF" + QByteArray::number(bank); }
        if (wire == "AM;") { return enabled ? "AM1" : "AM0"; }
        if (wire == "AS1;") {
            int count = 0;
            for (const Entry& entry : table) { if (entry.mode != '0') { ++count; } }
            return "AS1" + QByteArray::number(count).rightJustified(2, '0');
        }
        if (wire.startsWith("AS2") && wire.size() == 6) {
            const int index = wire.mid(3, 2).toInt();
            return division(index, table[index].hz, table[index].mode);
        }
        return {};
    }
    void ready() {
        kenwood::KenwoodBackendTestAccess::authenticate(*backend);
        for (const QByteArray& body : {QByteArray("ID024"), QByteArray("FR0"), QByteArray("FT0"),
                 QByteArray("FA00014074000"), QByteArray("OM02"), QByteArray("SF0000140740002"),
                 QByteArray("TS0"), QByteArray("RX")}) { observe(body); }
        get();
    }
    void observe(const QByteArray& body) { kenwood::KenwoodBackendTestAccess::observe(*backend, body); }
    quint64 call(const QString& verb, const QVariant& arguments = {}) {
        const quint64 id = nextRequest++;
        backend->invokeExtension("kenwood", verb, id, arguments);
        return id;
    }
    void get() { call("auto.mode.get"); }
    QVariantMap arguments() const { return {{"context", state.value("context")}}; }
    QVariantMap row(int index) const { return state.value("rows").toList().at(index).toMap(); }
    QVariantMap snapshot() const { return state.value("snapshot").toMap(); }
    QVariantMap progress() const { return state.value("refresh").toMap(); }
    QVariantMap lastWrite() const { return state.value("lastWrite").toMap(); }
    void tick(int count = 1, qint64 advance = 100) {
        for (int index = 0; index < count && backend; ++index) {
            now += advance;
            kenwood::KenwoodBackendTestAccess::poll(*backend, now);
        }
    }
    void qualify(int count = 3) {
        for (int index = 0; index < 32; ++index) {
            table[index] = index < count ? Entry{1000000LL * (index + 1), '2'} : Entry{};
        }
        answerReads = true;
        call("auto.mode.read", arguments());
        tick(40);
    }
    QVariantMap registration(qint64 hz = 14075000, const QString& mode = "2") const {
        QVariantMap args = arguments();
        args["snapshotToken"] = snapshot().value("token");
        args["upperHz"] = QVariant::fromValue(hz);
        args["mode"] = mode;
        return args;
    }
    QVariantMap deletion(int index) const {
        QVariantMap args = arguments();
        args["snapshotToken"] = snapshot().value("token");
        args["index"] = index;
        args["rowRevision"] = row(index).value("revision");
        return args;
    }
    void cancel() {
        QVariantMap args = arguments(); args["token"] = progress().value("token");
        call("auto.mode.cancel", args);
    }
};
}
class KenwoodAutoModeTest final : public QObject {
    Q_OBJECT
private slots:
    void cachedGetAndChoicesDoNotReadOrInventUnknownValues() {
        Fixture fixture; fixture.get();
        QCOMPARE(fixture.state.value("rows").toList().size(), 32);
        QCOMPARE(fixture.state.value("modeChoices").toList().size(), 14);
        QVERIFY(!fixture.state.value("enabledKnown").toBool());
        QVERIFY(!fixture.state.contains("enabled"));
        QVERIFY(!fixture.state.value("countKnown").toBool());
        QVERIFY(!fixture.state.contains("count"));
        const QVariantMap domain = fixture.state.value("hzDomain").toMap();
        QCOMPARE(domain.value("minimum"), QVariant::fromValue(0LL));
        QCOMPARE(domain.value("maximum"), QVariant::fromValue(99999999999LL));
        QStringList choices;
        for (const QVariant& item : fixture.state.value("modeChoices").toList()) {
            const QVariantMap choice = item.toMap(); choices.append(choice.value("value").toString());
            QVERIFY(!choice.value("label").toString().isEmpty());
        }
        QCOMPARE(choices.join(""), QString("12345679ABCDEF"));
        fixture.ready(); fixture.wires.clear(); fixture.get(); fixture.tick();
        QVERIFY(fixture.wires.isEmpty());
        const QVariantMap extension = fixture.backend->capabilities().extensions.value("kenwood").toMap();
        QCOMPARE(extension.value("nativeAutoModeVersion"), QVariant(1));
        QCOMPARE(extension.value("nativeAutoModeVerbs").toStringList().size(), 6);
    }
    void allNativeIndicesModesCountsAndEmptyLayoutsHaveLiteralObservations() {
        Fixture fixture; fixture.ready(); fixture.observe("MF0");
        for (int count = 0; count <= 32; ++count) {
            fixture.observe("AS1" + QByteArray::number(count).rightJustified(2, '0'));
            QCOMPARE(fixture.state.value("count").toInt(), count);
        }
        for (int index = 0; index < 32; ++index) {
            for (char mode : QByteArray("12345679ABCDEF")) {
                const QByteArray body = division(index, 14175000, mode);
                QCOMPARE(body.size(), 17); fixture.observe(body);
                const QVariantMap observed = fixture.row(index);
                QCOMPARE(observed.value("state").toString(), QString("populated"));
                QCOMPARE(observed.value("mode").toString(), QString(QChar::fromLatin1(mode)));
                QCOMPARE(observed.value("upperHz"), QVariant::fromValue(14175000LL));
                QCOMPARE(observed.value("rawUtf8Hex").toString(), QString::fromLatin1(body.toHex()));
            }
            fixture.observe(division(index));
            QCOMPARE(fixture.row(index).value("state").toString(), QString("empty"));
            QCOMPARE(fixture.row(index).value("mode").toString(), QString("0"));
        }
        for (qint64 hz : {0LL, 14175000LL, 99999999999LL}) {
            fixture.observe(division(0, hz, '2'));
            QCOMPARE(fixture.row(0).value("upperHz"), QVariant::fromValue(hz));
        }
        QVERIFY(!fixture.snapshot().value("complete").toBool());
    }
    void malformedObservationsRetainLastValidFieldsWithoutEmptyFabrication() {
        Fixture fixture; fixture.ready(); fixture.observe("MF0");
        const QByteArray good = division(7, 14175000, '2'); fixture.observe(good);
        QList<QByteArray> invalid{division(7, 14175000, '0'), division(7, 0, '8'), good.left(16), good + '0'};
        for (int offset : {3, 5, 15, 16}) { QByteArray body = good; body[offset] = '?'; invalid.append(body); }
        for (const QByteArray& body : invalid) {
            fixture.observe(body);
            QCOMPARE(fixture.row(7).value("upperHz"), QVariant::fromValue(14175000LL));
            QCOMPARE(fixture.row(7).value("mode").toString(), QString("2"));
            QCOMPARE(fixture.row(7).value("rawUtf8Hex").toString(), QString::fromLatin1(good.toHex()));
            QVERIFY(fixture.row(7).value("state").toString() != "empty");
        }
    }
    void explicitSurveyUsesOnlyThirtyFiveSerializedQueriesAndStableGetToken() {
        Fixture fixture; fixture.ready(); fixture.wires.clear(); fixture.qualify();
        QList<QByteArray> expected{"MF;", "AM;", "AS1;"};
        for (int index = 0; index < 32; ++index) { expected.append("AS2" + QByteArray::number(index).rightJustified(2, '0') + ';'); }
        QCOMPARE(fixture.wires, expected);
        QVERIFY(fixture.snapshot().value("complete").toBool());
        QVERIFY(fixture.snapshot().value("coherent").toBool());
        QCOMPARE(fixture.progress().value("completed").toInt(), 35);
        QCOMPARE(fixture.progress().value("observed").toInt(), 35);
        const QVariant token = fixture.snapshot().value("token");
        fixture.wires.clear(); fixture.get(); fixture.get(); fixture.tick();
        QCOMPARE(fixture.snapshot().value("token"), token); QVERIFY(fixture.wires.isEmpty());
    }
    void settersRefuseWrongTypesUnknownBankIncompleteFullAndDuplicateTables() {
        Fixture fixture; fixture.ready(); fixture.wires.clear();
        QVariantMap enabled = fixture.arguments(); enabled["enabledRevision"] = fixture.state.value("enabledRevision"); enabled["enabled"] = true;
        QVERIFY(fixture.errors.contains(fixture.call("auto.mode.enabled.set", enabled)));
        fixture.qualify(); QVERIFY(fixture.snapshot().value("complete").toBool()); fixture.wires.clear();
        for (const QVariant& value : {QVariant(14175000), QVariant("14175000"), QVariant(14175000.0), QVariant(true), QVariant::fromValue(-1LL), QVariant::fromValue(100000000000LL)}) {
            QVariantMap args = fixture.registration(); args["upperHz"] = value;
            QVERIFY(fixture.errors.contains(fixture.call("auto.mode.division.register", args)));
        }
        for (const QString& mode : {QString("0"), QString("8"), QString("USB"), QString("2;AS300")}) {
            QVERIFY(fixture.errors.contains(fixture.call("auto.mode.division.register", fixture.registration(14175000, mode))));
        }
        QVERIFY(fixture.errors.contains(fixture.call("auto.mode.division.register", fixture.registration(1000000))));
        QVariantMap bad = fixture.deletion(1); bad["index"] = true;
        QVERIFY(fixture.errors.contains(fixture.call("auto.mode.division.delete", bad)));
        QVERIFY(fixture.wires.isEmpty());
        fixture.qualify(32); fixture.wires.clear();
        QVERIFY(fixture.errors.contains(fixture.call("auto.mode.division.register", fixture.registration(50000000))));
        QVERIFY(fixture.wires.isEmpty());
    }
    void amSetBothValuesRetainsObservedValueUntilBoundedReadback() {
        for (bool target : {false, true}) {
            Fixture fixture; fixture.ready(); fixture.qualify(); fixture.wires.clear(); fixture.answerReads = false;
            QVariantMap args = fixture.arguments(); args["enabledRevision"] = fixture.state.value("enabledRevision"); args["enabled"] = target;
            const quint64 id = fixture.call("auto.mode.enabled.set", args);
            QCOMPARE(fixture.wires, QList<QByteArray>({target ? "AM1;" : "AM0;"}));
            QCOMPARE(fixture.state.value("enabled"), QVariant(false));
            QVERIFY(fixture.results.contains(id)); QCOMPARE(fixture.results[id].toMap().value("verified"), QVariant(false));
            fixture.tick(); QCOMPARE(fixture.wires.last(), QByteArray("AM;"));
            fixture.observe(target ? "AM1" : "AM0");
            QCOMPARE(fixture.state.value("enabled"), QVariant(target));
            QCOMPARE(fixture.lastWrite().value("state").toString(), QString("observed_consistent"));
        }
        Fixture fixture; fixture.ready(); fixture.qualify(); fixture.answerReads = false;
        QVariantMap args = fixture.arguments(); args["enabledRevision"] = fixture.state.value("enabledRevision"); args["enabled"] = true;
        fixture.call("auto.mode.enabled.set", args); fixture.tick(); fixture.tick(1, 2001);
        QCOMPARE(fixture.lastWrite().value("state").toString(), QString("indeterminate"));
        QCOMPARE(fixture.state.value("enabled"), QVariant(false));
    }
    void registerEveryModeUsesLiteralWriteAndUnorderedMultisetReadback() {
        for (char mode : QByteArray("12345679ABCDEF")) {
            Fixture fixture; fixture.ready(); fixture.qualify(2); fixture.wires.clear();
            fixture.onWrite = [&](const QByteArray& wire) {
                if (wire.startsWith("AS0")) { fixture.table[2] = fixture.table[0]; fixture.table[0] = Entry{14175000, mode}; }
            };
            const quint64 id = fixture.call("auto.mode.division.register", fixture.registration(14175000, QString(QChar::fromLatin1(mode))));
            QCOMPARE(fixture.wires.first(), "AS0" + QByteArray("00014175000") + mode + ';');
            QCOMPARE(fixture.wires.size(), 1);
            QVERIFY(fixture.results.contains(id)); QCOMPARE(fixture.results[id].toMap().value("verified"), QVariant(false));
            QVERIFY(!fixture.snapshot().value("complete").toBool());
            fixture.tick(40);
            QCOMPARE(fixture.lastWrite().value("state").toString(), QString("observed_consistent"));
            QVERIFY(fixture.snapshot().value("complete").toBool());
        }
    }
    void deleteEveryNativeIndexWaitsForCompleteShiftedReadback() {
        for (int selected = 0; selected < 32; ++selected) {
            Fixture fixture; fixture.ready(); fixture.qualify(32); fixture.wires.clear();
            const qint64 oldHz = fixture.row(selected).value("upperHz").toLongLong();
            fixture.onWrite = [&](const QByteArray& wire) {
                if (wire.startsWith("AS3")) { fixture.table.removeAt(selected); fixture.table.append(Entry{}); }
            };
            fixture.call("auto.mode.division.delete", fixture.deletion(selected));
            QCOMPARE(fixture.wires.first(), "AS3" + QByteArray::number(selected).rightJustified(2, '0') + ';');
            QCOMPARE(fixture.wires.size(), 1);
            QCOMPARE(fixture.row(selected).value("upperHz").toLongLong(), oldHz);
            QVERIFY(!fixture.snapshot().value("complete").toBool());
            fixture.tick(40);
            QCOMPARE(fixture.lastWrite().value("state").toString(), QString("observed_consistent"));
            QCOMPARE(fixture.state.value("count").toInt(), 31);
            QCOMPARE(fixture.row(31).value("state").toString(), QString("empty"));
        }
    }
    void sentCancellationAndUnexpectedEditsStayIndeterminateWithoutCompensation() {
        Fixture fixture; fixture.ready(); fixture.qualify(); fixture.wires.clear();
        fixture.call("auto.mode.division.register", fixture.registration());
        QCOMPARE(fixture.wires.size(), 1); fixture.cancel(); fixture.tick(40);
        QCOMPARE(fixture.wires.size(), 1);
        QCOMPARE(fixture.lastWrite().value("state").toString(), QString("indeterminate"));
        QVERIFY(!fixture.snapshot().value("complete").toBool());
        fixture.qualify(); fixture.wires.clear();
        fixture.onWrite = [&](const QByteArray& wire) { if (wire.startsWith("AS0")) { fixture.table[3] = Entry{14075000, '2'}; fixture.table[0] = Entry{7777777, '3'}; } };
        fixture.call("auto.mode.division.register", fixture.registration()); fixture.tick(40);
        QCOMPARE(fixture.lastWrite().value("state").toString(), QString("indeterminate"));
        int setters = 0;
        for (const QByteArray& wire : fixture.wires) { if (wire.startsWith("AS0") || wire.startsWith("AS3")) { ++setters; } }
        QCOMPARE(setters, 1);
    }
    void earlyNestedTransmitRestoreCannotAuthorizeOldMutation() {
        Fixture fixture; fixture.ready(); fixture.qualify(); fixture.wires.clear();
        bool injected = false;
        QObject::connect(fixture.backend.get(), &IRadioBackend::transmitChanged, fixture.backend.get(), [&](const TransmitDelta& delta) {
            if (delta.mox.value_or(false) && !injected) { injected = true; fixture.observe("RX"); }
        });
        bool armed = true;
        fixture.onStatus = [&](const QVariantMap& status) {
            if (armed && status.value("lastWrite").toMap().value("state") == "pending") { armed = false; fixture.observe("TX0"); }
        };
        const quint64 id = fixture.call("auto.mode.division.register", fixture.registration());
        QVERIFY(injected);
        for (const QByteArray& wire : fixture.wires) { QVERIFY(!wire.startsWith("AS0")); }
        QVERIFY(fixture.results.contains(id) || fixture.errors.contains(id));
    }
    void earlyNestedSourceRestoreCannotAuthorizeOldMutation() {
        Fixture fixture; fixture.ready(); fixture.qualify(); fixture.wires.clear();
        bool injected = false; bool armed = false;
        QObject::connect(fixture.backend.get(), &IRadioBackend::sliceChanged, fixture.backend.get(), [&](int, const SliceDelta&) {
            if (armed && !injected && kenwood::KenwoodBackendTestAccess::source(*fixture.backend) == 1) {
                injected = true; fixture.observe("FR0"); fixture.observe("SF0000140740002");
            }
        });
        fixture.onStatus = [&](const QVariantMap& status) {
            if (!armed && status.value("lastWrite").toMap().value("state") == "pending") { armed = true; fixture.observe("FR1"); }
        };
        const quint64 id = fixture.call("auto.mode.division.register", fixture.registration());
        QVERIFY(injected);
        for (const QByteArray& wire : fixture.wires) { QVERIFY(!wire.startsWith("AS0")); }
        QVERIFY(fixture.results.contains(id) || fixture.errors.contains(id));
    }
    void memoryPublisherCannotExposeOldAutoModeBankAuthority() {
        Fixture fixture; fixture.ready(); fixture.qualify(); fixture.wires.clear();
        QVERIFY(fixture.snapshot().value("complete").toBool());
        const QVariantMap stale = fixture.registration(); quint64 nested = 0;
        QObject::connect(fixture.backend.get(), &IRadioBackend::extensionStatus, fixture.backend.get(),
            [&](const QString&, const QString& kind, const QVariantMap&) {
                if (kind == "memoryRead" && !nested) { nested = fixture.nextRequest; fixture.call("auto.mode.division.register", stale); }
            });
        fixture.observe("MF1"); QVERIFY(nested);
        for (const QByteArray& wire : fixture.wires) { QVERIFY2(!wire.startsWith("AS0"), "New MF authority must be fenced before another native helper publishes callbacks."); }
        QVERIFY(fixture.errors.contains(nested));
        QVERIFY(!fixture.snapshot().value("complete").toBool());
    }
    void failedBankReadCannotLeaveEnabledWriteAuthorityQualified() {
        Fixture fixture; fixture.ready(); fixture.qualify();
        QVERIFY(fixture.state.value("bankKnown").toBool());
        fixture.answerReads = false;
        fixture.call("auto.mode.read", fixture.arguments()); fixture.tick();
        QCOMPARE(fixture.wires.last(), QByteArray("MF;"));
        fixture.tick(1, 2001); fixture.answerReads = true; fixture.tick(40);
        QVERIFY(!fixture.progress().value("busy").toBool());
        QVERIFY(!fixture.state.value("bankKnown").toBool());
        QVERIFY(!fixture.state.value("operations").toMap().value("enabled").toMap().value("editable").toBool());
        fixture.wires.clear(); QVariantMap args = fixture.arguments();
        args["enabledRevision"] = fixture.state.value("enabledRevision"); args["enabled"] = true;
        QVERIFY(fixture.errors.contains(fixture.call("auto.mode.enabled.set", args)));
        QVERIFY(fixture.wires.isEmpty());
    }
    void typedOwnedArgumentsAndNativeHzBoundariesUseRealAdmission() {
        Fixture fixture; fixture.ready(); fixture.qualify(); fixture.wires.clear();
        for (const QVariant& value : {QVariant(1), QVariant("1"), QVariant::fromValue(1LL)}) {
            QVariantMap args = fixture.arguments(); args["enabledRevision"] = fixture.state.value("enabledRevision"); args["enabled"] = value;
            QVERIFY(fixture.errors.contains(fixture.call("auto.mode.enabled.set", args)));
        }
        for (const QVariant& value : {QVariant(-1), QVariant(32), QVariant("1"), QVariant(1.0)}) {
            QVariantMap args = fixture.deletion(1); args["index"] = value;
            QVERIFY(fixture.errors.contains(fixture.call("auto.mode.division.delete", args)));
        }
        for (const QString& value : {QString("01"), QString("18446744073709551616"), QString("stale")}) {
            QVariantMap args = fixture.registration(); args["snapshotToken"] = value;
            QVERIFY(fixture.errors.contains(fixture.call("auto.mode.division.register", args)));
        }
        QVERIFY(fixture.wires.isEmpty());
        for (qint64 hz : {0LL, 99999999999LL}) {
            Fixture boundary; boundary.ready(); boundary.qualify(1); boundary.wires.clear();
            boundary.onWrite = [&](const QByteArray& wire) { if (wire.startsWith("AS0")) { boundary.table[1] = Entry{hz, '2'}; } };
            boundary.call("auto.mode.division.register", boundary.registration(hz));
            QCOMPARE(boundary.wires.first(), "AS0" + QByteArray::number(hz).rightJustified(11, '0') + "2;");
            boundary.tick(40);
            QCOMPARE(boundary.lastWrite().value("state").toString(), QString("observed_consistent"));
        }
    }
    void getterAndUnchangedPredispatchReportsCannotDispatchOrAcknowledgeEarly() {
        Fixture fixture; fixture.ready(); fixture.qualify(); fixture.wires.clear();
        bool nested = false;
        fixture.onStatus = [&](const QVariantMap& status) {
            if (!nested && status.value("lastWrite").toMap().value("state") == "pending") {
                nested = true; fixture.get();
                fixture.observe("AM0"); fixture.observe("AS103"); fixture.observe(division(0, 1000000, '2'));
                QVERIFY(fixture.wires.isEmpty());
            }
        };
        const quint64 id = fixture.call("auto.mode.division.register", fixture.registration());
        QVERIFY(nested); QCOMPARE(fixture.wires, QList<QByteArray>({"AS0000140750002;"}));
        QVERIFY(fixture.results.contains(id)); QCOMPARE(fixture.lastWrite().value("state").toString(), QString("sent_unverified"));
    }
    void countShiftAndResumedOldBankCannotRestoreSlotAuthority() {
        Fixture fixture; fixture.ready(); fixture.qualify(); fixture.wires.clear();
        const QVariantMap stale = fixture.registration();
        kenwood::KenwoodBackendTestAccess::batch(*fixture.backend, {"AS102", division(31, 7777777, '3')});
        QCOMPARE(fixture.row(31).value("state").toString(), QString("empty"));
        QVERIFY(!fixture.snapshot().value("complete").toBool());
        QVERIFY(fixture.errors.contains(fixture.call("auto.mode.division.register", stale)));
        QVERIFY(fixture.wires.isEmpty());

        Fixture bank; bank.ready(); bank.observe("MF0"); bank.observe(division(0, 1000000, '2'));
        bool nested = false;
        bank.onStatus = [&](const QVariantMap& status) {
            if (!nested && status.value("rows").toList().at(1).toMap().value("state") == "populated") {
                nested = true; bank.observe("MF1");
            }
        };
        kenwood::KenwoodBackendTestAccess::batch(*bank.backend,
            {division(1, 2000000, '2'), division(31, 7777777, '3'), "AS102", "MF0"});
        QVERIFY(nested); QCOMPARE(bank.state.value("bank"), QVariant(1));
        QCOMPARE(bank.row(31).value("state").toString(), QString("unread"));
        QVERIFY(!bank.state.value("countKnown").toBool());
    }
    void readReplacementCompletesBothCallersAndNeverPumpsInsidePublication() {
        Fixture fixture; fixture.ready(); fixture.wires.clear(); quint64 replacement = 0; bool nested = false;
        fixture.onStatus = [&](const QVariantMap& status) {
            if (!nested && status.value("refresh").toMap().value("busy").toBool()) {
                nested = true; fixture.cancel(); replacement = fixture.call("auto.mode.read", fixture.arguments());
                QVERIFY(fixture.wires.isEmpty());
            }
        };
        const quint64 original = fixture.call("auto.mode.read", fixture.arguments());
        QVERIFY(nested); QVERIFY(fixture.results.contains(original) || fixture.errors.contains(original));
        QVERIFY(fixture.results.contains(replacement) || fixture.errors.contains(replacement));
        QVERIFY(fixture.progress().value("busy").toBool()); fixture.tick();
        QCOMPARE(fixture.wires, QList<QByteArray>({"MF;"}));
    }
    void refusalAndPostHandoffDisconnectNeverRetryOrRestore() {
        Fixture refused; refused.ready(); refused.qualify(); refused.wires.clear(); refused.accept = false;
        const quint64 failed = refused.call("auto.mode.division.register", refused.registration());
        QVERIFY(refused.errors.contains(failed)); refused.tick(40);
        QCOMPARE(refused.wires.size(), 1); QVERIFY(!refused.snapshot().value("complete").toBool());

        Fixture disconnected; disconnected.ready(); disconnected.qualify(); disconnected.wires.clear();
        const quint64 sent = disconnected.call("auto.mode.division.register", disconnected.registration());
        QVERIFY(disconnected.results.contains(sent)); disconnected.backend->disconnectRadio(); disconnected.tick(40); disconnected.get();
        QCOMPARE(disconnected.lastWrite().value("state").toString(), QString("indeterminate"));
        QVERIFY(!disconnected.snapshot().value("complete").toBool());
        int setters = 0;
        for (const QByteArray& wire : disconnected.wires) { if (wire.startsWith("AS0") || wire.startsWith("AS3")) { ++setters; } }
        QCOMPARE(setters, 1);
    }
};
int main(int argc, char** argv) {
    TestSettingsProfile profile("kenwood-auto-mode"); if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv); KenwoodAutoModeTest test; return QTest::qExec(&test, argc, argv);
}
#include "kenwood_auto_mode_test.moc"
