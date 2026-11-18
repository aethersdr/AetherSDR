// Coordinator ordering only: injected HTTP replies and completed component
// events. Crypto/stream transport and waveform upload have separate tests.
#include "RadioSetupControlFixture.h"
#include "AetherPDTestTransport.h"
#include "AetherPDPackageFixture.h"
#include "AetherPDInstallationTestAdmission.h"
#include "core/AetherPDInstallationCoordinator.h"
#include "core/AetherPDInstallationMonitor.h"
#include "core/AetherPDInstallRequestClient.h"
#include "core/AetherPDProtocol.h"
#include "core/backends/AetherPDCarrierUpload.h"
#include "models/AetherPDInstallationModel.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <QtTest>
#include <QThreadPool>
#include <QTimer>

using namespace AetherSDR;
using Coordinator = AetherPDInstallationCoordinator;
namespace {
class Carrier final : public AetherPDCarrierUpload {
public:
    int starts{0}, nativeStarts{0};
    RadioModel* model{nullptr};
    void begin(const QString&, const RadioControlSession&, const FileTransferHandle&) override { ++starts; }
    void beginNative(const QString& path, const RadioControlSession&, const FileTransferHandle& reservation) override
    {
        ++nativeStarts;
        if (model) { model->requestFileTransfer(reservation, {FileTransferPurpose::LegacyWaveform,
            QFileInfo(path).size(), QFileInfo(path).fileName(), 0}, {}); }
    }
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
std::optional<AetherPDFirstPackage> firstPackage()
{
    AetherPDTest::PackageFixture fixture;
    return fixture.loadFirst();
}
AetherPDFirstRelease firstRelease(const AetherPDTest::PackageFixture& fixture)
{
    AetherPDFirstRelease release;
    release.version = "0.1.0-test"; release.firmware = "4.3.16.42034"; release.profile = 1;
    release.trust = fixture.firstTrust();
    const auto names = AetherPDFirstPackage::fileNames();
    for (std::size_t i = 0; i < names.size(); ++i) {
        release.urls[i] = QUrl(QStringLiteral("https://example.invalid/first/") + names[i]);
    }
    return release;
}
AetherPDReleaseCatalogue::Upgrade upgradeEntry(const AetherPDTest::PackageFixture& fixture)
{
    AetherPDRelease release;
    release.version = "0.2.0-catalogue"; release.firmware = "4.3.16.42034"; release.profile = 1; release.trust = fixture.trust;
    const auto names = AetherPDPackage::fileNames();
    for (std::size_t i = 0; i < names.size(); ++i) { release.urls[i] = QUrl(QStringLiteral("https://example.invalid/upgrade/") + names[i]); }
    const auto package = fixture.load();
    const auto plan = package ? aetherpd::parseInstallationPlan(package->plan("fixture-8600")) : std::nullopt;
    return {"FLEX-8600", release, plan ? QByteArray::fromHex(plan->previousDigest.toLatin1()) : QByteArray{},
        plan ? QByteArray::fromHex(plan->candidateDigest.toLatin1()) : QByteArray{}};
}
QByteArray installedRecord(const AetherPDReleaseCatalogue::Upgrade& entry, bool first = false)
{
    return QJsonDocument(QJsonObject{{"state", "committed"}, {"installation_committed", true}, {"profile", 1},
        {"firmware", entry.release.firmware}, {"serial", "fixture-8600"},
        {first ? "first_install_plan_blake2b_256" : "upgrade_plan_blake2b_256", QString(64, u'a')},
        {"selected_release_blake2b_256", QString::fromLatin1(entry.previousDigest.toHex())},
        {"selected_release_key", QString::fromLatin1(entry.release.trust.previousKey.toHex())}}).toJson();
}
std::optional<Coordinator::Prepared> prepareFirst()
{
    const auto package = firstPackage();
    return package ? Coordinator::prepareFirst(*package, "fixture-8600",
        QCryptographicHash::hash(QByteArray(64, 'a'), QCryptographicHash::Sha256), "fixture-radio") : std::nullopt;
}
AetherPDInstallationModel::RecoveryServices journalServices()
{
    AetherPDInstallationModel::RecoveryServices services;
    services.journal = [](const QByteArray&, const QByteArray&, bool) { return true; };
    return services;
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
    QList<std::function<void(int, const QString&)>> transferReplies;
    Fixture()
    {
        archive.model = &model;
        RadioSetupControlTestAccess::peer(model, QHostAddress(QStringLiteral("192.168.50.100")));
        RadioSetupControlTestAccess::identity(model, "fixture-8600", "4.3.16.42034");
        backend->setModelProvider([] { return QStringLiteral("FLEX-8600"); });
        model.handleLineForTest("S0|waveform wfp_status power=on ready=true");
        backend->setTransferCommandSink(RadioSetupControlTestAccess::productionTransferSink(model,
            [this](const QString& command, const FileTransferGuard&, const auto& reply) { transfers << command; transferReplies << reply; return true; }));
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
    void savedPreparationReproducesOnlyThePinnedPlan()
    {
        const auto package = firstPackage(); const auto original = prepareFirst(); QVERIFY(package); QVERIFY(original);
        const auto resumed = Coordinator::prepareSavedFirst(*package, original->approvedPlan(), original->publisherKey()); QVERIFY(resumed);
        QCOMPARE(resumed->approvedPlan(), original->approvedPlan());
        QCOMPARE(resumed->carrierSha256(), original->carrierSha256());
        QCOMPARE(resumed->nativeName(), original->nativeName()); QCOMPARE(resumed->carrierName(), original->carrierName());
        QVERIFY(!resumed->used());
        QByteArray changed = original->approvedPlan(); changed[24] ^= 1;
        QVERIFY(!Coordinator::prepareSavedFirst(*package, changed, original->publisherKey()));
        QVERIFY(!Coordinator::prepareSavedFirst(*package, original->approvedPlan(), QByteArray(32, 'z')));
        QVERIFY(!Coordinator::prepareSavedFirst(*package, original->approvedPlan().chopped(1), original->publisherKey()));
    }

    void savedAttemptRequiresItsCredentialAndExplicitApproval()
    {
        using Model = AetherPDInstallationModel; using Journal = AetherPDInstallationJournal;
        for (int mode = 0; mode < 6; ++mode) {
            Fixture f; const auto package = firstPackage(); const auto prepared = prepareFirst(); QVERIFY(package); QVERIFY(prepared);
            const QByteArray plan = prepared->approvedPlan(), key = prepared->publisherKey();
            Journal::Read saved{Journal::State::Pending, plan};
            std::function<void(const QString&)> loaded;
            int writes = 0, oldLoads = 0, retained = 0;
            auto services = journalServices();
            services.read = [&](const QByteArray& pin) { return pin == key ? saved : Journal::Read{}; };
            services.load = [&](const QString& serial, const QString& attempt, QObject*, auto done) {
                QCOMPARE(serial, QStringLiteral("fixture-8600"));
                QCOMPARE(attempt, QString::fromLatin1(QCryptographicHash::hash(plan, QCryptographicHash::Blake2b_256).toHex()));
                loaded = std::move(done);
            };
            services.retainPending = [&](const QByteArray& record, const QByteArray& pin) {
                ++retained; return saved.state == Journal::State::Pending && saved.plan == record && pin == key;
            };
            Model model(f.model, [&](const QString&, QObject*, auto) { ++oldLoads; },
                [&](RadioModel& radio) { return std::make_unique<Coordinator>(radio, f.network, f.channel, f.archive, [&] { return f.clock; }); },
                nullptr, {}, [&](const QString&, const QString&, const QString&, QObject*, auto) { ++writes; }, services);
            AetherPDInstallationTestAccess::inject(model);
            QVERIFY(model.prepareSavedFirstPackage(*package, key, f.model.controlSession())); QVERIFY(loaded);
            QVERIFY(model.preparingSavedInstallation()); QVERIFY(f.transfers.isEmpty()); QCOMPARE(writes, 0);
            if (mode == 1) { loaded(QString(64, u'b')); QCOMPARE(model.failure(), Model::Failure::NoCredential); }
            else if (mode == 2) { model.cancel(); loaded(QString(64, u'a')); QCOMPARE(model.outcome(), Coordinator::Outcome::Canceled); }
            else if (mode == 3) { RadioSetupControlTestAccess::replaceConnectionGeneration(f.model); loaded(QString(64, u'a')); QCOMPARE(model.failure(), Model::Failure::SessionChanged); }
            else if (mode == 4) { saved.state = Journal::State::CredentialSaved; loaded(QString(64, u'a')); QCOMPARE(model.failure(), Model::Failure::JournalFailed); }
            else {
                loaded(QString(64, u'a'));
                QTRY_COMPARE(model.state(), Model::State::AwaitingApproval);
                QCOMPARE(model.approvedPlan(), plan); QCOMPARE(model.carrierName(), prepared->carrierName());
                QVERIFY(f.transfers.isEmpty()); QVERIFY(f.network.sent.empty()); QCOMPARE(writes, 0); QCOMPARE(retained, 0);
                QVERIFY(!model.approve(plan + 'x'));
                if (mode == 5) {
                    saved.state = Journal::State::CredentialSaved;
                    QVERIFY(!model.approve(plan)); QCOMPARE(model.failure(), Model::Failure::JournalFailed);
                } else {
                    QVERIFY(model.approve(plan)); QCOMPARE(f.archive.nativeStarts, 1); QCOMPARE(retained, 1);
                    QCOMPARE(model.phase(), Coordinator::State::UploadingNative); model.cancel();
                }
            }
            QCOMPARE(writes, 0); QCOMPARE(oldLoads, 0); QVERIFY(!f.channel.connected);
            if (mode != 0) { QVERIFY(f.transfers.isEmpty()); QCOMPARE(f.archive.nativeStarts, 0); }
        }
    }

    void recoveredCleanupRechecksCommitAndRemovesOnlyTheAttempt()
    {
        Fixture f; const auto prepared = prepareFirst(); QVERIFY(prepared);
        const QByteArray key = prepared->publisherKey();
        QSignalSpy done(&f.coordinator, &Coordinator::finished);
        QVERIFY(!f.coordinator.beginFirstCleanup(prepared->approvedPlan(), key, f.model.controlSession(), QString(64, u'b')));
        QVERIFY(f.network.sent.empty()); QVERIFY(f.transfers.isEmpty());
        QVERIFY(f.coordinator.beginFirstCleanup(prepared->approvedPlan(), key, f.model.controlSession(), QString(64, u'a')));
        f.carrier(true); QVERIFY(f.reserved()); QVERIFY(f.transfers.isEmpty());
        QCOMPARE(f.archive.starts, 0); QCOMPARE(f.archive.nativeStarts, 0); QVERIFY(!f.channel.connected);
        const auto plan = aetherpd::parseFirstInstallationPlan(prepared->approvedPlan(), key); QVERIFY(plan);
        auto* reply = f.pending("/installation"); QVERIFY(reply);
        reply->complete(200, QJsonDocument(QJsonObject{{"state", "committed"}, {"installation_committed", true},
            {"first_install_plan_blake2b_256", plan->planDigest}, {"selected_release_blake2b_256", plan->candidateDigest},
            {"selected_release_key", plan->candidateKey}, {"profile", 1}, {"firmware", plan->firmware}, {"serial", plan->serial}}).toJson());
        f.coordinator.poll();
        QCOMPARE(f.transfers, QStringList({"waveform remove_container " + prepared->carrierName(), "waveform uninstall " + prepared->nativeName()}));
        { const auto accepted = f.transferReplies[1]; accepted(0, {}); }
        f.carrier(false); QVERIFY(done.isEmpty());
        { const auto accepted = f.transferReplies[0]; accepted(0, {}); }
        f.coordinator.poll(); QCOMPARE(done.size(), 1); QVERIFY(done.first()[1].toBool());
        QCOMPARE(done.first()[0].value<Coordinator::Outcome>(), Coordinator::Outcome::Committed);
        QVERIFY(!f.reserved());
        QCOMPARE(f.network.sent.size(), std::size_t(1));
        QCOMPARE(f.network.sent.front().method, QNetworkAccessManager::GetOperation);
    }

    void recoveredCleanupNeverRemovesBeforeExactCommit()
    {
        for (int mode = 0; mode < 5; ++mode) {
            Fixture f; const auto prepared = prepareFirst(); QVERIFY(prepared);
            QSignalSpy done(&f.coordinator, &Coordinator::finished);
            QVERIFY(f.coordinator.beginFirstCleanup(prepared->approvedPlan(), prepared->publisherKey(), f.model.controlSession(), QString(64, u'a')));
            f.carrier(true);
            const auto plan = aetherpd::parseFirstInstallationPlan(prepared->approvedPlan(), prepared->publisherKey()); QVERIFY(plan);
            if (mode == 0) {
                auto* reply = f.pending("/installation"); QVERIFY(reply);
                reply->complete(200, QJsonDocument(QJsonObject{{"state", "committed"}, {"installation_committed", true},
                    {"first_install_plan_blake2b_256", QString(64, u'0')}, {"selected_release_blake2b_256", plan->candidateDigest},
                    {"selected_release_key", plan->candidateKey}, {"profile", 1}, {"firmware", plan->firmware}, {"serial", plan->serial}}).toJson());
            } else if (mode == 1) { f.coordinator.cancel(); }
            else if (mode == 2) { RadioSetupControlTestAccess::replaceConnectionGeneration(f.model); }
            else if (mode == 3) { auto* reply = f.pending("/installation"); QVERIFY(reply); reply->complete(401, "{}"); }
            f.coordinator.poll();
            f.clock = 500'000; f.coordinator.poll();
            QVERIFY(f.transfers.isEmpty()); QCOMPARE(f.archive.starts, 0); QCOMPARE(f.archive.nativeStarts, 0);
            QVERIFY(!f.channel.connected); QCOMPARE(done.size(), 1); QVERIFY(!done.first()[1].toBool());
        }
    }

    void freshInstallUsesPublicUploadsAndNewIdentityOnly()
    {
        Fixture f; const auto prepared = prepareFirst(); QVERIFY(prepared);
        QSignalSpy done(&f.coordinator, &Coordinator::finished);
        QVERIFY(!f.coordinator.begin(*prepared, f.model.controlSession(), QString(64, u'b')));
        QVERIFY(f.transfers.isEmpty()); QVERIFY(f.network.sent.empty());
        QVERIFY(f.coordinator.begin(*prepared, f.model.controlSession(), QString(64, u'a')));
        QCOMPARE(f.archive.nativeStarts, 1); QCOMPARE(f.archive.starts, 0); QVERIFY(f.reserved());
        QVERIFY(f.network.sent.empty()); QVERIFY(!f.channel.connected);
        { const auto reply = f.transferReplies.last(); reply(0, {}); }
        { const auto reply = f.transferReplies.last(); reply(0, "42607"); }
        f.archive.complete(); QCOMPARE(f.coordinator.state(), Coordinator::State::WaitingForNative);
        f.clock = 2999; f.coordinator.poll(); QCOMPARE(f.archive.starts, 0);
        f.clock = 3000; f.coordinator.poll(); QCOMPARE(f.archive.starts, 1);
        f.archive.complete(); f.carrier(true);
        QCOMPARE(f.coordinator.state(), Coordinator::State::WaitingForCommit);
        QCOMPARE(f.network.sent.size(), std::size_t(1)); QVERIFY(!f.channel.connected);
        QCOMPARE(f.network.sent.back().method, QNetworkAccessManager::GetOperation);
        auto* observation = f.pending("/installation"); QVERIFY(observation);
        AetherPDTest::PackageFixture fixture;
        const auto plan = aetherpd::parseFirstInstallationPlan(prepared->approvedPlan(), fixture.trust.candidateKey); QVERIFY(plan);
        observation->complete(200, QJsonDocument(QJsonObject{{"state", "committed"}, {"installation_committed", true},
            {"first_install_plan_blake2b_256", plan->planDigest}, {"selected_release_blake2b_256", plan->candidateDigest},
            {"selected_release_key", plan->candidateKey}, {"profile", 1}, {"firmware", plan->firmware}, {"serial", plan->serial}}).toJson());
        f.coordinator.poll(); QVERIFY(f.reserved()); QVERIFY(done.isEmpty());
        QVERIFY(f.transfers.contains("waveform remove_container " + prepared->carrierName()));
        QCOMPARE(f.transfers.last(), "waveform uninstall " + prepared->nativeName());
        { const auto reply = f.transferReplies.last(); reply(0, {}); }
        f.carrier(false);
        QVERIFY(done.isEmpty());
        { const auto reply = f.transferReplies[f.transfers.indexOf("waveform remove_container " + prepared->carrierName())]; reply(0, {}); }
        f.coordinator.poll();
        QCOMPARE(done.size(), 1); QCOMPARE(done.first()[0].value<Coordinator::Outcome>(), Coordinator::Outcome::Committed);
        QVERIFY(done.first()[1].toBool()); QVERIFY(!f.reserved());
        for (const auto& sent : f.network.sent) { QCOMPARE(sent.method, QNetworkAccessManager::GetOperation); }
    }

    void freshCancelRemovesOnlyNativeAndWaitsForAcknowledgment()
    {
        Fixture f; const auto prepared = prepareFirst(); QVERIFY(prepared);
        QSignalSpy done(&f.coordinator, &Coordinator::finished);
        QVERIFY(f.coordinator.begin(*prepared, f.model.controlSession(), QString(64, u'a')));
        f.coordinator.cancel(); f.coordinator.poll();
        QCOMPARE(f.transfers.last(), "waveform uninstall " + prepared->nativeName());
        QVERIFY(done.isEmpty()); QVERIFY(f.reserved());
        { const auto reply = f.transferReplies.last(); reply(0, {}); }
        f.coordinator.poll();
        QCOMPARE(done.size(), 1); QCOMPARE(done.first()[0].value<Coordinator::Outcome>(), Coordinator::Outcome::Canceled);
        QCOMPARE(f.archive.starts, 0); QVERIFY(f.network.sent.empty()); QVERIFY(!f.channel.connected);
        QVERIFY(prepared->used());
    }

    void freshCancelRemovesPreviouslyObservedCarrierAfterItDisappears()
    {
        for (const bool accepted : {false, true}) {
            Fixture f; const auto prepared = prepareFirst(); QVERIFY(prepared);
            QSignalSpy done(&f.coordinator, &Coordinator::finished);
            QVERIFY(f.coordinator.begin(*prepared, f.model.controlSession(), QString(64, u'a')));
            { const auto reply = f.transferReplies.last(); reply(0, {}); }
            { const auto reply = f.transferReplies.last(); reply(0, "42607"); }
            f.archive.complete(); f.clock = 3000; f.coordinator.poll();
            f.archive.complete(); f.carrier(true); f.carrier(false);
            QCOMPARE(f.coordinator.state(), Coordinator::State::WaitingForCommit);
            f.coordinator.cancel(); f.coordinator.poll();
            const QString remove = "waveform remove_container " + prepared->carrierName();
            const int removeIndex = f.transfers.indexOf(remove);
            QVERIFY(removeIndex >= 0);
            const int nativeIndex = f.transfers.indexOf("waveform uninstall " + prepared->nativeName());
            QVERIFY(nativeIndex >= 0);
            { const auto reply = f.transferReplies[nativeIndex]; reply(0, {}); }
            f.coordinator.poll(); QVERIFY(done.isEmpty());
            { const auto reply = f.transferReplies[removeIndex]; reply(accepted ? 0 : 1, {}); }
            f.coordinator.poll();
            if (!accepted) {
                QVERIFY(done.isEmpty());
                f.clock = 63000; f.coordinator.poll();
            }
            QCOMPARE(done.size(), 1);
            QCOMPARE(done.first()[0].value<Coordinator::Outcome>(), Coordinator::Outcome::Canceled);
            QCOMPARE(done.first()[1].toBool(), accepted);
            QCOMPARE(f.transfers.count(remove), 1);
            QCOMPARE(f.archive.starts, 1); QCOMPARE(f.archive.nativeStarts, 1);
        }
    }

    void freshDestructorKeepsUploadSettleWindow()
    {
        Fixture f; const auto prepared = prepareFirst(); QVERIFY(prepared);
        auto owner = std::make_unique<Coordinator>(f.model, f.network, f.channel, f.archive, [&f] { return f.clock; });
        QVERIFY(owner->begin(*prepared, f.model.controlSession(), QString(64, u'a')));
        owner.reset();
        QVERIFY(f.reserved());
        QCOMPARE(f.archive.starts, 0); QVERIFY(f.network.sent.empty());
    }

    void freshDeadlineCannotStartLateCarrier()
    {
        Fixture f; const auto prepared = prepareFirst(); QVERIFY(prepared);
        QVERIFY(f.coordinator.begin(*prepared, f.model.controlSession(), QString(64, u'a')));
        { const auto reply = f.transferReplies.last(); reply(0, {}); }
        { const auto reply = f.transferReplies.last(); reply(0, "42607"); }
        f.archive.complete();
        f.clock = 500000; f.coordinator.poll();
        QCOMPARE(f.coordinator.state(), Coordinator::State::Cleaning);
        QCOMPARE(f.archive.starts, 0); QVERIFY(f.network.sent.empty());
    }

    void freshSessionLossNeverUsesReplacementForCleanup()
    {
        Fixture f; const auto prepared = prepareFirst(); QVERIFY(prepared);
        QSignalSpy done(&f.coordinator, &Coordinator::finished);
        QVERIFY(f.coordinator.begin(*prepared, f.model.controlSession(), QString(64, u'a')));
        const auto before = f.transfers;
        RadioSetupControlTestAccess::replaceConnectionGeneration(f.model);
        f.coordinator.poll();
        QCOMPARE(done.size(), 1); QVERIFY(!done.first()[1].toBool()); QCOMPARE(f.transfers, before);
        QVERIFY(f.network.sent.empty()); QCOMPARE(f.archive.starts, 0);
    }

    void firstApprovalRefusesConcurrentRemoteSetup()
    {
        Fixture f; const auto package = firstPackage(); QVERIFY(package);
        int writes = 0;
        std::function<void(bool)> saved;
        AetherPDInstallationModel model(f.model, {},
            [&](RadioModel& radio) { return std::make_unique<Coordinator>(radio, f.network, f.channel, f.archive, [&] { return f.clock; }); },
            nullptr, {}, [&](const QString&, const QString&, const QString&, QObject*, auto done) { ++writes; saved = std::move(done); }, journalServices());
        AetherPDInstallationTestAccess::inject(model);
        QVERIFY(model.prepareFirstPackage(*package, "fixture-radio", f.model.controlSession()));
        QTRY_COMPARE(model.state(), AetherPDInstallationModel::State::AwaitingApproval);
        auto setup = AetherPDInstallationTestAccess::acquire(QStringLiteral("fixture-8600"));
        QVERIFY(setup);
        const QByteArray plan = model.approvedPlan();
        QVERIFY(!model.approve(model.approvedPlan()));
        QCOMPARE(model.failure(), AetherPDInstallationModel::Failure::SetupBusy);
        QCOMPARE(model.state(), AetherPDInstallationModel::State::AwaitingApproval);
        QCOMPARE(model.approvedPlan(), plan);
        QCOMPARE(writes, 0); QVERIFY(f.transfers.isEmpty()); QVERIFY(f.network.sent.empty());
        setup.reset();
        QVERIFY(model.approve(plan)); QCOMPARE(writes, 1); QVERIFY(saved);
        saved(false); QVERIFY(f.transfers.isEmpty()); QVERIFY(f.network.sent.empty());
    }

    void firstCredentialBackendRetainsAdmissionAfterCancelTimeoutOrDestruction()
    {
        for (int mode = 0; mode < 3; ++mode) {
            Fixture f; const auto package = firstPackage(); QVERIFY(package);
            std::function<void(bool)> saved;
            auto model = std::make_unique<AetherPDInstallationModel>(f.model, AetherPDInstallationModel::TokenLoader{},
                [&](RadioModel& radio) { return std::make_unique<Coordinator>(radio, f.network, f.channel, f.archive, [&] { return f.clock; }); },
                nullptr, AetherPDInstallationModel::DownloadFactory{},
                [&](const QString&, const QString&, const QString&, QObject*, auto done) { saved = std::move(done); }, journalServices());
            AetherPDInstallationTestAccess::inject(*model);
            QVERIFY(model->prepareFirstPackage(*package, "fixture-radio", f.model.controlSession()));
            QTRY_COMPARE(model->state(), AetherPDInstallationModel::State::AwaitingApproval);
            QVERIFY(model->approve(model->approvedPlan())); QVERIFY(saved);
            QVERIFY(!AetherPDInstallationTestAccess::acquire(QStringLiteral("fixture-8600")));
            QVERIFY(AetherPDInstallationTestAccess::acquire(QStringLiteral("another-fixture-radio")));
            if (mode == 0) { model->cancel(); }
            else if (mode == 1) { QVERIFY(QMetaObject::invokeMethod(model->findChild<QTimer*>(), "timeout", Qt::DirectConnection)); }
            else { model.reset(); }
            QVERIFY(!AetherPDInstallationTestAccess::acquire(QStringLiteral("fixture-8600")));
            const auto completed = saved;
            completed(true);
            auto next = AetherPDInstallationTestAccess::acquire(QStringLiteral("fixture-8600"));
            QVERIFY(next);
            // Even a retained duplicate completion cannot release a later job.
            saved(false);
            QVERIFY(!AetherPDInstallationTestAccess::acquire(QStringLiteral("fixture-8600")));
            QVERIFY(f.transfers.isEmpty()); QVERIFY(f.network.sent.empty());
        }
    }

    void twoFirstInstallationsRequireExplicitRetryAfterTheSaveOutcome()
    {
        Fixture f; const auto package = firstPackage(); QVERIFY(package);
        std::function<void(bool)> firstSaved, secondSaved;
        int firstWrites = 0, secondWrites = 0;
        const auto factory = [&](RadioModel& radio) { return std::make_unique<Coordinator>(radio, f.network, f.channel, f.archive, [&] { return f.clock; }); };
        AetherPDInstallationModel first(f.model, {}, factory, nullptr, {},
            [&](const QString&, const QString&, const QString&, QObject*, auto done) { ++firstWrites; firstSaved = std::move(done); }, journalServices());
        AetherPDInstallationModel second(f.model, {}, factory, nullptr, {},
            [&](const QString&, const QString&, const QString&, QObject*, auto done) { ++secondWrites; secondSaved = std::move(done); }, journalServices());
        AetherPDInstallationTestAccess::inject(first); AetherPDInstallationTestAccess::inject(second);
        QVERIFY(first.prepareFirstPackage(*package, "fixture-radio", f.model.controlSession()));
        QVERIFY(second.prepareFirstPackage(*package, "fixture-radio", f.model.controlSession()));
        QTRY_COMPARE(first.state(), AetherPDInstallationModel::State::AwaitingApproval);
        QTRY_COMPARE(second.state(), AetherPDInstallationModel::State::AwaitingApproval);
        QVERIFY(first.approve(first.approvedPlan())); QCOMPARE(firstWrites, 1);
        QVERIFY(!second.approve(second.approvedPlan())); QCOMPARE(secondWrites, 0);
        firstSaved(false);
        QCOMPARE(secondWrites, 0);
        QVERIFY(second.approve(second.approvedPlan())); QCOMPARE(secondWrites, 1);
        secondSaved(false);
        QVERIFY(AetherPDInstallationTestAccess::acquire(QStringLiteral("fixture-8600")));
        QVERIFY(f.transfers.isEmpty()); QVERIFY(f.network.sent.empty());
    }

    void firstCredentialMustPersistBeforeAnyUpload()
    {
        Fixture f; const auto package = firstPackage(); QVERIFY(package);
        int loads = 0;
        QString savedSerial, savedAttempt, savedToken;
        std::function<void(bool)> saved;
        AetherPDInstallationModel model(f.model, [&](const QString&, QObject*, auto) { ++loads; },
            [&](RadioModel& radio) { return std::make_unique<Coordinator>(radio, f.network, f.channel, f.archive, [&] { return f.clock; }); },
            nullptr, {}, [&](const QString& serial, const QString& attempt, const QString& token, QObject*, auto done) {
                savedSerial = serial; savedAttempt = attempt; savedToken = token; saved = std::move(done);
            }, journalServices());
        AetherPDInstallationTestAccess::inject(model);
        QVERIFY(model.prepareFirstPackage(*package, "fixture-radio", f.model.controlSession()));
        QTRY_COMPARE(model.state(), AetherPDInstallationModel::State::AwaitingApproval);
        const auto preview = model.preview(); QVERIFY(preview); QVERIFY(preview->firstInstallation);
        QVERIFY(preview->previousDigest.isEmpty()); QVERIFY(!preview->nativeName.isEmpty());
        QVERIFY(!saved); QVERIFY(f.transfers.isEmpty()); QCOMPARE(loads, 0);
        QVERIFY(!model.approve(preview->plan + 'x')); QVERIFY(!saved);
        QVERIFY(model.approve(preview->plan));
        QCOMPARE(model.state(), AetherPDInstallationModel::State::SavingCredential);
        QCOMPARE(savedSerial, QStringLiteral("fixture-8600")); QCOMPARE(savedAttempt, preview->planDigest);
        QCOMPARE(savedToken.size(), 64);
        QCOMPARE(QCryptographicHash::hash(savedToken.toLatin1(), QCryptographicHash::Sha256), preview->plan.mid(148, 32));
        QVERIFY(!preview->plan.contains(savedToken.toLatin1())); QVERIFY(f.transfers.isEmpty());
        saved(false);
        QCOMPARE(model.failure(), AetherPDInstallationModel::Failure::CredentialSaveFailed);
        QVERIFY(f.transfers.isEmpty()); QCOMPARE(f.archive.nativeStarts, 0); QCOMPARE(loads, 0);
        saved(true); QCOMPARE(f.archive.nativeStarts, 0);
    }

    void firstCredentialLateCompletionCannotStartCanceledOrReplacedSession()
    {
        for (int mode = 0; mode < 3; ++mode) {
            Fixture f; const auto package = firstPackage(); QVERIFY(package);
            std::function<void(bool)> saved;
            AetherPDInstallationModel model(f.model, {},
                [&](RadioModel& radio) { return std::make_unique<Coordinator>(radio, f.network, f.channel, f.archive, [&] { return f.clock; }); },
                nullptr, {}, [&](const QString&, const QString&, const QString&, QObject*, auto done) { saved = std::move(done); }, journalServices());
            AetherPDInstallationTestAccess::inject(model);
            QVERIFY(model.prepareFirstPackage(*package, "fixture-radio", f.model.controlSession()));
            QTRY_COMPARE(model.state(), AetherPDInstallationModel::State::AwaitingApproval);
            QVERIFY(model.approve(model.approvedPlan())); QVERIFY(saved);
            if (mode == 0) { model.cancel(); }
            else if (mode == 1) { RadioSetupControlTestAccess::replaceConnectionGeneration(f.model); model.reconcile(); }
            else { QVERIFY(QMetaObject::invokeMethod(model.findChild<QTimer*>(), "timeout", Qt::DirectConnection)); }
            saved(true);
            QCOMPARE(f.archive.nativeStarts, 0); QVERIFY(f.transfers.isEmpty());
        }
    }

    void firstCredentialPromotesOnlyAfterExactCommit()
    {
        for (int mode = 0; mode < 3; ++mode) {
            const bool persisted = mode != 0;
            Fixture f; const auto package = firstPackage(); QVERIFY(package);
            Coordinator* job = nullptr;
            QStringList attempts;
            QStringList tokens;
            std::function<void(bool)> saved;
            AetherPDInstallationModel model(f.model, {}, [&](RadioModel& radio) {
                auto coordinator = std::make_unique<Coordinator>(radio, f.network, f.channel, f.archive, [&] { return f.clock; });
                job = coordinator.get(); return coordinator;
            }, nullptr, {}, [&](const QString&, const QString& attempt, const QString& token, QObject*, auto done) {
                attempts << attempt; tokens << token; saved = std::move(done);
            }, journalServices());
            AetherPDInstallationTestAccess::inject(model);
            QVERIFY(model.prepareFirstPackage(*package, "fixture-radio", f.model.controlSession()));
            QTRY_COMPARE(model.state(), AetherPDInstallationModel::State::AwaitingApproval);
            const auto preview = model.preview(); QVERIFY(preview);
            QVERIFY(model.approve(preview->plan));
            { const auto complete = saved; complete(true); }
            QCOMPARE(f.archive.nativeStarts, 1); QCOMPARE(attempts.size(), 1); QVERIFY(job);
            { const auto reply = f.transferReplies.last(); reply(0, {}); }
            { const auto reply = f.transferReplies.last(); reply(0, "42607"); }
            f.archive.complete(); f.clock = 3000; job->poll(); f.archive.complete();
            f.model.handleLineForTest("S0|waveform container name=" + model.carrierName() + " version=1"); job->poll();
            auto* observation = f.pending("/installation"); QVERIFY(observation);
            observation->complete(200, QJsonDocument(QJsonObject{{"state", "committed"}, {"installation_committed", true},
                {"first_install_plan_blake2b_256", preview->planDigest}, {"selected_release_blake2b_256", preview->candidateDigest},
                {"selected_release_key", preview->candidateKey}, {"profile", 1}, {"firmware", preview->firmware}, {"serial", preview->serial}}).toJson());
            job->poll(); QCOMPARE(attempts.size(), 1);
            { const auto reply = f.transferReplies.last(); reply(0, {}); }
            f.model.handleLineForTest("S0|waveform container name=" + model.carrierName() + " removed"); job->poll();
            QCOMPARE(model.state(), AetherPDInstallationModel::State::Running);
            { const auto reply = f.transferReplies[f.transfers.indexOf("waveform remove_container " + model.carrierName())]; reply(0, {}); }
            job->poll();
            QCOMPARE(model.state(), AetherPDInstallationModel::State::PromotingCredential);
            QVERIFY(!AetherPDInstallationTestAccess::acquire(QStringLiteral("fixture-8600")));
            QCOMPARE(attempts.size(), 2); QVERIFY(attempts.last().isEmpty()); QCOMPARE(tokens.first(), tokens.last());
            model.cancel(); QCOMPARE(model.state(), AetherPDInstallationModel::State::PromotingCredential);
            if (mode == 2) {
                QVERIFY(QMetaObject::invokeMethod(model.findChild<QTimer*>(), "timeout", Qt::DirectConnection));
                QCOMPARE(model.state(), AetherPDInstallationModel::State::Finished);
                QVERIFY(!AetherPDInstallationTestAccess::acquire(QStringLiteral("fixture-8600")));
            }
            { const auto complete = saved; complete(persisted); }
            QVERIFY(AetherPDInstallationTestAccess::acquire(QStringLiteral("fixture-8600")));
            QCOMPARE(model.state(), AetherPDInstallationModel::State::Finished);
            QCOMPARE(model.outcome(), Coordinator::Outcome::Committed); QVERIFY(model.cleanupConfirmed());
            QCOMPARE(model.failure(), persisted && mode != 2 ? AetherPDInstallationModel::Failure::None : AetherPDInstallationModel::Failure::CredentialSaveFailed);
        }
    }

    void journalFailurePreventsFirstUpload()
    {
        Fixture f; const auto package = firstPackage(); QVERIFY(package);
        int writes = 0;
        auto services = journalServices();
        services.journal = [](const QByteArray&, const QByteArray&, bool) { return false; };
        AetherPDInstallationModel model(f.model, {},
            [&](RadioModel& radio) { return std::make_unique<Coordinator>(radio, f.network, f.channel, f.archive, [&] { return f.clock; }); },
            nullptr, {}, [&](const QString&, const QString&, const QString&, QObject*, auto done) { ++writes; done(true); }, services);
        AetherPDInstallationTestAccess::inject(model);
        QVERIFY(model.prepareFirstPackage(*package, "fixture-radio", f.model.controlSession()));
        QTRY_COMPARE(model.state(), AetherPDInstallationModel::State::AwaitingApproval);
        QVERIFY(model.approve(model.approvedPlan()));
        QCOMPARE(writes, 1); QCOMPARE(model.failure(), AetherPDInstallationModel::Failure::JournalFailed);
        QCOMPARE(f.archive.nativeStarts, 0); QVERIFY(f.transfers.isEmpty()); QVERIFY(f.network.sent.empty());
    }

    void recoveryObservesOnlyAndPromotesExactCommit()
    {
        Fixture f; const auto prepared = prepareFirst(); QVERIFY(prepared);
        AetherPDTest::PackageFixture fixture;
        const QByteArray key = fixture.trust.candidateKey;
        int writes = 0, journalCompletions = 0;
        auto services = journalServices();
        services.read = [&](const QByteArray& pin) {
            return pin == key ? AetherPDInstallationJournal::Read{AetherPDInstallationJournal::State::Pending, prepared->approvedPlan()}
                              : AetherPDInstallationJournal::Read{AetherPDInstallationJournal::State::Invalid, {}};
        };
        services.load = [](const QString&, const QString&, QObject*, auto done) { done(QString(64, u'a')); };
        services.monitor = [&] { return std::make_unique<AetherPDInstallationMonitor>(f.network, [&] { return f.clock; }); };
        services.journal = [&](const QByteArray& plan, const QByteArray& pin, bool complete) {
            if (complete && plan == prepared->approvedPlan() && pin == key) { ++journalCompletions; return true; }
            return false;
        };
        AetherPDInstallationModel model(f.model, {}, [&](RadioModel& radio) {
                return std::make_unique<Coordinator>(radio, f.network, f.channel, f.archive, [&] { return f.clock; });
            }, nullptr, {},
            [&](const QString&, const QString& attempt, const QString& token, QObject*, auto done) {
                QVERIFY(attempt.isEmpty()); QCOMPARE(token, QString(64, u'a')); ++writes; done(true);
            }, services);
        AetherPDInstallationTestAccess::inject(model);
        QVERIFY(!model.recoverFirstInstallation(QByteArray(32, 'z'), f.model.controlSession()));
        QVERIFY(f.network.sent.empty());
        auto setup = AetherPDInstallationTestAccess::acquire(QStringLiteral("fixture-8600"));
        QVERIFY(setup);
        QVERIFY(model.recoverFirstInstallation(key, f.model.controlSession()));
        QCOMPARE(model.state(), AetherPDInstallationModel::State::Unavailable);
        QCOMPARE(model.failure(), AetherPDInstallationModel::Failure::SetupBusy);
        QCOMPARE(writes, 0); QVERIFY(f.network.sent.empty());
        setup.reset();
        QVERIFY(model.recoverFirstInstallation(key, f.model.controlSession()));
        QCOMPARE(model.state(), AetherPDInstallationModel::State::CheckingInstallation);
        QVERIFY(!AetherPDInstallationTestAccess::acquire(QStringLiteral("fixture-8600")));
        QVERIFY(model.recovering()); QCOMPARE(writes, 0); QVERIFY(!model.approve(model.approvedPlan()));
        const auto preview = model.preview(); QVERIFY(preview);
        auto* reply = f.pending("/installation"); QVERIFY(reply);
        reply->complete(200, QJsonDocument(QJsonObject{{"state", "committed"}, {"installation_committed", true},
            {"first_install_plan_blake2b_256", preview->planDigest}, {"selected_release_blake2b_256", preview->candidateDigest},
            {"selected_release_key", preview->candidateKey}, {"profile", 1}, {"firmware", preview->firmware}, {"serial", preview->serial}}).toJson());
        QCOMPARE(model.state(), AetherPDInstallationModel::State::Finished);
        QCOMPARE(model.outcome(), Coordinator::Outcome::Committed); QVERIFY(!model.cleanupConfirmed());
        QCOMPARE(writes, 1); QCOMPARE(journalCompletions, 1); QVERIFY(f.transfers.isEmpty());
        QVERIFY(AetherPDInstallationTestAccess::acquire(QStringLiteral("fixture-8600")));
        QCOMPARE(f.archive.nativeStarts, 0); QCOMPARE(f.archive.starts, 0); QVERIFY(!f.channel.connected);
        for (const auto& sent : f.network.sent) { QCOMPARE(sent.method, QNetworkAccessManager::GetOperation); }
        QVERIFY(model.recoveredCleanupAvailable());
        QVERIFY(!model.cleanupRecoveredInstallation(QByteArray(308, 'x')));
        QVERIFY(model.cleanupRecoveredInstallation(prepared->approvedPlan()));
        QVERIFY(model.cleaningRecoveredInstallation()); QCOMPARE(model.state(), AetherPDInstallationModel::State::Running);
        QCOMPARE(f.network.sent.size(), std::size_t(2)); QVERIFY(f.transfers.isEmpty());
        QCOMPARE(f.archive.nativeStarts, 0); QCOMPARE(f.archive.starts, 0);
        model.cancel(); QCOMPARE(model.outcome(), Coordinator::Outcome::Canceled); QVERIFY(!model.cleanupConfirmed());
        QVERIFY(f.transfers.isEmpty()); QCOMPARE(writes, 1);
    }

    void recoveryCancellationSessionLossAndWrongTokenDoNotAct()
    {
        for (int mode = 0; mode < 5; ++mode) {
            Fixture f; const auto prepared = prepareFirst(); QVERIFY(prepared);
            std::function<void(const QString&)> loaded;
            int writes = 0;
            auto services = journalServices();
            services.read = [&](const QByteArray&) { return AetherPDInstallationJournal::Read{AetherPDInstallationJournal::State::Pending, prepared->approvedPlan()}; };
            services.load = [&](const QString&, const QString&, QObject*, auto done) { loaded = std::move(done); };
            services.monitor = [&] { return std::make_unique<AetherPDInstallationMonitor>(f.network, [&] { return f.clock; }); };
            AetherPDInstallationModel model(f.model, {}, {}, nullptr, {},
                [&](const QString&, const QString&, const QString&, QObject*, auto) { ++writes; }, services);
            AetherPDInstallationTestAccess::inject(model);
            AetherPDTest::PackageFixture fixture;
            QVERIFY(model.recoverFirstInstallation(fixture.trust.candidateKey, f.model.controlSession())); QVERIFY(loaded);
            if (mode == 0) { model.cancel(); }
            if (mode == 1) { RadioSetupControlTestAccess::replaceConnectionGeneration(f.model); model.reconcile(); }
            loaded(mode == 2 ? QString(64, u'b') : QString(64, u'a'));
            if (mode == 3) { model.cancel(); }
            if (mode == 4) { RadioSetupControlTestAccess::replaceConnectionGeneration(f.model); model.reconcile(); }
            QCOMPARE(writes, 0); QVERIFY(f.transfers.isEmpty());
            QCOMPARE(f.network.sent.size(), mode < 3 ? std::size_t(0) : std::size_t(1));
            if (mode >= 3) { QVERIFY(f.network.sent.front().reply->isFinished()); }
        }
    }

    void compiledDevelopmentSelectionRequiresExplicitPreparation()
    {
        Fixture f;
        int loads = 0;
        AetherPDInstallationModel model(f.model, [&](const QString&, QObject*, auto) { ++loads; }, {});
        AetherPDInstallationTestAccess::inject(model);
        const bool selected = !model.selectedDevelopmentVersion().isEmpty();
        QCOMPARE(model.prepareSelectedDevelopment(f.model.controlSession()), selected);
        if (!selected) {
            QCOMPARE(model.state(), AetherPDInstallationModel::State::Idle);
        } else {
            QVERIFY(!model.prepareSelectedDevelopment(f.model.controlSession()));
            QTRY_COMPARE_WITH_TIMEOUT(model.state(), AetherPDInstallationModel::State::AwaitingApproval, 30000);
            QVERIFY(model.approvedPlan().startsWith("PDUPGR02"));
            model.cancel();
            QVERIFY(model.prepareSelectedDevelopment(f.model.controlSession()));
            model.cancel();
            QThreadPool::globalInstance()->waitForDone();
            QCoreApplication::processEvents();
            QCOMPARE(model.state(), AetherPDInstallationModel::State::Finished);
            QVERIFY(model.prepareSelectedDevelopment(f.model.controlSession()));
            RadioSetupControlTestAccess::replaceConnectionGeneration(f.model);
            model.reconcile();
            QThreadPool::globalInstance()->waitForDone();
            QCoreApplication::processEvents();
            QCOMPARE(model.failure(), AetherPDInstallationModel::Failure::SessionChanged);
        }
        QCOMPARE(loads, 0); QVERIFY(f.network.sent.empty()); QVERIFY(f.transfers.isEmpty());
    }
    void catalogueUpgradeChecksBeforeDownloadAndAgainAfterExactApproval()
    {
        for (int mode = 0; mode < 4; ++mode) {
            Fixture f; AetherPDTest::PackageFixture fixture;
            AetherPDTest::Network inventory, downloads;
            const auto entry = upgradeEntry(fixture);
            const AetherPDReleaseCatalogue catalogue({}, {entry}); QVERIFY(catalogue.valid());
            int loads = 0, creates = 0;
            std::function<void(const QString&)> loaded;
            AetherPDInstallationModel model(f.model, [&](const QString&, QObject*, auto done) { ++loads; loaded = std::move(done); },
                [&](RadioModel& radio) { ++creates; return std::make_unique<Coordinator>(radio, f.network, f.channel, f.archive, [&] { return f.clock; }); },
                nullptr, [&] { return std::make_unique<AetherPDPackageDownload>(downloads); }, {}, {}, catalogue,
                [&] { return std::make_unique<AetherPDInstallationRead>(inventory, [&] { return f.clock; }); });
            AetherPDInstallationTestAccess::inject(model);
            QVERIFY(model.updateAvailable()); QVERIFY(!model.selectedUpdateIsDevelopment());
            QVERIFY(model.prepareSelectedUpdate(f.model.controlSession())); QCOMPARE(loads, 1);
            QVERIFY(inventory.sent.empty()); QVERIFY(downloads.sent.empty()); QVERIFY(!model.approve(model.approvedPlan()));
            loaded(QString(64, u'a')); QCOMPARE(inventory.sent.size(), std::size_t(1));
            QCOMPARE(inventory.sent[0].method, QNetworkAccessManager::GetOperation);
            inventory.last->complete(200, installedRecord(entry, mode == 0));
            for (int i = 0; i < 8; ++i) { downloads.last->complete(200, fixture.bytes(i)); }
            QTRY_COMPARE(model.state(), AetherPDInstallationModel::State::AwaitingApproval);
            QCOMPARE(model.selectedUpdateVersion(), entry.release.version); QCOMPARE(creates, 0); QVERIFY(f.network.sent.empty());
            QVERIFY(!model.approve(model.approvedPlan() + 'x')); QCOMPARE(loads, 1);
            QVERIFY(model.approve(model.approvedPlan())); QCOMPARE(loads, 2); QCOMPARE(creates, 0);
            loaded(QString(64, mode == 2 ? u'b' : u'a'));
            if (mode == 2) {
                QCOMPARE(model.failure(), AetherPDInstallationModel::Failure::SessionChanged);
                QCOMPARE(inventory.sent.size(), std::size_t(1));
            } else {
                QCOMPARE(model.state(), AetherPDInstallationModel::State::ReadingInstallation);
                QCOMPARE(inventory.sent.size(), std::size_t(2)); QCOMPARE(creates, 0); QVERIFY(f.network.sent.empty());
                if (mode == 3) { model.cancel(); inventory.last->complete(200, installedRecord(entry)); }
                else if (mode == 1) {
                    auto changed = entry; changed.previousDigest = QByteArray(32, 'x');
                    inventory.last->complete(200, installedRecord(changed));
                    QCOMPARE(model.failure(), AetherPDInstallationModel::Failure::NoMatchingRelease);
                } else {
                    inventory.last->complete(200, installedRecord(entry));
                    QCOMPARE(creates, 1); QCOMPARE(f.network.sent.size(), std::size_t(1));
                    QCOMPARE(f.network.sent[0].method, QNetworkAccessManager::PostOperation);
                    model.cancel();
                }
            }
            if (mode != 0) { QCOMPARE(creates, 0); QVERIFY(f.network.sent.empty()); }
            QVERIFY(f.transfers.isEmpty()); QCOMPARE(f.archive.starts, 0);
        }
    }
    void catalogueUpgradeRefusesUnknownInventoryAndMetadataBeforeInstallation()
    {
        for (int mode = 0; mode < 6; ++mode) {
            Fixture f; AetherPDTest::PackageFixture fixture; AetherPDTest::Network inventory, downloads;
            auto entry = upgradeEntry(fixture); if (mode == 5) { entry.candidateDigest = QByteArray(32, 'x'); }
            int creates = 0;
            AetherPDInstallationModel model(f.model, [](const QString&, QObject*, auto done) { done(QString(64, u'a')); },
                [&](RadioModel&) -> std::unique_ptr<Coordinator> { ++creates; return {}; }, nullptr,
                [&] { return std::make_unique<AetherPDPackageDownload>(downloads); }, {}, {}, AetherPDReleaseCatalogue({}, {entry}),
                [&] { return std::make_unique<AetherPDInstallationRead>(inventory, [&] { return f.clock; }); });
            AetherPDInstallationTestAccess::inject(model);
            QVERIFY(model.prepareSelectedUpdate(f.model.controlSession()));
            if (mode == 0) { inventory.last->complete(401, {}); }
            if (mode == 1) { inventory.last->complete(503, {}); }
            if (mode == 2) {
                auto other = entry; other.previousDigest = QByteArray(32, 'p'); inventory.last->complete(200, installedRecord(other));
            }
            if (mode == 3) {
                auto object = QJsonDocument::fromJson(installedRecord(entry)).object(); object["serial"] = "different-radio";
                inventory.last->complete(200, QJsonDocument(object).toJson());
            }
            if (mode == 4) {
                RadioSetupControlTestAccess::replaceConnectionGeneration(f.model);
                inventory.last->complete(200, installedRecord(entry));
            }
            if (mode == 5) {
                inventory.last->complete(200, installedRecord(entry));
                for (int i = 0; i < 8; ++i) { downloads.last->complete(200, fixture.bytes(i)); }
            }
            QCOMPARE(model.state(), AetherPDInstallationModel::State::Unavailable);
            if (mode < 5) { QVERIFY(downloads.sent.empty()); }
            QCOMPARE(creates, 0); QVERIFY(f.network.sent.empty()); QVERIFY(f.transfers.isEmpty());
        }
    }
    void lateInventoryCredentialCannotEnterReplacementPreparation()
    {
        Fixture f; AetherPDTest::PackageFixture fixture; AetherPDTest::Network inventory;
        const auto entry = upgradeEntry(fixture);
        std::function<void(const QString&)> loaded;
        AetherPDInstallationModel model(f.model, [&](const QString&, QObject*, auto done) { loaded = std::move(done); }, {},
            nullptr, {}, {}, {}, AetherPDReleaseCatalogue({}, {entry}),
            [&] { return std::make_unique<AetherPDInstallationRead>(inventory, [&] { return f.clock; }); });
        AetherPDInstallationTestAccess::inject(model);
        QVERIFY(model.prepareSelectedUpdate(f.model.controlSession())); const auto old = loaded;
        model.cancel(); QVERIFY(model.prepareSelectedUpdate(f.model.controlSession()));
        old(QString(64, u'b'));
        QCOMPARE(model.state(), AetherPDInstallationModel::State::LoadingInventoryCredential); QVERIFY(inventory.sent.empty());
        loaded(QString(64, u'a')); QCOMPARE(model.state(), AetherPDInstallationModel::State::ReadingInstallation);
        QCOMPARE(inventory.sent.size(), std::size_t(1)); model.cancel();
    }
    void inventoryCancelAndReentrantDeletionCannotStartDownload()
    {
        for (int mode = 0; mode < 3; ++mode) {
            Fixture f; AetherPDTest::PackageFixture fixture; AetherPDTest::Network inventory, downloads;
            const auto entry = upgradeEntry(fixture);
            std::function<void(const QString&)> loaded;
            auto model = std::make_unique<AetherPDInstallationModel>(f.model,
                [&](const QString&, QObject*, auto done) { loaded = std::move(done); }, AetherPDInstallationModel::Factory{}, nullptr,
                [&] { return std::make_unique<AetherPDPackageDownload>(downloads); }, AetherPDInstallationModel::TokenWriter{},
                AetherPDInstallationModel::RecoveryServices{}, AetherPDReleaseCatalogue({}, {entry}),
                [&] { return std::make_unique<AetherPDInstallationRead>(inventory, [&] { return f.clock; }); });
            AetherPDInstallationTestAccess::inject(*model);
            QVERIFY(model->prepareSelectedUpdate(f.model.controlSession()));
            if (mode == 0) { model->cancel(); loaded(QString(64, u'a')); QVERIFY(inventory.sent.empty()); }
            else {
                loaded(QString(64, u'a'));
                if (mode == 1) { model->cancel(); }
                else {
                    connect(model.get(), &AetherPDInstallationModel::changed, &f.model, [&] {
                        if (model && model->state() == AetherPDInstallationModel::State::Downloading) { model.reset(); }
                    });
                }
                inventory.last->complete(200, installedRecord(entry));
                if (mode == 2) { QVERIFY(!model); }
            }
            QVERIFY(downloads.sent.empty()); QVERIFY(f.network.sent.empty()); QVERIFY(f.transfers.isEmpty());
        }
    }
    void catalogueFirstDownloadMatchesHardwareAndSignedRelease()
    {
        for (int mode = 0; mode < 4; ++mode) {
            Fixture f; AetherPDTest::PackageFixture fixture;
            const auto package = fixture.loadFirst(); QVERIFY(package);
            AetherPDTest::Network downloads;
            auto services = journalServices();
            services.read = [](const QByteArray&) { return AetherPDInstallationJournal::Read{AetherPDInstallationJournal::State::Missing, {}}; };
            const QByteArray digest = mode == 2 ? QByteArray(32, 'x') : package->candidateDigest();
            AetherPDReleaseCatalogue catalogue({{"FLEX-8600", firstRelease(fixture), digest, true}});
            int writes = 0;
            AetherPDInstallationModel model(f.model, {}, {}, nullptr,
                [&] { return std::make_unique<AetherPDPackageDownload>(downloads); },
                [&](const QString&, const QString&, const QString&, QObject*, auto) { ++writes; }, services, catalogue);
            AetherPDInstallationTestAccess::inject(model);
            QVERIFY(!model.selectedFirstIsDevelopment());
            if (mode == 0) {
                f.backend->setModelProvider([] { return QStringLiteral("FLEX-6300"); });
                QVERIFY(model.selectedFirstVersion().isEmpty());
                QVERIFY(!model.prepareSelectedFirstInstallation(f.model.controlSession())); QVERIFY(downloads.sent.empty());
            } else {
                QCOMPARE(model.selectedFirstVersion(), QStringLiteral("0.1.0-test"));
                QVERIFY(model.prepareSelectedFirstInstallation(f.model.controlSession()));
                if (mode == 3) { f.backend->setModelProvider([] { return QStringLiteral("FLEX-6300"); }); }
                if (mode == 3) {
                    downloads.last->complete(200, fixture.firstFiles()[0]);
                    QCOMPARE(model.failure(), AetherPDInstallationModel::Failure::SessionChanged);
                } else {
                    for (const auto& file : fixture.firstFiles()) { downloads.last->complete(200, file); }
                    if (mode == 2) { QCOMPARE(model.failure(), AetherPDInstallationModel::Failure::PackageRejected); }
                    else {
                        QTRY_COMPARE(model.state(), AetherPDInstallationModel::State::AwaitingApproval);
                        const auto preview = model.preview(); QVERIFY(preview);
                        QCOMPARE(preview->candidateDigest, QString::fromLatin1(package->candidateDigest().toHex()));
                    }
                }
            }
            QCOMPARE(writes, 0); QVERIFY(f.transfers.isEmpty()); QVERIFY(f.network.sent.empty());
        }
    }
    void catalogueSavedDownloadReusesExactIdentityOrRejectsChangedJournal()
    {
        for (bool changed : {false, true}) {
            Fixture f; AetherPDTest::PackageFixture fixture;
            const auto package = fixture.loadFirst(); const auto prepared = prepareFirst(); QVERIFY(package); QVERIFY(prepared);
            QByteArray journal = prepared->approvedPlan();
            AetherPDTest::Network downloads;
            int loads = 0, writes = 0;
            std::function<void(const QString&)> loaded;
            auto services = journalServices();
            services.read = [&](const QByteArray& key) {
                if (key != fixture.trust.candidateKey) { return AetherPDInstallationJournal::Read{}; }
                return AetherPDInstallationJournal::Read{AetherPDInstallationJournal::State::Pending, journal};
            };
            services.load = [&](const QString&, const QString&, QObject*, auto done) { ++loads; loaded = std::move(done); };
            services.retainPending = [](const QByteArray&, const QByteArray&) { return true; };
            const AetherPDReleaseCatalogue catalogue({{"FLEX-8600", firstRelease(fixture), package->candidateDigest(), false}});
            AetherPDInstallationModel model(f.model, {}, {}, nullptr,
                [&] { return std::make_unique<AetherPDPackageDownload>(downloads); },
                [&](const QString&, const QString&, const QString&, QObject*, auto) { ++writes; }, services, catalogue);
            AetherPDInstallationTestAccess::inject(model);
            QVERIFY(model.selectedFirstVersion().isEmpty()); QVERIFY(!model.firstInstallationAvailable());
            QVERIFY(model.savedFirstInstallationAvailable()); QVERIFY(model.savedFirstPreparationAvailable());
            QVERIFY(model.prepareSelectedSavedFirstInstallation(f.model.controlSession()));
            QCOMPARE(loads, 0); QCOMPARE(model.state(), AetherPDInstallationModel::State::Downloading);
            if (changed) { const auto replacement = prepareFirst(); QVERIFY(replacement); journal = replacement->approvedPlan(); }
            for (const auto& file : fixture.firstFiles()) { downloads.last->complete(200, file); }
            if (changed) {
                QCOMPARE(model.failure(), AetherPDInstallationModel::Failure::PackageRejected); QCOMPARE(loads, 0);
            } else {
                QCOMPARE(loads, 1); loaded(QString(64, u'a'));
                QTRY_COMPARE(model.state(), AetherPDInstallationModel::State::AwaitingApproval);
                QVERIFY(model.preparingSavedInstallation()); QCOMPARE(model.approvedPlan(), prepared->approvedPlan());
                const auto preview = model.preview(); QVERIFY(preview); QCOMPARE(preview->nativeName, prepared->nativeName());
                QCOMPARE(preview->carrierName, prepared->carrierName());
            }
            QCOMPARE(writes, 0); QVERIFY(f.transfers.isEmpty()); QVERIFY(f.network.sent.empty());
        }
    }
    void retainedPublisherRecoversSavedAttemptAfterNewPublisherIsOffered()
    {
        Fixture f; AetherPDTest::PackageFixture fixture;
        const auto package = fixture.loadFirst(); const auto prepared = prepareFirst(); QVERIFY(package); QVERIFY(prepared);
        const auto oldRelease = firstRelease(fixture);
        auto newer = oldRelease; newer.version = "0.2.0"; newer.trust.publisherKey = QByteArray(32, 'n');
        const AetherPDReleaseCatalogue catalogue({{"FLEX-8600", oldRelease, package->candidateDigest(), false},
            {"FLEX-8600", newer, QByteArray(32, 'n'), true}});
        int writes = 0; QString loadedAttempt;
        std::function<void(const QString&)> loaded;
        auto services = journalServices();
        services.read = [&](const QByteArray& key) {
            return key == fixture.trust.candidateKey
                ? AetherPDInstallationJournal::Read{AetherPDInstallationJournal::State::Pending, prepared->approvedPlan()}
                : AetherPDInstallationJournal::Read{AetherPDInstallationJournal::State::Invalid, {}};
        };
        services.load = [&](const QString&, const QString& attempt, QObject*, auto done) { loadedAttempt = attempt; loaded = std::move(done); };
        services.monitor = [&] { return std::make_unique<AetherPDInstallationMonitor>(f.network, [&] { return f.clock; }); };
        AetherPDInstallationModel model(f.model, {}, {}, nullptr, {},
            [&](const QString&, const QString&, const QString&, QObject*, auto) { ++writes; }, services, catalogue);
        AetherPDInstallationTestAccess::inject(model);
        QCOMPARE(model.selectedFirstVersion(), QStringLiteral("0.2.0")); QVERIFY(!model.firstInstallationAvailable());
        QVERIFY(model.savedFirstInstallationAvailable()); QVERIFY(model.recoverSelectedFirstInstallation(f.model.controlSession()));
        const auto preview = model.preview(); QVERIFY(preview);
        QCOMPARE(preview->candidateKey, QString::fromLatin1(fixture.trust.candidateKey.toHex()));
        QCOMPARE(preview->plan, prepared->approvedPlan()); QCOMPARE(loadedAttempt, preview->planDigest);
        loaded(QString(64, u'a'));
        QCOMPARE(model.state(), AetherPDInstallationModel::State::CheckingInstallation);
        QCOMPARE(f.network.sent.size(), std::size_t(1));
        QCOMPARE(f.network.sent[0].method, QNetworkAccessManager::GetOperation);
        QCOMPARE(f.network.sent[0].request.url().path(), QStringLiteral("/v1/aetherpd/installation"));
        QCOMPARE(writes, 0); QVERIFY(f.transfers.isEmpty()); model.cancel();
    }
    void uncertainFirstJournalCannotStartAnotherIdentity()
    {
        Fixture f; AetherPDTest::PackageFixture fixture;
        const auto package = fixture.loadFirst(); QVERIFY(package);
        const AetherPDReleaseCatalogue catalogue({{"FLEX-8600", firstRelease(fixture), package->candidateDigest(), true}});
        for (const auto state : {AetherPDInstallationJournal::State::Invalid, AetherPDInstallationJournal::State::Unavailable,
                                AetherPDInstallationJournal::State::Pending, AetherPDInstallationJournal::State::CredentialSaved}) {
            auto services = journalServices(); services.read = [state](const QByteArray&) { return AetherPDInstallationJournal::Read{state, {}}; };
            AetherPDTest::Network downloads;
            AetherPDInstallationModel model(f.model, {}, {}, nullptr, [&] { return std::make_unique<AetherPDPackageDownload>(downloads); },
                [](const QString&, const QString&, const QString&, QObject*, auto) {}, services, catalogue);
            AetherPDInstallationTestAccess::inject(model);
            QVERIFY(!model.firstInstallationAvailable());
            QVERIFY(!model.prepareSelectedFirstInstallation(f.model.controlSession())); QVERIFY(downloads.sent.empty());
        }
    }
    void firstDownloadNeedsExactPlanApprovalBeforeCredentialOrRadioEffects()
    {
        Fixture f; AetherPDTest::PackageFixture fixture;
        AetherPDTest::Network downloads;
        int loads = 0, writes = 0, creates = 0;
        std::function<void(bool)> saved;
        QByteArray savedHash;
        AetherPDInstallationModel model(f.model, [&](const QString&, QObject*, auto) { ++loads; },
            [&](RadioModel& radio) { ++creates; return std::make_unique<Coordinator>(radio, f.network, f.channel, f.archive, [&] { return f.clock; }); },
            nullptr, [&] { return std::make_unique<AetherPDPackageDownload>(downloads); },
            [&](const QString& serial, const QString& attempt, const QString& token, QObject*, auto done) {
                ++writes; QCOMPARE(serial, QStringLiteral("fixture-8600")); QVERIFY(!attempt.isEmpty());
                savedHash = QCryptographicHash::hash(token.toLatin1(), QCryptographicHash::Sha256); saved = std::move(done);
            }, journalServices());
        AetherPDInstallationTestAccess::inject(model);
        QVERIFY(model.prepareFirstRelease(firstRelease(fixture), "fixture-radio", f.model.controlSession()));
        QCOMPARE(model.state(), AetherPDInstallationModel::State::Downloading);
        QVERIFY(!model.approve(model.approvedPlan()));
        for (const auto& file : fixture.firstFiles()) { downloads.last->complete(200, file); }
        QTRY_COMPARE(model.state(), AetherPDInstallationModel::State::AwaitingApproval);
        const auto preview = model.preview(); QVERIFY(preview); QVERIFY(preview->firstInstallation);
        QVERIFY(preview->previousDigest.isEmpty()); QCOMPARE(preview->hostname, QStringLiteral("fixture-radio"));
        QCOMPARE(model.downloadReceived(), model.downloadTotal()); QVERIFY(model.downloadTotal() > 0);
        QCOMPARE(writes, 0); QCOMPARE(loads, 0); QCOMPARE(creates, 0);
        QVERIFY(f.network.sent.empty()); QVERIFY(f.transfers.isEmpty()); QCOMPARE(f.archive.nativeStarts, 0);
        QVERIFY(!model.approve(preview->plan + 'x')); QCOMPARE(writes, 0);
        QVERIFY(model.approve(preview->plan)); QCOMPARE(writes, 1); QCOMPARE(savedHash, preview->plan.mid(148, 32));
        QCOMPARE(model.state(), AetherPDInstallationModel::State::SavingCredential);
        QVERIFY(f.network.sent.empty()); QVERIFY(f.transfers.isEmpty());
        saved(false); QCOMPARE(model.failure(), AetherPDInstallationModel::Failure::CredentialSaveFailed);
        QCOMPARE(creates, 0); QCOMPARE(loads, 0); QCOMPARE(f.archive.nativeStarts, 0);
    }
    void firstDownloadRejectsChangedSessionCancelAndCorruptionWithoutEffects()
    {
        for (int mode = 0; mode < 5; ++mode) {
            Fixture f; AetherPDTest::PackageFixture fixture;
            AetherPDTest::Network downloads;
            int writes = 0;
            AetherPDInstallationModel model(f.model, {}, {}, nullptr,
                [&] { return std::make_unique<AetherPDPackageDownload>(downloads); },
                [&](const QString&, const QString&, const QString&, QObject*, auto) { ++writes; }, journalServices());
            AetherPDInstallationTestAccess::inject(model);
            const auto selection = firstRelease(fixture);
            QVERIFY(model.prepareFirstRelease(selection, "fixture-radio", f.model.controlSession()));
            const QPointer<AetherPDTest::Reply> old = downloads.last;
            const auto files = fixture.firstFiles();
            if (mode == 0) {
                model.cancel(); old->complete(200, files[0]);
                QCOMPARE(model.state(), AetherPDInstallationModel::State::Finished); QVERIFY(old->aborted);
            } else if (mode == 1) {
                RadioSetupControlTestAccess::replaceConnectionGeneration(f.model);
                old->complete(200, files[0]);
                QCOMPARE(model.failure(), AetherPDInstallationModel::Failure::SessionChanged);
            } else if (mode == 2) {
                old->complete(200, files[0] + 'x');
                QCOMPARE(model.failure(), AetherPDInstallationModel::Failure::DownloadFailed);
            } else {
                for (const auto& file : files) { downloads.last->complete(200, file); }
                QCOMPARE(model.state(), AetherPDInstallationModel::State::Preparing);
                if (mode == 3) { model.cancel(); }
                else { RadioSetupControlTestAccess::replaceConnectionGeneration(f.model); model.reconcile(); }
                QVERIFY(QThreadPool::globalInstance()->waitForDone(5000)); QCoreApplication::processEvents();
                QVERIFY(model.state() != AetherPDInstallationModel::State::AwaitingApproval);
                if (mode == 4) { QCOMPARE(model.failure(), AetherPDInstallationModel::Failure::SessionChanged); }
            }
            QCOMPARE(writes, 0); QVERIFY(f.network.sent.empty()); QVERIFY(f.transfers.isEmpty()); QCOMPARE(f.archive.nativeStarts, 0);
        }
    }
    void firstDownloadInvalidTargetAndMissingIdentityServicesStartNothing()
    {
        Fixture f; AetherPDTest::PackageFixture fixture;
        AetherPDTest::Network downloads;
        const auto factory = [&] { return std::make_unique<AetherPDPackageDownload>(downloads); };
        const AetherPDInstallationModel::TokenWriter writer = [](const QString&, const QString&, const QString&, QObject*, auto) {};
        AetherPDInstallationModel missingWriter(f.model, {}, {}, nullptr, factory, {}, journalServices());
        AetherPDInstallationTestAccess::inject(missingWriter);
        AetherPDInstallationModel missingJournal(f.model, {}, {}, nullptr, factory, writer);
        AetherPDInstallationTestAccess::inject(missingJournal);
        const auto selection = firstRelease(fixture);
        QVERIFY(!missingWriter.prepareFirstRelease(selection, "fixture-radio", f.model.controlSession()));
        QVERIFY(!missingJournal.prepareFirstRelease(selection, "fixture-radio", f.model.controlSession()));
        AetherPDInstallationModel model(f.model, {}, {}, nullptr, factory, writer, journalServices());
        AetherPDInstallationTestAccess::inject(model);
        auto bad = selection; bad.firmware = "4.0.0.0";
        QVERIFY(!model.prepareFirstRelease(bad, "fixture-radio", f.model.controlSession()));
        bad = selection; bad.profile = 2;
        QVERIFY(!model.prepareFirstRelease(bad, "fixture-radio", f.model.controlSession()));
        QVERIFY(!model.prepareFirstRelease(selection, {}, f.model.controlSession()));
        QVERIFY(downloads.sent.empty()); QVERIFY(f.transfers.isEmpty());
    }
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
        AetherPDInstallationTestAccess::inject(model);
        QVERIFY(model.prepareRelease(release, f.model.controlSession()));
        QVERIFY(!model.approve(model.approvedPlan())); QVERIFY(!model.prepareRelease(release, f.model.controlSession()));
        for (int i = 0; i < 8; ++i) { downloads.last->complete(200, fixture.bytes(i)); }
        QTRY_COMPARE(model.state(), AetherPDInstallationModel::State::AwaitingApproval);
        QVERIFY(model.approvedPlan().startsWith("PDUPGR02"));
        QCOMPARE(model.downloadReceived(), model.downloadTotal());
        QCOMPARE(loads, 0); QVERIFY(f.network.sent.empty()); QVERIFY(f.transfers.isEmpty());
        QVERIFY(model.approve(model.approvedPlan())); QCOMPARE(loads, 1); model.cancel();
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
        AetherPDInstallationTestAccess::inject(model);
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
        AetherPDInstallationTestAccess::inject(model);
        QVERIFY(model.offer(*package, f.model.controlSession()));
        QCOMPARE(loads, 0); QVERIFY(f.network.sent.empty());
        QCOMPARE(model.state(), AetherPDInstallationModel::State::AwaitingApproval);
        QVERIFY(model.approve(model.approvedPlan())); QVERIFY(!model.approve(model.approvedPlan())); QCOMPARE(loads, 1);
        QVERIFY(!AetherPDInstallationTestAccess::acquire(QStringLiteral("fixture-8600")));
        model.cancel(); deliver(QString(43, u'a'));
        QVERIFY(AetherPDInstallationTestAccess::acquire(QStringLiteral("fixture-8600")));
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
        AetherPDInstallationTestAccess::inject(model);
        QVERIFY(model.offer(*package, f.model.controlSession())); QVERIFY(model.approve(model.approvedPlan()));
        RadioSetupControlTestAccess::replaceConnectionGeneration(f.model);
        deliver(QString(43, u'a'));
        QCOMPARE(model.failure(), AetherPDInstallationModel::Failure::SessionChanged);
        QCOMPARE(creates, 0); QVERIFY(f.network.sent.empty());
        QVERIFY(model.offer(*package, f.model.controlSession())); QVERIFY(model.approve(model.approvedPlan()));
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
        AetherPDInstallationTestAccess::inject(model);
        QVERIFY(model.offer(*package, f.model.controlSession())); QVERIFY(model.approve(model.approvedPlan())); deliver({});
        QCOMPARE(model.failure(), AetherPDInstallationModel::Failure::NoCredential);
        QVERIFY(model.offer(*package, f.model.controlSession())); QVERIFY(model.approve(model.approvedPlan()));
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
        AetherPDInstallationTestAccess::inject(model);
        QVERIFY(model.offer(*package, f.model.controlSession())); QVERIFY(model.approve(model.approvedPlan()));
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
        AetherPDInstallationTestAccess::inject(model);
        QVERIFY(model.offer(*package, f.model.controlSession())); QVERIFY(model.approve(model.approvedPlan())); QVERIFY(job);
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
        AetherPDInstallationTestAccess::inject(model);
        QVERIFY(model.offer(*package, f.model.controlSession())); QVERIFY(model.approve(model.approvedPlan()));
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
        QVERIFY(done.isEmpty());
        { const auto reply = f.transferReplies[f.transfers.indexOf("waveform remove_container " + package->carrierName())]; reply(0, {}); }
        f.coordinator.poll();
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
