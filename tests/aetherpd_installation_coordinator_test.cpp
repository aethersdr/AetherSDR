// Coordinator ordering only: injected HTTP replies and completed component
// events. Crypto/stream transport and waveform upload have separate tests.
#include "RadioSetupControlFixture.h"
#include "AetherPDTestTransport.h"
#include "AetherPDPackageFixture.h"
#include "core/AetherPDInstallationCoordinator.h"
#include "core/AetherPDInstallationMonitor.h"
#include "core/AetherPDInstallRequestClient.h"
#include "core/AetherPDProtocol.h"
#include "core/backends/AetherPDCarrierUpload.h"
#include "models/AetherPDInstallationModel.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <QtTest>
#include <QThreadPool>

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
std::optional<Coordinator::Prepared> prepare()
{
    AetherPDTest::PackageFixture fixture;
    const auto package = fixture.load();
    return package ? Coordinator::prepare(*package, "fixture-8600") : std::nullopt;
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
    void downloadedPackagePreparesPreviewWithoutCredentialOrRadioEffects()
    {
        Fixture f; AetherPDTest::PackageFixture fixture;
        AetherPDTest::Network downloads;
        AetherPDRelease release;
        release.version = "0.1.0-test"; release.firmware = "4.3.16.42034"; release.profile = 1; release.trust = fixture.trust;
        const auto names = AetherPDPackage::fileNames();
        for (std::size_t i = 0; i < 8; ++i) { release.urls[i] = QUrl(QStringLiteral("https://example.invalid/") + names[i]); }
        int loads = 0;
        AetherPDInstallationModel model(f.model, [&](const QString&, QObject*, auto) { ++loads; },
            [&](RadioModel& radio) { return std::make_unique<Coordinator>(radio, f.network, f.channel, f.archive, [&f] { return f.clock; }); },
            nullptr, [&] { return std::make_unique<AetherPDPackageDownload>(downloads); });
        QVERIFY(model.prepareRelease(release, f.model.controlSession()));
        QVERIFY(!model.approve()); QVERIFY(!model.prepareRelease(release, f.model.controlSession()));
        for (int i = 0; i < 8; ++i) { downloads.last->complete(200, fixture.bytes(i)); }
        QTRY_COMPARE(model.state(), AetherPDInstallationModel::State::AwaitingApproval);
        QVERIFY(model.approvedPlan().startsWith("PDUPGR02"));
        QCOMPARE(model.downloadReceived(), model.downloadTotal());
        QCOMPARE(loads, 0); QVERIFY(f.network.sent.empty()); QVERIFY(f.transfers.isEmpty());
        QVERIFY(model.approve()); QCOMPARE(loads, 1); model.cancel();
    }
    void changedSessionAndCancellationRetirePackagePreparation()
    {
        Fixture f; AetherPDTest::PackageFixture fixture;
        AetherPDTest::Network downloads;
        AetherPDRelease release;
        release.version = "0.1.0-test"; release.firmware = "4.3.16.42034"; release.profile = 1; release.trust = fixture.trust;
        const auto names = AetherPDPackage::fileNames();
        for (std::size_t i = 0; i < 8; ++i) { release.urls[i] = QUrl(QStringLiteral("https://example.invalid/") + names[i]); }
        int loads = 0;
        AetherPDInstallationModel model(f.model, [&](const QString&, QObject*, auto) { ++loads; }, {},
            nullptr, [&] { return std::make_unique<AetherPDPackageDownload>(downloads); });
        QVERIFY(model.prepareRelease(release, f.model.controlSession()));
        const QPointer<AetherPDTest::Reply> old = downloads.last;
        model.cancel(); QVERIFY(old->aborted);
        old->complete(200, fixture.bytes(0));
        QCOMPARE(model.state(), AetherPDInstallationModel::State::Finished);
        QVERIFY(model.prepareRelease(release, f.model.controlSession()));
        RadioSetupControlTestAccess::replaceConnectionGeneration(f.model);
        downloads.last->complete(200, fixture.bytes(0));
        QCOMPARE(model.failure(), AetherPDInstallationModel::Failure::SessionChanged);
        QVERIFY(model.prepareRelease(release, f.model.controlSession()));
        for (int i = 0; i < 8; ++i) { downloads.last->complete(200, fixture.bytes(i)); }
        QCOMPARE(model.state(), AetherPDInstallationModel::State::Preparing);
        model.cancel();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
        QCoreApplication::processEvents();
        QCOMPARE(model.state(), AetherPDInstallationModel::State::Finished);
        QVERIFY(model.approvedPlan().isEmpty()); QCOMPARE(loads, 0); QVERIFY(f.network.sent.empty());
    }
    void modelOwnsApprovalAndIgnoresLateCredentialAfterCancel()
    {
        Fixture f; const auto package = prepare(); QVERIFY(package);
        std::function<void(const QString&)> deliver;
        int loads = 0, creates = 0;
        AetherPDInstallationModel model(f.model,
            [&](const QString& serial, QObject*, auto callback) { QCOMPARE(serial, QStringLiteral("fixture-8600")); ++loads; deliver = std::move(callback); },
            [&](RadioModel& radio) { ++creates; return std::make_unique<Coordinator>(radio, f.network, f.channel, f.archive, [&f] { return f.clock; }); });
        QVERIFY(model.offer(*package, f.model.controlSession()));
        QCOMPARE(loads, 0); QVERIFY(f.network.sent.empty());
        QCOMPARE(model.state(), AetherPDInstallationModel::State::AwaitingApproval);
        QVERIFY(model.approve()); QVERIFY(!model.approve()); QCOMPARE(loads, 1);
        model.cancel(); deliver(QString(43, u'a'));
        QCOMPARE(creates, 0); QVERIFY(f.network.sent.empty());
        QCOMPARE(model.outcome(), std::optional(Coordinator::Outcome::Canceled));
        QVERIFY(model.cleanupConfirmed()); QCOMPARE(model.approvedPlan(), package->approvedPlan());
    }
    void credentialIsBoundToOriginalSessionAndSingleUse()
    {
        Fixture f; const auto package = prepare(); QVERIFY(package);
        std::function<void(const QString&)> deliver;
        int creates = 0;
        AetherPDInstallationModel model(f.model,
            [&](const QString&, QObject*, auto callback) { deliver = std::move(callback); },
            [&](RadioModel& radio) { ++creates; return std::make_unique<Coordinator>(radio, f.network, f.channel, f.archive, [&f] { return f.clock; }); });
        QVERIFY(model.offer(*package, f.model.controlSession())); QVERIFY(model.approve());
        RadioSetupControlTestAccess::replaceConnectionGeneration(f.model);
        deliver(QString(43, u'a'));
        QCOMPARE(model.failure(), AetherPDInstallationModel::Failure::SessionChanged);
        QCOMPARE(creates, 0); QVERIFY(f.network.sent.empty());
        QVERIFY(model.offer(*package, f.model.controlSession())); QVERIFY(model.approve());
        deliver(QString(43, u'a')); deliver(QString(43, u'b'));
        QCOMPARE(creates, 1); QCOMPARE(f.network.sent.size(), size_t(1));
        QCOMPARE(model.state(), AetherPDInstallationModel::State::Running);
        QVERIFY(!model.offer(*package, f.model.controlSession()));
    }
    void missingCredentialAndDeadlineNeverStartInstallation()
    {
        Fixture f; const auto package = prepare(); QVERIFY(package);
        std::function<void(const QString&)> deliver;
        AetherPDInstallationModel model(f.model,
            [&](const QString&, QObject*, auto callback) { deliver = std::move(callback); },
            [&](RadioModel& radio) { return std::make_unique<Coordinator>(radio, f.network, f.channel, f.archive, [&f] { return f.clock; }); });
        QVERIFY(model.offer(*package, f.model.controlSession())); QVERIFY(model.approve()); deliver({});
        QCOMPARE(model.failure(), AetherPDInstallationModel::Failure::NoCredential);
        QVERIFY(model.offer(*package, f.model.controlSession())); QVERIFY(model.approve());
        auto* deadline = model.findChild<QTimer*>(); QVERIFY(deadline);
        QVERIFY(QMetaObject::invokeMethod(deadline, "timeout", Qt::DirectConnection));
        deliver(QString(43, u'a'));
        QCOMPARE(model.failure(), AetherPDInstallationModel::Failure::CredentialTimeout);
        QVERIFY(f.network.sent.empty());
    }
    void lateCredentialCannotStartDuringTransmit()
    {
        Fixture f; const auto package = prepare(); QVERIFY(package);
        std::function<void(const QString&)> deliver;
        AetherPDInstallationModel model(f.model,
            [&](const QString&, QObject*, auto callback) { deliver = std::move(callback); },
            [&](RadioModel& radio) { return std::make_unique<Coordinator>(radio, f.network, f.channel, f.archive, [&f] { return f.clock; }); });
        QVERIFY(model.offer(*package, f.model.controlSession())); QVERIFY(model.approve());
        RadioSetupControlTestAccess::transmitting(f.model, true);
        deliver(QString(43, u'a'));
        QCOMPARE(model.failure(), AetherPDInstallationModel::Failure::StartRefused);
        QVERIFY(f.network.sent.empty()); QVERIFY(f.transfers.isEmpty());
    }
    void radioOwnsJobModelIndependentlyOfView()
    {
        QPointer<AetherPDInstallationModel> job;
        {
            RadioModel radio;
            job = &radio.aetherPDInstallationModel();
            {
                QObject view;
                connect(job, &AetherPDInstallationModel::changed, &view, [] {});
            }
            QVERIFY(job); QCOMPARE(job.data(), &radio.aetherPDInstallationModel());
        }
        QVERIFY(job.isNull());
    }
    void runningModelTracksCancellationThroughConfirmedCleanup()
    {
        Fixture f; const auto package = prepare(); QVERIFY(package);
        Coordinator* job = nullptr;
        AetherPDInstallationModel model(f.model,
            [](const QString&, QObject*, auto callback) { callback(QString(43, u'a')); },
            [&](RadioModel& radio) {
                auto created = std::make_unique<Coordinator>(radio, f.network, f.channel, f.archive, [&f] { return f.clock; });
                job = created.get(); return created;
            });
        QVERIFY(model.offer(*package, f.model.controlSession())); QVERIFY(model.approve()); QVERIFY(job);
        model.cancel(); QCOMPARE(model.state(), AetherPDInstallationModel::State::Canceling);
        f.network.last->complete(202, QJsonDocument(QJsonObject{{"state", "awaiting_inputs"},
            {"request_id", job->requestId()}, {"installation_committed", false}, {"port", 48995}}).toJson());
        job->poll(); f.network.last->complete(404, "{}"); job->poll();
        QCOMPARE(model.state(), AetherPDInstallationModel::State::Finished);
        QCOMPARE(model.outcome(), std::optional(Coordinator::Outcome::Canceled));
        QVERIFY(model.cleanupConfirmed()); QVERIFY(!model.active());
        QCOMPARE(model.approvedPlan(), package->approvedPlan());
        const Coordinator::Prepared copy = *package;
        QVERIFY(copy.used());
        QVERIFY(!model.offer(copy, f.model.controlSession()));
        QVERIFY(!f.begin(copy));
        const auto next = prepare(); QVERIFY(next);
        QVERIFY(model.offer(*next, f.model.controlSession()));
    }
    void changedIdentityRetiresRunningJobWithoutRemoval()
    {
        Fixture f; const auto package = prepare(); QVERIFY(package);
        AetherPDInstallationModel model(f.model,
            [](const QString&, QObject*, auto callback) { callback(QString(43, u'a')); },
            [&](RadioModel& radio) { return std::make_unique<Coordinator>(radio, f.network, f.channel, f.archive, [&f] { return f.clock; }); });
        QVERIFY(model.offer(*package, f.model.controlSession())); QVERIFY(model.approve());
        RadioSetupControlTestAccess::identity(f.model, "replacement", "4.3.16.42034");
        model.reconcile();
        QCOMPARE(model.state(), AetherPDInstallationModel::State::Finished);
        QCOMPARE(model.outcome(), std::optional(Coordinator::Outcome::Unconfirmed));
        QVERIFY(!model.cleanupConfirmed()); QVERIFY(f.transfers.isEmpty());
        QVERIFY(f.network.last->aborted);
    }
    void preparationBindsFreshAttempt()
    {
        const auto first = prepare(), second = prepare();
        QVERIFY(first); QVERIFY(second);
        QVERIFY(first->approvedPlan().startsWith("PDUPGR02"));
        QVERIFY(first->approvedPlan() != second->approvedPlan());
        QVERIFY(first->carrierName() != second->carrierName());
        AetherPDTest::PackageFixture fixture;
        const auto package = fixture.load(); QVERIFY(package);
        QVERIFY(!Coordinator::prepare(*package, QString()));
        AetherPDTest::PackageFixture arm(7);
        const auto armPackage = arm.load(); QVERIFY(armPackage);
        QVERIFY(!Coordinator::prepare(*armPackage, "fixture-6300"));
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
