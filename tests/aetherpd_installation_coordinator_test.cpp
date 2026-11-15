// Coordinator ordering only: injected HTTP replies and completed component
// events. Crypto/stream transport and waveform upload have separate tests.
#include "RadioSetupControlFixture.h"
#include "AetherPDTestTransport.h"
#include "core/AetherPDInstallationCoordinator.h"
#include "core/AetherPDInstallationMonitor.h"
#include "core/AetherPDInstallRequestClient.h"
#include "core/AetherPDProtocol.h"
#include "core/backends/AetherPDCarrierUpload.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <QtTest>

using namespace AetherSDR;
using Coordinator = AetherPDInstallationCoordinator;
namespace {
class Carrier final : public AetherPDCarrierUpload {
public:
    int starts{0};
    void begin(const QString&, const RadioControlSession&, const FileTransferHandle&) override { ++starts; }
    void cancel() override {}
    void complete() { emit finished(true); }
};
class Channel final : public AetherPDInstallChannel {
public:
    bool connected{false};
    void connectTo(const QHostAddress&) override { connected = true; }
    qint64 write(const char*, qint64) override { return -1; }
    qint64 pendingBytes() const override { return 0; }
    qint64 availableBytes() const override { return 0; }
    QByteArray read(qint64) override { return {}; }
    bool finishWrites() override { return false; }
    void close() override {}
};
AetherPDInstallUpload::Inputs inputs()
{
    QFile file(QStringLiteral(AETHERPD_UPLOAD_TESTDATA));
    if (!file.open(QIODevice::ReadOnly)) { return {}; }
    const QJsonArray list = QJsonDocument::fromJson(file.readAll()).object().value("inputs").toArray();
    AetherPDInstallUpload::Inputs result;
    for (int i = 0; i < 4; ++i) { result[i] = QByteArray::fromHex(list[i].toString().toLatin1()); }
    return result;
}
std::optional<Coordinator::Prepared> prepare()
{
    QByteArray elf(64, '\0');
    elf.replace(0, 7, QByteArray::fromHex("7f454c46020101"));
    qToLittleEndian<quint16>(183, elf.data() + 18);
    return Coordinator::prepare(inputs(), {elf, elf, elf, elf}, "fixture license");
}
struct Fixture {
    RadioModel model;
    RadioSetupTestBackend* backend{installRadioSetupBackend(model)};
    AetherPDTest::Network network;
    Channel channel;
    Carrier archive;
    qint64 clock{0};
    Coordinator coordinator{model, network, channel, archive, [this] { return clock; }};
    QStringList transfers;
    Fixture()
    {
        RadioSetupControlTestAccess::peer(model, QHostAddress(QStringLiteral("192.168.50.100")));
        RadioSetupControlTestAccess::identity(model, "fixture-8600", "4.3.16.42034");
        backend->setModelProvider([] { return QStringLiteral("FLEX-8600"); });
        model.handleLineForTest("S0|waveform wfp_status power=on ready=true");
        backend->setTransferCommandSink(RadioSetupControlTestAccess::productionTransferSink(model,
            [this](const QString& command, const FileTransferGuard&, const auto&) { transfers << command; return true; }));
    }
    bool begin(const Coordinator::Prepared& prepared)
    { return coordinator.begin(prepared, model.controlSession(), QString(43, u'a')); }
    void acceptRequest()
    {
        network.last->complete(202, QJsonDocument(QJsonObject{{"state", "awaiting_inputs"},
            {"request_id", coordinator.requestId()}, {"installation_committed", false}, {"port", 48995}}).toJson());
    }
    bool reserved()
    { return !model.beginFileTransfer(FileTransferPurpose::LegacyWaveform, &model, model.controlSession(), 1000); }
    void carrier(bool present)
    {
        model.handleLineForTest("S0|waveform container name=" + coordinator.carrierName()
            + (present ? " version=1" : " removed"));
        coordinator.poll();
    }
    AetherPDTest::Reply* pending(const QString& suffix)
    {
        for (auto* reply : network.findChildren<QNetworkReply*>()) {
            if (!reply->isFinished() && reply->url().path().endsWith(suffix)) { return static_cast<AetherPDTest::Reply*>(reply); }
        }
        return nullptr;
    }
};
}
class CoordinatorTest : public QObject {
    Q_OBJECT
private slots:
    void preparationBindsFreshAttempt()
    {
        const auto first = prepare(), second = prepare();
        QVERIFY(first); QVERIFY(second);
        QVERIFY(first->approvedPlan().startsWith("PDUPGR02"));
        QVERIFY(first->approvedPlan() != second->approvedPlan());
        QVERIFY(first->carrierName() != second->carrierName());
        auto bad = inputs(); bad[2][0] ^= 1;
        QVERIFY(!Coordinator::prepare(bad, {}, "license"));
    }
    void wrongSessionAndIdentityHaveNoEffects()
    {
        Fixture f; const auto package = prepare(); QVERIFY(package);
        auto session = f.model.controlSession(); ++session.connectionGeneration;
        QVERIFY(!f.coordinator.begin(*package, session, QString(43, u'a')));
        RadioSetupControlTestAccess::identity(f.model, "other-radio", "4.3.16.42034");
        QVERIFY(!f.begin(*package));
        QVERIFY(f.network.sent.empty()); QVERIFY(f.transfers.isEmpty());
    }
    void preexistingCarrierAndBusyTransferRefuseWithoutPost()
    {
        Fixture f; const auto package = prepare(); QVERIFY(package);
        f.model.handleLineForTest("S0|waveform container name=" + package->carrierName() + " version=1");
        QVERIFY(!f.begin(*package)); QVERIFY(f.network.sent.empty());
        f.model.handleLineForTest("S0|waveform container name=" + package->carrierName() + " removed");
        const auto other = f.model.beginFileTransfer(FileTransferPurpose::LegacyWaveform, &f.model, f.model.controlSession(), 10000);
        QVERIFY(other); QVERIFY(!f.begin(*package));
        QVERIFY(f.network.sent.empty()); QVERIFY(f.transfers.isEmpty());
        QVERIFY(f.model.fileTransferCurrent(other));
    }
    void commitAndCleanupRetainReservation()
    {
        Fixture f; const auto package = prepare(); QVERIFY(package);
        QSignalSpy done(&f.coordinator, &Coordinator::finished);
        QVERIFY(f.begin(*package)); QVERIFY(f.reserved());
        f.acceptRequest(); QCOMPARE(f.archive.starts, 1);
        f.archive.complete();
        QCOMPARE(f.coordinator.state(), Coordinator::State::WaitingForCarrier);
        QVERIFY(!f.channel.connected);
        f.carrier(true); QVERIFY(f.channel.connected);
        auto* observation = f.pending("/installation"); QVERIFY(observation);
        QCOMPARE(f.coordinator.state(), Coordinator::State::UploadingInputs);
        QVERIFY(done.isEmpty()); QVERIFY(f.reserved());
        const auto plan = aetherpd::parseInstallationPlan(package->approvedPlan()); QVERIFY(plan);
        observation->complete(200, QJsonDocument(QJsonObject{{"state", "committed"}, {"installation_committed", true},
            {"upgrade_plan_blake2b_256", plan->planDigest}, {"selected_release_blake2b_256", plan->candidateDigest},
            {"selected_release_key", plan->candidateKey}, {"profile", 1}, {"firmware", plan->firmware}, {"serial", plan->serial}}).toJson());
        QCOMPARE(f.coordinator.state(), Coordinator::State::Cleaning); QVERIFY(f.reserved());
        f.coordinator.poll();
        QVERIFY(f.transfers.contains("waveform remove_container " + package->carrierName()));
        f.carrier(false);
        QVERIFY(done.isEmpty());
        // A restarted shim no longer has the retired receiver request.
        auto* request = f.pending('/' + f.coordinator.requestId()); QVERIFY(request);
        request->complete(404, "{}"); f.coordinator.poll();
        QCOMPARE(done.size(), 1);
        QCOMPARE(done.first()[0].value<Coordinator::Outcome>(), Coordinator::Outcome::Committed);
        QVERIFY(done.first()[1].toBool());
        QVERIFY(!f.reserved());
    }
    void deliveryAcknowledgmentIsNotCommit()
    {
        Fixture f; const auto package = prepare(); QVERIFY(package);
        QSignalSpy done(&f.coordinator, &Coordinator::finished);
        QVERIFY(f.begin(*package)); f.acceptRequest(); f.archive.complete(); f.carrier(true);
        auto* request = f.pending('/' + f.coordinator.requestId()); QVERIFY(request);
        request->complete(200, QJsonDocument(QJsonObject{{"state", "delivery_finished"},
            {"request_id", f.coordinator.requestId()}, {"installation_committed", false}, {"delivery_result", 0}}).toJson());
        f.coordinator.poll();
        QVERIFY(done.isEmpty()); QVERIFY(f.reserved());
        QVERIFY(f.transfers.isEmpty());
    }
    void lostSessionNeverRemovesThroughReplacement()
    {
        Fixture f; const auto package = prepare(); QVERIFY(package);
        QSignalSpy done(&f.coordinator, &Coordinator::finished);
        QVERIFY(f.begin(*package)); f.acceptRequest(); QCOMPARE(f.archive.starts, 1);
        f.carrier(true);
        RadioSetupControlTestAccess::replaceConnectionGeneration(f.model);
        f.coordinator.cancel(); f.coordinator.poll();
        QCOMPARE(done.size(), 1); QVERIFY(!done.first()[1].toBool());
        QVERIFY(f.transfers.isEmpty());
        QVERIFY(!f.channel.connected);
    }
    void cancelBeforeCarrierWaitsForReceiverRetirement()
    {
        Fixture f; const auto package = prepare(); QVERIFY(package);
        QSignalSpy done(&f.coordinator, &Coordinator::finished);
        QVERIFY(f.begin(*package));
        f.coordinator.cancel();
        f.acceptRequest(); // Late POST reply must not upload the carrier.
        f.coordinator.poll();
        auto* stop = f.pending('/' + f.coordinator.requestId()); QVERIFY(stop);
        stop->complete(202, "{\"state\":\"stop_requested\"}");
        f.clock = 500; f.coordinator.poll();
        auto* retired = f.pending('/' + f.coordinator.requestId()); QVERIFY(retired);
        retired->complete(404, "{}"); f.coordinator.poll();
        QCOMPARE(done.size(), 1); QVERIFY(done.first()[1].toBool());
        QCOMPARE(done.first()[0].value<Coordinator::Outcome>(), Coordinator::Outcome::Canceled);
        QVERIFY(f.transfers.isEmpty()); QVERIFY(f.transfers.isEmpty());
    }
    void missingPublicationNeverClaimsCleanupOrRemovesAnUnseenCarrier()
    {
        Fixture f; const auto package = prepare(); QVERIFY(package);
        QSignalSpy done(&f.coordinator, &Coordinator::finished);
        QVERIFY(f.begin(*package)); f.acceptRequest(); f.archive.complete();
        f.clock = 500000; f.coordinator.poll();
        QCOMPARE(f.coordinator.state(), Coordinator::State::Cleaning);
        f.clock = 560000; f.coordinator.poll();
        QCOMPARE(done.size(), 1); QVERIFY(!done.first()[1].toBool());
        QCOMPARE(done.first()[0].value<Coordinator::Outcome>(), Coordinator::Outcome::Unconfirmed);
        QVERIFY(f.transfers.isEmpty()); QVERIFY(!f.channel.connected);
    }
    void completionMayDestroyCoordinator()
    {
        Fixture f; const auto package = prepare(); QVERIFY(package);
        auto owner = std::make_unique<Coordinator>(f.model, f.network, f.channel, f.archive, [&f] { return f.clock; });
        const QPointer<Coordinator> weak(owner.get());
        connect(owner.get(), &Coordinator::finished, &f.model, [&owner] { owner.reset(); });
        QVERIFY(owner->begin(*package, f.model.controlSession(), QString(43, u'a')));
        const QString id = owner->requestId();
        owner->cancel();
        f.network.last->complete(202, QJsonDocument(QJsonObject{{"state", "awaiting_inputs"},
            {"request_id", id}, {"installation_committed", false}, {"port", 48995}}).toJson());
        owner->poll();
        f.network.last->complete(404, "{}");
        owner->poll();
        QVERIFY(weak.isNull());
        QVERIFY(f.transfers.isEmpty());
    }
};
QTEST_GUILESS_MAIN(CoordinatorTest)
#include "aetherpd_installation_coordinator_test.moc"
