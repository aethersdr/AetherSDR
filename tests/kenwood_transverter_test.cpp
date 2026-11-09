// Independent literal XV/XO fixtures: PC reference printed 69.
#include "TestSettingsProfile.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "core/backends/SliceDelta.h"
#include <QByteArray>
#include <QCoreApplication>
#include <QList>
#include <QMap>
#include <QMetaType>
#include <QPair>
#include <QVariantMap>
#include <QtTest>
#include <functional>
#include <memory>

namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& backend) {
        backend.m_clock.start(); backend.m_protocol.start("transverter-fixture", "fixture-only", false, 0);
        backend.m_protocol.receive("##CN1;", 1); backend.m_protocol.receive("##ID1;", 2); backend.ready();
    }
    static void observe(KenwoodBackend& backend, const QByteArray& body) { backend.adopt(body); }
    static quint64 ingress(const KenwoodBackend& backend) { return backend.m_controlIngressSequence; }
    static void observeAt(KenwoodBackend& backend, const QByteArray& body, quint64 batch) { backend.adopt(body, batch); }
    static void poll(KenwoodBackend& backend, qint64 now) { backend.pollTransverter(now); }
    static void batch(KenwoodBackend& backend, const QList<QByteArray>& bodies) {
        const quint64 batch = ++backend.m_controlIngressSequence;
        for (const QByteArray& body : bodies) { backend.adopt(body, batch); }
    }
};
}
namespace {
using namespace AetherSDR;
using Backend = kenwood::KenwoodBackend;
QByteArray offset(int direction, qint64 magnitude) { return "XO" + QByteArray::number(direction) + QByteArray::number(magnitude).rightJustified(11, '0'); }
QVariant hz(qint64 value) { return QVariant::fromValue(value); }
struct Fixture {
    QList<QByteArray> wires;
    QVariantMap state;
    QMap<quint64, QVariant> results;
    QMap<quint64, QString> errors;
    std::function<void(const QVariantMap&)> onStatus;
    std::function<void(const QByteArray&)> onWire;
    std::unique_ptr<Backend> backend;
    bool enabled = false, answerReads = false, accept = true, memory = false;
    int direction = 0, basic = 0, source = 0;
    qint64 magnitude = 402000000, fa = 28000000, fb = 14000000, now = 0;
    quint64 nextRequest = 1;
    Fixture() {
        backend = std::make_unique<Backend>(Backend::Transport{[this](const QByteArray& wire) {
            wires.append(wire); const bool accepted = accept; const auto callback = onWire;
            if (callback) { callback(wire); }
            if (backend && accepted && answerReads) { const QByteArray body = reply(wire); if (!body.isEmpty()) { observe(body); } }
            return accepted;
        }, {}});
        QObject::connect(backend.get(), &IRadioBackend::extensionStatus, backend.get(), [this](const QString&, const QString& kind, const QVariantMap& value) {
            if (kind == "transverter") { state = value; const auto callback = onStatus; if (callback) { callback(value); } }
        });
        QObject::connect(backend.get(), &IRadioBackend::extensionResult, backend.get(), [this](quint64 id, const QVariant& value) {
            results[id] = value; if (value.toMap().contains("enabledKnown")) { state = value.toMap(); }
        });
        QObject::connect(backend.get(), &IRadioBackend::extensionError, backend.get(), [this](quint64 id, const QString& value) { errors[id] = value; });
    }
    QByteArray reply(const QByteArray& wire) const {
        if (wire == "XV;") { return enabled ? "XV1" : "XV0"; }
        if (wire == "XO;") { return offset(direction, magnitude); }
        if (wire == "DS0;") { return "DS0" + QByteArray::number(basic); }
        if (wire == "FR;") { return "FR" + QByteArray::number(source); }
        if (wire == "FT;") { return "FT0"; }
        if (wire == "FA;") { return "FA" + QByteArray::number(fa).rightJustified(11, '0'); }
        if (wire == "FB;") { return "FB" + QByteArray::number(fb).rightJustified(11, '0'); }
        if (wire == "SF0;") { return "SF0" + QByteArray::number(fa).rightJustified(11, '0') + "3"; }
        if (wire == "SF1;") { return "SF1" + QByteArray::number(fb).rightJustified(11, '0') + "3"; }
        if (wire == "IF;") { return "IF00028000000      000000000030000000"; }
        if (wire == "MN;") { return "MN007"; }
        if (wire == "MV;") { return memory ? "MV1" : "MV0"; }
        if (wire == "QR;") { return "QR00"; }
        if (wire == "MA70;") { return "MA7000014074000"; }
        if (wire == "MA71;") { return "MA7100014075000"; }
        return {};
    }
    void ready(bool basis = true, bool qualifyCoordinates = true) {
        kenwood::KenwoodBackendTestAccess::authenticate(*backend); observe("ID024"); observe("RX");
        if (qualifyCoordinates) { observe("XV0"); }
        if (basis) { observe("FR0"); observe("SF0000280000003"); observe("FA00028000000"); }
        get(); wires.clear();
    }
    void observe(const QByteArray& body) { kenwood::KenwoodBackendTestAccess::observe(*backend, body); }
    quint64 call(const QString& verb, const QVariant& args = {}) { const quint64 id = nextRequest++; backend->invokeExtension("kenwood", verb, id, args); return id; }
    void get() { call("transverter.get"); }
    QVariantMap arguments() const { return {{"context", state.value("context")}}; }
    QVariantMap enabledArguments(bool value) const { QVariantMap args = arguments(); args["enabledRevision"] = state.value("enabledRevision"); args["value"] = value; return args; }
    QVariantMap offsetArguments(int sign, qint64 value, bool preview = false) const {
        QVariantMap args = arguments(); args["frequencyRevision"] = state.value("frequency").toMap().value("revision");
        if (!preview) { args["offsetRevision"] = state.value("offsetRevision"); }
        args["direction"] = sign; args["magnitudeHz"] = hz(value); return args;
    }
    QVariantMap preview(int sign, qint64 value) { return results.value(call("transverter.offset.preview", offsetArguments(sign, value, true))).toMap(); }
    QVariantMap progress() const { return state.value("refresh").toMap(); }
    QVariantMap lastWrite() const { return state.value("lastWrite").toMap(); }
    void tick(int count = 1, qint64 advance = 100) { for (int i = 0; i < count && backend; ++i) { now += advance; kenwood::KenwoodBackendTestAccess::poll(*backend, now); } }
    void read(const QString& scope = "controls") { answerReads = true; QVariantMap args = arguments(); args["scope"] = scope; call("transverter.read", args); tick(22); }
    void cancel() { QVariantMap args = arguments(); args["token"] = progress().value("token"); call("transverter.cancel", args); }
};
}
class KenwoodTransverterTest final : public QObject {
    Q_OBJECT
private slots:
    void cachedGetterDoesNotFabricateOffsetOrWriteAnything() {
        Fixture f; f.get(); QVERIFY(f.wires.isEmpty()); QCOMPARE(f.state.value("enabledKnown"), QVariant(false)); QCOMPARE(f.state.value("offsetKnown"), QVariant(false));
        QVERIFY(!f.state.contains("enabled")); QVERIFY(!f.state.contains("direction")); QVERIFY(!f.state.contains("magnitudeHz"));
        QCOMPARE(f.state.value("magnitudeDomain").toMap().value("maximum"), hz(99999999999LL));
        QCOMPARE(f.state.value("displayDomain").toMap().value("maximum"), hz(4294967295LL));
    }
    void controlsReadIsThreeQueriesWithoutChangingNativeState() {
        Fixture f; f.ready(false); f.read(); QCOMPARE(f.wires, QList<QByteArray>({"XV;", "XO;", "DS0;"}));
        QCOMPARE(f.progress().value("observed"), QVariant(3)); QVERIFY(f.state.value("enabledKnown").toBool()); QVERIFY(f.state.value("offsetKnown").toBool());
    }
    void rawElevenDigitDomainAndSignedZeroRemainNativeObservations() {
        Fixture f; f.ready();
        for (int sign : {0, 1}) {
            for (qint64 value : {0LL, 1LL, 28000000LL, 402000000LL, 4294967295LL, 99999999999LL}) {
                f.observe(offset(sign, value)); f.get(); QCOMPARE(f.state.value("direction"), QVariant(sign)); QCOMPARE(f.state.value("magnitudeHz"), hz(value));
                QCOMPARE(f.state.value("signedOffsetHz"), hz(sign ? -value : value));
            }
        }
        QVERIFY(f.wires.isEmpty());
    }
    void previewHasExactDraftEchoAndTwoIdentifiedCandidates() {
        Fixture f; f.ready(); f.read(); f.observe("FA00028000000"); f.get(); f.wires.clear();
        const QVariantMap result = f.preview(0, 402000000); QCOMPARE(result.value("direction"), QVariant(0)); QCOMPARE(result.value("magnitudeHz"), hz(402000000));
        QCOMPARE(result.value("frequencyRevision"), f.state.value("frequency").toMap().value("revision"));
        const QVariantMap bounds = result.value("activeBounds").toMap(); QCOMPARE(bounds.value("known"), QVariant(true)); QCOMPARE(bounds.value("baseHz"), hz(28000000)); QCOMPARE(bounds.value("proposedDisplayHz"), hz(430000000));
        const QVariantList vfos = result.value("vfos").toList(); QCOMPARE(vfos.size(), 2); QCOMPARE(vfos[0].toMap().value("id"), QVariant("A")); QCOMPARE(vfos[1].toMap().value("id"), QVariant("B"));
        QCOMPARE(vfos[0].toMap().value("candidate").toMap().value("known"), QVariant(false)); QVERIFY(f.wires.isEmpty());
    }
    void checkedDisplayBoundsUseActiveFreshBasisAndNotReceiverCaps() {
        Fixture f; f.ready(); f.read(); f.observe("FA00028000000"); f.get(); const qint64 physicalMaximum = f.backend->capabilities().tuningMaxHz;
        for (const auto& choice : QList<QPair<int,qint64>>{{0,4266967295LL},{1,28000000LL}}) {
            QCOMPARE(f.preview(choice.first, choice.second).value("activeBounds").toMap().value("state"), QVariant("valid"));
        }
        for (const auto& choice : QList<QPair<int,qint64>>{{0,4266967296LL},{1,28000001LL}}) {
            QCOMPARE(f.preview(choice.first, choice.second).value("activeBounds").toMap().value("state"), QVariant("invalid"));
            f.wires.clear(); QVERIFY(f.errors.contains(f.call("transverter.offset.set", f.offsetArguments(choice.first, choice.second)))); QVERIFY(f.wires.isEmpty());
        }
        QCOMPARE(f.backend->capabilities().tuningMaxHz, physicalMaximum); QVERIFY(physicalMaximum < 4294967295LL);
    }
    void nativeWritesAreExplicitlyDeferredEvenWithFreshReadback() {
        Fixture f; f.ready(); f.read(); f.wires.clear();
        for (bool value : {false, true}) {
            const quint64 request = f.call("transverter.enabled.set", f.enabledArguments(value));
            QVERIFY(f.errors.contains(request));
            QVERIFY(!f.errors.value(request).isEmpty());
        }
        for (int sign : {0, 1}) {
            const quint64 request = f.call("transverter.offset.set", f.offsetArguments(sign, 402000000));
            QVERIFY(f.errors.contains(request));
            QVERIFY(!f.errors.value(request).isEmpty());
        }
        QVERIFY(f.wires.isEmpty());
        const QVariantMap operations = f.state.value("operations").toMap();
        for (const QString& key : {QStringLiteral("enabled"), QStringLiteral("offset")}) {
            const QVariantMap operation = operations.value(key).toMap();
            QVERIFY(!operation.value("editable").toBool());
            QVERIFY(!operation.value("reason").toString().isEmpty());
        }
    }
    void unknownTransverterDoesNotPublishOrdinaryCoordinatesOrTune() {
        Fixture f; f.ready(false, false);
        int frequencies = 0;
        int geometries = 0;
        int frames = 0;
        QObject::connect(f.backend.get(), &IRadioBackend::sliceChanged, f.backend.get(),
            [&](int, const SliceDelta& delta) { if (delta.frequency) { ++frequencies; } });
        QObject::connect(f.backend.get(), &IRadioBackend::panCenterBandwidthChanged, f.backend.get(), [&] { ++geometries; });
        QObject::connect(f.backend.get(), &IRadioBackend::spectrumFrameReady, f.backend.get(), [&] { ++frames; });
        f.observe("FR0"); f.observe("FA00028000000"); f.observe("SF0000280000003");
        f.observe("BS30"); f.observe("BSK4"); f.observe("BS43");
        f.observe("##DD2" + QByteArray(1280, '0'));
        QCOMPARE(frequencies, 0); QCOMPARE(geometries, 0); QCOMPARE(frames, 0);
        QCOMPARE(f.backend->capabilities().sliceFrequencyControl.authority, SliceFrequencyControl::Authority::Unknown);
        f.wires.clear();
        QVERIFY(!f.backend->requestReceiveTune(0, 28001000, IRadioBackend::ReceiveTuneView::Preserve));
        f.backend->setSliceFrequency(0, 28001000);
        const quint64 request = f.call("vfo.frequency", QVariantMap{{"vfo", "A"}, {"frequencyHz", 28001000}});
        QVERIFY(f.errors.contains(request));
        QVERIFY(f.wires.isEmpty());
    }
    void staleTransverterOnCannotRetireANewerOffSession() {
        Fixture f; f.ready();
        const quint64 older = kenwood::KenwoodBackendTestAccess::ingress(*f.backend);
        f.observe("XV0");
        QString error;
        QObject::connect(f.backend.get(), &IRadioBackend::connectionError, f.backend.get(), [&](const QString& value) { error = value; });
        kenwood::KenwoodBackendTestAccess::observeAt(*f.backend, "XV1", older);
        QVERIFY(f.backend->isConnected());
        QVERIFY(error.isEmpty());
        QCOMPARE(f.backend->capabilities().sliceFrequencyControl.authority, SliceFrequencyControl::Authority::Radio);
    }
    void malformedPreviewDraftsCannotCoerceRawArithmetic() {
        Fixture f; f.ready(); f.read(); f.wires.clear();
        QVariantMap args = f.offsetArguments(0, 402000000, true);
        args["direction"] = true;
        QVERIFY(f.errors.contains(f.call("transverter.offset.preview", args)));
        args = f.offsetArguments(0, 402000000, true); args["magnitudeHz"] = 402000000;
        QVERIFY(f.errors.contains(f.call("transverter.offset.preview", args)));
        args = f.offsetArguments(0, 402000000, true); args["frequencyRevision"] = "01";
        QVERIFY(f.errors.contains(f.call("transverter.offset.preview", args)));
        args = f.offsetArguments(0, 402000000, true); args["extra"] = "TX";
        QVERIFY(f.errors.contains(f.call("transverter.offset.preview", args)));
        QVERIFY(f.wires.isEmpty());
    }
    void observedTransverterOnRefusesSessionWithoutChangingRadio() {
        Fixture f; f.ready();
        QString error;
        int frequencies = 0;
        int geometries = 0;
        int frames = 0;
        QObject::connect(f.backend.get(), &IRadioBackend::connectionError, f.backend.get(), [&](const QString& value) { error = value; });
        QObject::connect(f.backend.get(), &IRadioBackend::sliceChanged, f.backend.get(),
            [&](int, const SliceDelta& delta) { if (delta.frequency) { ++frequencies; } });
        QObject::connect(f.backend.get(), &IRadioBackend::panCenterBandwidthChanged, f.backend.get(), [&] { ++geometries; });
        QObject::connect(f.backend.get(), &IRadioBackend::spectrumFrameReady, f.backend.get(), [&] { ++frames; });
        f.wires.clear(); f.observe("XV1");
        QVERIFY(!f.backend->isConnected());
        QVERIFY(!error.isEmpty());
        QVERIFY(error.contains("transverter", Qt::CaseInsensitive));
        f.observe("FA00430000000"); f.observe("SF0004300000003");
        f.observe("BS30"); f.observe("BSK4"); f.observe("BS43");
        f.observe("##DD2" + QByteArray(1280, '0'));
        QVERIFY(!f.backend->requestReceiveTune(0, 28001000, IRadioBackend::ReceiveTuneView::Preserve));
        f.backend->setSliceFrequency(0, 28001000);
        QCOMPARE(frequencies, 0); QCOMPARE(geometries, 0); QCOMPARE(frames, 0);
        QVERIFY(f.wires.isEmpty());
    }
    void cancelledOrRefusedReadNeverRestoresOrWritesConfiguration() {
        Fixture cancelled; cancelled.ready(); cancelled.read(); cancelled.wires.clear(); cancelled.answerReads = false;
        QVariantMap args = cancelled.arguments(); args["scope"] = "controls";
        cancelled.call("transverter.read", args); cancelled.tick(); cancelled.cancel(); cancelled.tick(8, 2500); cancelled.get();
        QVERIFY(!cancelled.progress().value("busy").toBool());
        for (const QByteArray& wire : cancelled.wires) { QVERIFY(wire == "XV;" || wire == "XO;" || wire == "DS0;"); }
        Fixture refused; refused.ready(); refused.wires.clear(); refused.accept = false;
        args = refused.arguments(); args["scope"] = "controls";
        refused.call("transverter.read", args); refused.tick(8, 2500); refused.get();
        QVERIFY(refused.progress().value("failed").toInt() > 0);
        for (const QByteArray& wire : refused.wires) { QVERIFY(wire == "XV;" || wire == "XO;" || wire == "DS0;"); }
    }
    void deferredSettersNeverCoerceOrDispatchAnyArgument() {
        Fixture f; f.ready(); f.read(); f.wires.clear();
        for (const QVariant& value : {QVariant(1), QVariant("true"), QVariant(1.0)}) { QVariantMap args = f.enabledArguments(true); args["value"] = value; QVERIFY(f.errors.contains(f.call("transverter.enabled.set", args))); }
        for (const QVariant& value : {QVariant(1), QVariant(true), QVariant("402000000"), QVariant(402000000.0), hz(-1), hz(100000000000LL)}) {
            QVariantMap args = f.offsetArguments(0, 402000000); args["magnitudeHz"] = value; QVERIFY(f.errors.contains(f.call("transverter.offset.set", args)));
        }
        QVariantMap args = f.offsetArguments(0, 402000000); args["direction"] = true; QVERIFY(f.errors.contains(f.call("transverter.offset.set", args)));
        args = f.offsetArguments(0, 402000000); args["frequencyRevision"] = "01"; QVERIFY(f.errors.contains(f.call("transverter.offset.set", args)));
        args = f.offsetArguments(0, 402000000); args["extra"] = "TX"; QVERIFY(f.errors.contains(f.call("transverter.offset.set", args))); QVERIFY(f.wires.isEmpty());
    }
    void allReadSkipsTemporaryMemoryWithoutFreshNormalView() {
        Fixture f; f.ready(); f.read("all"); QCOMPARE(f.progress().value("total"), QVariant(15));
        QCOMPARE(f.progress().value("completed"), QVariant(15)); QCOMPARE(f.progress().value("observed"), QVariant(13)); QCOMPARE(f.progress().value("skipped"), QVariant(2));
        QVERIFY(!f.wires.contains("MA70;")); QVERIFY(!f.wires.contains("MA71;"));
    }
    void memoryCompanionsRequireThisRefreshOwnedMetadata() {
        Fixture f; f.ready(); f.observe("MN007"); f.observe("MV1"); f.observe("QR00"); f.memory = true;
        f.answerReads = true; f.onWire = [&](const QByteArray& wire) { if (wire == "MV;") { f.observe("?"); } };
        QVariantMap args = f.arguments(); args["scope"] = "companions"; f.call("transverter.read", args); f.tick(20); f.get();
        QVERIFY(!f.wires.contains("MA70;")); QVERIFY(!f.wires.contains("MA71;")); QVERIFY(f.progress().value("failed").toInt() >= 1);
    }
    void temporaryReadChecksViewAgainAfterPendingPublication() {
        Fixture f; f.ready(); f.memory = true; f.answerReads = true; bool once = false;
        f.onStatus = [&](const QVariantMap& status) {
            if (!once && status.value("refresh").toMap().value("pendingKey") == "MA70") { once = true; f.observe("MV0"); f.observe("MV1"); }
        };
        QVariantMap args = f.arguments(); args["scope"] = "companions"; f.call("transverter.read", args); f.tick(20); f.get();
        QVERIFY(once); QVERIFY(!f.wires.contains("MA70;")); QVERIFY(!f.wires.contains("MA71;")); QVERIFY(f.progress().value("skipped").toInt() >= 2);
    }
    void staleBasisAndSameFrequencyMemorySwitchCannotReuseOldA() {
        Fixture f; f.ready(); f.read(); f.observe("FA00028000000"); f.get(); const QVariant oldToken = f.state.value("frequency").toMap().value("revision");
        f.observe("SF1000280000003"); f.observe("FB00028000000"); f.observe("FR1"); f.get();
        QVERIFY(f.state.value("frequency").toMap().value("revision") != oldToken);
        f.observe("FR3"); f.get(); const QVariantMap bounds = f.preview(0, 402000000).value("activeBounds").toMap();
        QCOMPARE(bounds.value("state"), QVariant("unknown")); QVERIFY(!bounds.contains("baseHz"));
    }
    void malformedOwnedReadWithdrawsOffsetQualificationButKeepsObservedBytes() {
        Fixture f; f.ready(); f.read(); const QVariant prior = f.state.value("magnitudeHz"); f.answerReads = false;
        QVariantMap args = f.arguments(); args["scope"] = "controls"; f.call("transverter.read", args); f.tick(); f.observe("XV0"); f.tick();
        QCOMPARE(f.progress().value("pendingKey"), QVariant("XO")); f.observe("XO200402000000"); f.get();
        QCOMPARE(f.state.value("offsetKnown"), QVariant(false)); QCOMPARE(f.state.value("magnitudeHz"), prior); QVERIFY(f.progress().value("failed").toInt() >= 1);
    }
    void orderedSameBatchOffsetReportsKeepTheLatestNativeValue() {
        Fixture f; f.ready(); kenwood::KenwoodBackendTestAccess::batch(*f.backend,{offset(0,10),offset(1,20)}); f.get();
        QCOMPARE(f.state.value("direction"), QVariant(1)); QCOMPARE(f.state.value("magnitudeHz"),hz(20));
    }
    void newerNestedOffsetCannotBeOverwrittenByOlderBatchRemainder() {
        Fixture f; f.ready(); bool once = false;
        f.onStatus = [&](const QVariantMap& status) {
            if (!once && status.value("magnitudeHz")==hz(10)) { once=true; f.observe(offset(0,30)); }
        };
        kenwood::KenwoodBackendTestAccess::batch(*f.backend,{offset(0,10),offset(1,20)}); f.get();
        QVERIFY(once); QCOMPARE(f.state.value("direction"),QVariant(0)); QCOMPARE(f.state.value("magnitudeHz"),hz(30));
    }
    void dispatchedTemporaryReplyCannotQualifyAfterMemoryViewRoundTrip() {
        Fixture f; f.ready(); f.memory=true; f.answerReads=true; bool once=false;
        f.onWire = [&](const QByteArray& wire) {
            if (!once && wire=="MA70;") { once=true; f.answerReads=false; f.observe("MV0"); f.observe("MV1"); f.observe("MA7000014074000"); }
        };
        QVariantMap args=f.arguments(); args["scope"]="companions"; f.call("transverter.read",args); f.tick(20); f.get();
        QVERIFY(once); QCOMPARE(f.wires.count("MA70;"),1); QVERIFY(!f.wires.contains("MA71;"));
        QCOMPARE(f.state.value("frequency").toMap().value("memory").toMap().value("left").toMap().value("known"),QVariant(false));
        QCOMPARE(f.progress().value("observed"),QVariant(10)); QCOMPARE(f.progress().value("skipped"),QVariant(2));
    }
    void freshOwnedNormalMemoryReadStillQualifiesBothTemporarySamples() {
        Fixture f; f.ready(); f.memory=true; f.read("companions");
        QCOMPARE(f.progress().value("observed"),QVariant(12)); QCOMPARE(f.progress().value("skipped"),QVariant(0));
        QCOMPARE(f.progress().value("failed"),QVariant(0)); QCOMPARE(f.wires.count("MA70;"),1); QCOMPARE(f.wires.count("MA71;"),1);
        const QVariantMap memory=f.state.value("frequency").toMap().value("memory").toMap();
        QCOMPARE(memory.value("left").toMap().value("known"),QVariant(true)); QCOMPARE(memory.value("right").toMap().value("known"),QVariant(true));
        QCOMPARE(memory.value("left").toMap().value("rawHz"),hz(14074000)); QCOMPARE(memory.value("right").toMap().value("rawHz"),hz(14075000));
        QCOMPARE(memory.value("basisKnown"),QVariant(false));
    }
    void unchangedCompanionPollsDoNotRetireFrequencyAuthority() {
        Fixture f; f.ready(); f.read("all"); const QVariant revision=f.state.value("frequency").toMap().value("revision");
        f.read("all"); QCOMPARE(f.state.value("frequency").toMap().value("revision"),revision);
    }
};
int main(int argc, char** argv) {
    TestSettingsProfile profile("kenwood-transverter"); if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc,argv); KenwoodTransverterTest test; return QTest::qExec(&test,argc,argv);
}
#include "kenwood_transverter_test.moc"
