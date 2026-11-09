#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include <QCoreApplication>
#include <QMetaType>
#include <QPointer>
#include <QVariantMap>
#include <QtTest>
#include <functional>
#include <memory>
#include <utility>

namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& b) {
        b.m_clock.start(); b.m_protocol.start("fixture", "fixture", false, 0);
        b.m_protocol.receive("##CN1;", 1); b.m_protocol.receive("##ID1;", 2); b.ready();
    }
    static quint64 feed(KenwoodBackend& b, const QByteArray& frame) { b.adopt(frame); return b.m_controlIngressSequence; }
    static void batch(KenwoodBackend& b, const QList<QByteArray>& frames) {
        const quint64 ingress = ++b.m_controlIngressSequence;
        for (const QByteArray& frame : frames) { b.adopt(frame, ingress); }
    }
    static void stale(KenwoodBackend& b, const QByteArray& frame, quint64 batch) { b.adopt(frame, batch); }
    static void poll(KenwoodBackend& b, qint64 advance = 0) { b.pollReceiverOptions(b.m_clock.elapsed() + advance); }
    static QVariantMap status(const KenwoodBackend& b) { return b.receiverOptionsStatus(); }
    static void intentAba(KenwoodBackend& b) {
        b.m_keyIntent = true; b.refreshReceiverOptionsContext();
        b.m_keyIntent = false; b.refreshReceiverOptionsContext();
    }
};
}
using namespace AetherSDR;
using Access = kenwood::KenwoodBackendTestAccess;
namespace {
struct Fixture {
    QList<QByteArray> writes;
    std::function<void(const QByteArray&)> onWrite;
    bool refuseRecording = false;
    std::function<void(const QVariantMap&)> onStatus;
    std::unique_ptr<kenwood::KenwoodBackend> backend;
    QMap<quint64, QVariant> results;
    QMap<quint64, QString> errors;
    quint64 sequence = 1;
    Fixture() {
        backend = std::make_unique<kenwood::KenwoodBackend>(kenwood::KenwoodBackend::Transport{
            [this](const QByteArray& bytes) { writes.append(bytes); if (onWrite) { onWrite(bytes); } return !(refuseRecording && bytes == "RE;"); }, {}});
        QObject::connect(backend.get(), &IRadioBackend::extensionResult, backend.get(),
            [this](quint64 id, const QVariant& value) { results[id] = value; });
        QObject::connect(backend.get(), &IRadioBackend::extensionError, backend.get(),
            [this](quint64 id, const QString& reason) { errors[id] = reason; });
        QObject::connect(backend.get(), &IRadioBackend::extensionStatus, backend.get(),
            [this](const QString&, const QString& kind, const QVariantMap& state) { if (kind == "receiverOptions" && onStatus) { onStatus(state); } });
        Access::authenticate(*backend); feed("ID024"); writes.clear();
    }
    void feed(const QByteArray& bytes) { Access::feed(*backend, bytes); }
    void poll(qint64 advance = 0) { Access::poll(*backend, advance); }
    QVariantMap status() const { return Access::status(*backend); }
    QVariantMap row(const QString& id) const {
        for (const QVariant& raw : status().value("rows").toList()) { if (raw.toMap().value("id") == id) { return raw.toMap(); } }
        return {};
    }
    quint64 invoke(const QString& verb, const QVariant& argument = {}) {
        const quint64 id = sequence++; backend->invokeExtension("kenwood", "receiver.options." + verb, id, argument); return id;
    }
    quint64 read() { return invoke("read", QVariantMap{{"context", status().value("context")}}); }
    void completeRead() {
        read(); poll(); feed("QS0"); poll(); feed("BK0"); poll(); feed("BY1"); poll(); feed("RE0000"); poll();
    }
    quint64 set(const QString& id, const QVariant& value) {
        return invoke("set", QVariantMap{{"context", status().value("context")}, {"id", id},
            {"revision", row(id).value("revision")}, {"value", value}});
    }
};
}
class KenwoodReceiverOptionsTest final : public QObject {
    Q_OBJECT
private slots:
    void recordingResponseDomainIsReadOnlyAndStrict() {
        Fixture f;
        QVERIFY(!f.status().value("recording").toMap().value("known").toBool());
        for (int state : {0, 1, 3, 4, 5, 6, 7}) {
            for (int progress : {0, 1, 100}) {
                f.feed("RE" + QByteArray::number(state) + QByteArray::number(progress).rightJustified(3, '0'));
                const QVariantMap recording = f.status().value("recording").toMap();
                QCOMPARE(recording.value("known"), QVariant(true));
                QCOMPARE(recording.value("state"), QVariant(state)); QCOMPARE(recording.value("progress"), QVariant(progress));
            }
        }
        for (const QByteArray& bad : {QByteArray("RE2000"), QByteArray("RE8000"), QByteArray("RE9000"), QByteArray("RE-000"),
            QByteArray("RE3101"), QByteArray("RE3999"), QByteArray("RE30a0"), QByteArray("RE3"), QByteArray("RE30000"), QByteArray("RE3000x")}) {
            f.feed(bad); const QVariantMap recording = f.status().value("recording").toMap();
            QCOMPARE(recording.value("known"), QVariant(false)); QCOMPARE(recording.value("state"), QVariant(7));
            QCOMPARE(recording.value("progress"), QVariant(100));
        }
        f.feed("RE3001"); QVERIFY(f.status().value("recording").toMap().value("known").toBool());
        QVERIFY(f.writes.isEmpty());
        const quint64 denied = f.invoke("set", QVariantMap{{"context", f.status().value("context")},
            {"id", "recording"}, {"revision", f.status().value("recording").toMap().value("revision")}, {"value", true}});
        QVERIFY(f.errors.contains(denied)); QVERIFY(f.writes.isEmpty());
    }
    void newerMalformedRecordingFencesOlderCoalescedRemainder() {
        Fixture f; f.feed("RE0000"); bool nested = false;
        f.onStatus = [&](const QVariantMap& status) {
            const QVariantMap recording = status.value("recording").toMap();
            if (!nested && recording.value("known").toBool() && recording.value("state").toInt() == 3) {
                nested = true; f.feed("RE2101");
            }
        };
        Access::batch(*f.backend, {"RE3001", "RE0000"});
        QVERIFY(nested); const QVariantMap recording = f.status().value("recording").toMap();
        QCOMPARE(recording.value("known"), QVariant(false)); QCOMPARE(recording.value("state"), QVariant(3));
        QCOMPARE(recording.value("progress"), QVariant(1));
        f.feed("RE0000"); QVERIFY(f.status().value("recording").toMap().value("known").toBool());
        QVERIFY(f.writes.isEmpty());
    }
    void recordingPredispatchCannotSatisfyOwnedReadAndTimeoutRetainsHistory() {
        Fixture f; f.feed("RE3001"); bool injected = false;
        f.onStatus = [&](const QVariantMap& status) {
            const QVariantMap refresh = status.value("refresh").toMap();
            if (!injected && refresh.value("pendingKey") == "RE") { injected = true; f.feed("RE3050"); }
        };
        f.read(); f.poll(); f.feed("QS0"); f.poll(); f.feed("BK0"); f.poll(); f.feed("BY0"); f.poll();
        QVERIFY(injected); QVERIFY(f.writes.contains("RE;"));
        QCOMPARE(f.status().value("refresh").toMap().value("completed"), QVariant(3));
        f.poll(2101);
        QCOMPARE(f.status().value("refresh").toMap().value("failed"), QVariant(1));
        const QVariantMap recording = f.status().value("recording").toMap();
        QCOMPARE(recording.value("known"), QVariant(false)); QCOMPARE(recording.value("progress"), QVariant(50));
        for (const QByteArray& wire : f.writes) { QVERIFY(!wire.startsWith("RE") || wire == "RE;"); }
    }
    void recordingTransportRefusalAndSessionRetirementDoNotCreateActions() {
        Fixture f; f.feed("RE6000"); f.refuseRecording = true; f.read(); f.poll(); f.feed("QS0"); f.poll(); f.feed("BK0"); f.poll(); f.feed("BY0"); f.poll();
        QCOMPARE(f.status().value("recording").toMap().value("known"), QVariant(false));
        QCOMPARE(f.status().value("refresh").toMap().value("failed"), QVariant(1));
        f.backend->disconnectRadio(); const QVariantMap recording = f.status().value("recording").toMap();
        QVERIFY(!recording.value("known").toBool()); QVERIFY(!recording.contains("state"));
        for (const QByteArray& wire : f.writes) { QVERIFY(!wire.startsWith("RE") || wire == "RE;"); }
    }
    void queryDomainDoesNotRequireVfoModeTfOrBank() {
        Fixture f;
        const quint64 get = f.invoke("get");
        QVERIFY(f.results.value(get).toMap().contains("rows")); QVERIFY(f.writes.isEmpty());
        f.completeRead();
        QCOMPARE(f.writes, (QList<QByteArray>{"QS;", "BK;", "BY;", "RE;"}));
        for (const char* id : {"speakerMute", "signalBlanking", "busy"}) { QVERIFY(f.row(id).value("known").toBool()); }
        QVERIFY(!f.row("speakerMute").value("editable").toBool());
        QVERIFY(f.row("busy").value("readonly").toBool());
        QCOMPARE(f.status().value("refresh").toMap().value("observed").toInt(), 4);
    }
    void bothWritableFieldsUseExactBooleansAndExplicitReadback() {
        for (const auto& [id, prefix] : {std::pair{"speakerMute", "QS"}, std::pair{"signalBlanking", "BK"}}) {
            Fixture f; f.feed("RX"); f.completeRead();
            for (bool value : {true, false}) {
                f.writes.clear(); const bool previous = f.row(id).value("value").toBool();
                const quint64 request = f.set(id, value); f.poll();
                QVERIFY(!f.errors.contains(request));
                QVERIFY(f.writes.contains(QByteArray(prefix) + (value ? "1;" : "0;")));
                QVERIFY(f.writes.contains(QByteArray(prefix) + ";"));
                QCOMPARE(f.row(id).value("value").toBool(), previous);
                QVERIFY(!f.results.value(request).toMap().value("verified").toBool());
                f.feed(QByteArray(prefix) + (value ? "1" : "0")); f.poll();
                QCOMPARE(f.row(id).value("value").toBool(), value);
                QVERIFY(!f.row(id).value("pending").toBool());
            }
            f.writes.clear();
            for (const QVariant& value : {QVariant(0), QVariant(1), QVariant(1.0), QVariant("1")}) {
                QVERIFY(f.errors.contains(f.set(id, value)));
            }
            QVERIFY(f.errors.contains(f.set("busy", true))); QVERIFY(f.writes.isEmpty());
        }
    }
    void malformedAndOlderFactsCannotReplaceNewerNativeSample() {
        Fixture f; f.feed("RX"); f.completeRead();
        const quint64 old = Access::feed(*f.backend, "QS0"); f.feed("QS1");
        for (const QByteArray& bytes : {QByteArray("QS2"), QByteArray("QS01"), QByteArray("QSx"), QByteArray("BK9"), QByteArray("BY-1")}) { f.feed(bytes); }
        Access::stale(*f.backend, "QS0", old);
        QVERIFY(f.row("speakerMute").value("value").toBool());
        QVERIFY(!f.row("signalBlanking").value("value").toBool());
        QVERIFY(f.row("busy").value("value").toBool());
    }
    void globalSamplesSurviveUnrelatedReceiveContextChanges() {
        Fixture f; f.feed("RX"); f.completeRead();
        const QString revision = f.row("speakerMute").value("revision").toString();
        const QString context = f.status().value("context").toString();
        for (const QByteArray& bytes : {QByteArray("FR1"), QByteArray("FA00014074000"), QByteArray("OM04"), QByteArray("TS1"), QByteArray("MF1")}) { f.feed(bytes); }
        QCOMPARE(f.status().value("context").toString(), context);
        QCOMPARE(f.row("speakerMute").value("revision").toString(), revision);
        QVERIFY(f.row("speakerMute").value("known").toBool());
    }
    void busyExpiryIsPresentationOnlyWithoutAutomaticCommands() {
        Fixture f; f.completeRead(); f.writes.clear(); f.poll(1601);
        QVERIFY(!f.row("busy").value("known").toBool());
        QVERIFY(f.row("busy").value("value").toBool());
        QVERIFY(f.row("speakerMute").value("known").toBool()); QVERIFY(f.writes.isEmpty());
    }
    void timeoutAndCancellationDoNotInferSuccessOrSendInverse() {
        Fixture f; f.feed("RX"); f.completeRead(); f.writes.clear();
        const quint64 request = f.set("speakerMute", true); f.poll();
        const QString token = f.results.value(request).toMap().value("token").toString();
        QVERIFY(!token.isEmpty()); f.writes.clear();
        const quint64 cancel = f.invoke("cancel", QVariantMap{{"context", f.status().value("context")},
            {"token", f.status().value("refresh").toMap().value("token")}});
        QVERIFY(f.results.value(cancel).toMap().value("cancelled").toBool());
        QVERIFY(f.writes.isEmpty()); QVERIFY(!f.row("speakerMute").value("value").toBool());
        QVERIFY(f.status().value("lastWrite").toMap().value("state") != "observed_consistent");
        f.read(); f.poll(); f.poll(2101);
        QVERIFY(f.status().value("refresh").toMap().value("failed").toInt() > 0);
        QVERIFY(!f.writes.contains("QS0;") && !f.writes.contains("BK0;"));
    }
    void localIntentAbaBeforeHandoffRejectsOldDraft() {
        Fixture f; f.feed("RX"); f.completeRead(); bool nested = false;
        f.onStatus = [&](const QVariantMap& state) {
            if (!nested && state.value("lastWrite").toMap().value("state") == "pending") {
                nested = true; Access::intentAba(*f.backend);
            }
        };
        f.writes.clear(); f.set("speakerMute", true); f.poll();
        QVERIFY(nested); QVERIFY(!f.writes.contains("QS1;"));
    }
    void unchangedPredispatchFactStillFencesOlderCopiedObservation() {
        Fixture f; f.feed("RX"); f.completeRead(); bool nested = false;
        f.onStatus = [&](const QVariantMap& state) {
            if (!nested && state.value("lastWrite").toMap().value("state") == "pending") {
                nested = true;
                const quint64 oldBatch = Access::feed(*f.backend, "BK0");
                Access::feed(*f.backend, "QS0");
                Access::stale(*f.backend, "QS1", oldBatch);
            }
        };
        f.writes.clear(); f.set("speakerMute", true);
        QVERIFY(nested);
        QVERIFY(!f.row("speakerMute").value("value").toBool());
        QVERIFY(f.writes.contains("QS1;"));
    }
    void reconnectCannotBorrowPreviousSessionFreshnessClock() {
        Fixture f; f.completeRead(); f.poll(40000);
        f.backend->disconnectRadio();
        Access::authenticate(*f.backend); f.feed("ID024");
        f.feed("BY1");
        QVERIFY(f.row("busy").value("known").toBool());
        f.poll(1601);
        QVERIFY(!f.row("busy").value("known").toBool());
        QVERIFY(f.row("busy").value("value").toBool());
    }
    void destructionDuringReadPublicationCannotSendQuery() {
        Fixture f; QPointer<kenwood::KenwoodBackend> guard(f.backend.get());
        f.onStatus = [&](const QVariantMap& state) {
            if (state.value("refresh").toMap().value("busy").toBool()) { f.backend.reset(); }
        };
        f.writes.clear(); f.read();
        QVERIFY(guard.isNull()); QVERIFY(f.writes.isEmpty());
    }
};
int main(int argc, char** argv) {
    TestSettingsProfile profile("kenwood-receiver-options"); if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv); AppSettings::instance().load();
    KenwoodReceiverOptionsTest test; return QTest::qExec(&test, argc, argv);
}
#include "kenwood_receiver_options_test.moc"
