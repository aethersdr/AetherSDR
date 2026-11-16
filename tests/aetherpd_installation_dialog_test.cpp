#include "RadioSetupControlFixture.h"
#include "AetherPDPackageFixture.h"
#include "AetherPDTestTransport.h"
#include "gui/AetherPDDialog.h"
#include "models/AetherPDInstallationModel.h"
#include "core/backends/AetherPDCarrierUpload.h"
#include "core/AetherPDInstallationMonitor.h"
#include <QLabel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QtTest>
#include <functional>
#include <memory>

using namespace AetherSDR;
void RadioSetupControlTestAccess::installationModel(RadioModel& radio, std::unique_ptr<AetherPDInstallationModel> model)
{ radio.m_aetherPDInstallationModel = std::move(model); }
namespace {
using Coordinator = AetherPDInstallationCoordinator;
class Carrier final : public AetherPDCarrierUpload {
public:
    void begin(const QString&, const RadioControlSession&, const FileTransferHandle&) override {}
    void cancel() override {}
};
class Channel final : public AetherPDInstallChannel {
public:
    void connectTo(const QHostAddress&) override {}
    qint64 write(const char*, qint64) override { return -1; }
    qint64 pendingBytes() const override { return 0; }
    qint64 availableBytes() const override { return 0; }
    QByteArray read(qint64) override { return {}; }
    bool finishWrites() override { return false; }
    void close() override {}
};
struct Fixture {
    AetherPDTest::Network network;
    Channel channel;
    Carrier carrier;
    RadioModel radio;
    RadioSetupTestBackend* backend{installRadioSetupBackend(radio)};
    AetherPDTest::PackageFixture files;
    int loads{0}, writes{0}, recoveryLoads{0};
    QByteArray journalPlan;
    std::function<void(const QString&)> recoveryLoaded;
    std::function<void(bool)> saved;
    std::function<void(const QString&)> credential;
    QPointer<Coordinator> job;
    std::unique_ptr<AetherPDDialog> dialog;
    Fixture()
    {
        RadioSetupControlTestAccess::peer(radio, QHostAddress(QStringLiteral("192.168.50.100")));
        RadioSetupControlTestAccess::identity(radio, "fixture-8600", "4.3.16.42034");
        backend->setModelProvider([] { return QStringLiteral("FLEX-8600"); });
        radio.handleLineForTest("S0|waveform wfp_status power=on ready=true");
        auto model = std::make_unique<AetherPDInstallationModel>(radio,
            [this](const QString&, QObject*, auto done) { ++loads; credential = std::move(done); },
            [this](RadioModel& model) {
                auto result = std::make_unique<Coordinator>(model, network, channel, carrier, [] { return qint64(0); });
                job = result.get(); return result;
            }, nullptr, AetherPDInstallationModel::DownloadFactory{}, [this](const QString&, const QString&, const QString&, QObject*, auto done) {
                ++writes; saved = std::move(done);
            }, AetherPDInstallationModel::RecoveryServices{
                [](const QByteArray&, const QByteArray&, bool) { return true; },
                [this](const QByteArray& key) {
                    if (key != files.trust.candidateKey) { return AetherPDInstallationJournal::Read{AetherPDInstallationJournal::State::Invalid, {}}; }
                    return AetherPDInstallationJournal::Read{journalPlan.isEmpty() ? AetherPDInstallationJournal::State::Missing
                        : AetherPDInstallationJournal::State::Pending, journalPlan};
                },
                [this](const QString&, const QString&, QObject*, auto done) { ++recoveryLoads; recoveryLoaded = std::move(done); },
                [this] { return std::make_unique<AetherPDInstallationMonitor>(network, [] { return qint64(0); }); }
            });
        RadioSetupControlTestAccess::installationModel(radio, std::move(model));
        open();
    }
    void open()
    {
        // Status attachment is separate from installer credential approval.
        dialog = std::make_unique<AetherPDDialog>(&radio, [](const QString&, QObject*, auto) {});
    }
    AetherPDInstallationModel& model() { return radio.aetherPDInstallationModel(); }
    bool offer()
    {
        const auto package = files.load();
        const auto prepared = package ? Coordinator::prepare(*package, "fixture-8600") : std::nullopt;
        return prepared && model().offer(*prepared, radio.controlSession());
    }
    QPushButton* button(const char* name) { return dialog->findChild<QPushButton*>(QString::fromLatin1(name)); }
    QString state() { return dialog->findChild<QLabel*>(QStringLiteral("pdInstallState"))->text(); }
    QString preview() { return dialog->findChild<QPlainTextEdit*>(QStringLiteral("pdInstallPlan"))->toPlainText(); }
};
}
class InstallationDialogTest : public QObject {
    Q_OBJECT
private slots:
    void firstInstallationPreviewAndCredentialFailure()
    {
        Fixture f; const auto package = f.files.loadFirst(); QVERIFY(package);
        QVERIFY(f.model().prepareFirstPackage(*package, "fixture-radio", f.radio.controlSession()));
        QTRY_COMPARE(f.model().state(), AetherPDInstallationModel::State::AwaitingApproval);
        QVERIFY(f.preview().contains("First installation"));
        QVERIFY(f.preview().contains("host-root authority"));
        QVERIFY(f.preview().contains("Fixed boot/session helpers remain installed"));
        QVERIFY(!f.preview().contains("retaining its Remote Access identity"));
        QCOMPARE(f.loads, 0); QCOMPARE(f.writes, 0); QVERIFY(f.network.sent.empty());
        QTest::mouseClick(f.button("pdInstallApprove"), Qt::LeftButton);
        QCOMPARE(f.writes, 1); QCOMPARE(f.loads, 0);
        QVERIFY(f.state().contains("system keychain before installation"));
        QVERIFY(!f.button("pdInstallApprove")->isEnabled());
        QVERIFY(f.button("pdInstallCancel")->isEnabled());
        f.saved(false);
        QVERIFY(f.state().contains("no installation was started"));
        QVERIFY(f.network.sent.empty()); QVERIFY(!f.job);
    }

    void firstPrepareRequiresCompiledPinsAndExplicitApproval()
    {
        Fixture f;
        QVERIFY(!f.button("pdInstallCheck")->isEnabled());
        if (qEnvironmentVariableIsSet("AETHERPD_EXPECT_FIRST_SELECTION")) { QVERIFY(!f.model().selectedFirstDevelopmentVersion().isEmpty()); }
        if (f.model().selectedFirstDevelopmentVersion().isEmpty()) {
            QVERIFY(!f.button("pdInstallFirst")->isEnabled());
            QVERIFY(!f.button("pdInstallFirst")->accessibleDescription().isEmpty());
            return;
        }
        QVERIFY(f.button("pdInstallFirst")->isEnabled());
        QTest::mouseClick(f.button("pdInstallFirst"), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(f.model().state(), AetherPDInstallationModel::State::AwaitingApproval, 30000);
        QVERIFY(f.preview().contains("First installation"));
        QVERIFY(f.preview().contains("not qualified for production"));
        QCOMPARE(f.button("pdInstallApprove")->text(), QStringLiteral("Approve first installation"));
        const QString screenshot = QString::fromLocal8Bit(qgetenv("AETHERPD_FIRST_PANEL_SCREENSHOT"));
        if (!screenshot.isEmpty()) {
            f.dialog->show(); QCoreApplication::processEvents();
            QVERIFY(f.dialog->grab().save(screenshot));
        }
        QVERIFY(!f.button("pdInstallFirst")->isEnabled());
        QCOMPARE(f.loads, 0); QCOMPARE(f.writes, 0); QVERIFY(f.network.sent.empty());
        QTest::mouseClick(f.button("pdInstallCancel"), Qt::LeftButton);
        QCOMPARE(f.model().outcome(), Coordinator::Outcome::Canceled);
        QCOMPARE(f.loads, 0); QCOMPARE(f.writes, 0); QVERIFY(f.network.sent.empty());
    }

    void savedInstallationButtonOnlyObservesThePinnedAttempt()
    {
        Fixture f; const auto package = f.files.loadFirst(); QVERIFY(package);
        const auto prepared = Coordinator::prepareFirst(*package, "fixture-8600",
            QCryptographicHash::hash(QByteArray(64, 'a'), QCryptographicHash::Sha256), "fixture-radio"); QVERIFY(prepared);
        f.journalPlan = prepared->approvedPlan(); f.dialog.reset(); f.open();
        if (qEnvironmentVariableIsSet("AETHERPD_EXPECT_FIRST_SELECTION")) { QVERIFY(f.model().savedFirstInstallationAvailable()); }
        if (!f.model().savedFirstInstallationAvailable()) {
            QVERIFY(!f.button("pdInstallCheck")->isEnabled());
            return;
        }
        QVERIFY(f.button("pdInstallCheck")->isEnabled()); QVERIFY(!f.button("pdInstallFirst")->isEnabled());
        QTest::mouseClick(f.button("pdInstallCheck"), Qt::LeftButton);
        QCOMPARE(f.recoveryLoads, 1); QCOMPARE(f.loads, 0); QCOMPARE(f.writes, 0);
        QVERIFY(f.preview().contains("only reads the radio's status"));
        QVERIFY(!f.button("pdInstallApprove")->isEnabled()); QVERIFY(f.button("pdInstallCancel")->isEnabled());
        QVERIFY(f.recoveryLoaded); f.recoveryLoaded(QString(64, u'a'));
        QCOMPARE(f.model().state(), AetherPDInstallationModel::State::CheckingInstallation);
        QCOMPARE(f.network.sent.size(), std::size_t(1)); QCOMPARE(f.network.sent.front().method, QNetworkAccessManager::GetOperation);
        QVERIFY(!f.job); QCOMPARE(f.writes, 0);
        QTest::mouseClick(f.button("pdInstallCancel"), Qt::LeftButton);
        QCOMPARE(f.model().outcome(), Coordinator::Outcome::Canceled); QCOMPARE(f.writes, 0);
        QVERIFY(f.state().contains("observation canceled")); QVERIFY(!f.model().cleanupConfirmed());
        QVERIFY(f.button("pdInstallCheck")->isEnabled());
    }

    void recoveredCleanupRequiresAnExplicitClickAndFreshCommit()
    {
        Fixture f; const auto package = f.files.loadFirst(); QVERIFY(package);
        const auto prepared = Coordinator::prepareFirst(*package, "fixture-8600",
            QCryptographicHash::hash(QByteArray(64, 'a'), QCryptographicHash::Sha256), "fixture-radio"); QVERIFY(prepared);
        f.journalPlan = prepared->approvedPlan();
        QVERIFY(!f.button("pdInstallCleanup")->isEnabled());
        QVERIFY(f.model().recoverFirstInstallation(f.files.trust.candidateKey, f.radio.controlSession()));
        f.recoveryLoaded(QString(64, u'a'));
        const auto preview = f.model().preview(); QVERIFY(preview);
        f.network.last->complete(200, QJsonDocument(QJsonObject{{"state", "committed"}, {"installation_committed", true},
            {"first_install_plan_blake2b_256", preview->planDigest}, {"selected_release_blake2b_256", preview->candidateDigest},
            {"selected_release_key", preview->candidateKey}, {"profile", 1}, {"firmware", preview->firmware}, {"serial", preview->serial}}).toJson());
        QVERIFY(f.saved); QVERIFY(!f.button("pdInstallCleanup")->isEnabled()); f.saved(true);
        QVERIFY(f.button("pdInstallCleanup")->isEnabled()); QVERIFY(!f.job);
        QCOMPARE(f.network.sent.size(), std::size_t(1));
        QTest::mouseClick(f.button("pdInstallCleanup"), Qt::LeftButton);
        QCOMPARE(f.recoveryLoads, 2); QVERIFY(!f.button("pdInstallCleanup")->isEnabled());
        QVERIFY(f.preview().contains(prepared->nativeName())); QVERIFY(f.preview().contains(prepared->carrierName()));
        f.recoveryLoaded(QString(64, u'a'));
        QVERIFY(f.job); QCOMPARE(f.job->state(), Coordinator::State::WaitingForCommit);
        QCOMPARE(f.network.sent.size(), std::size_t(2));
        QCOMPARE(f.network.sent.back().method, QNetworkAccessManager::GetOperation);
        QTest::mouseClick(f.button("pdInstallCancel"), Qt::LeftButton);
        QCOMPARE(f.model().outcome(), Coordinator::Outcome::Canceled);
        QVERIFY(f.state().contains("cleanup canceled")); QVERIFY(!f.model().cleanupConfirmed());
    }

    void prepareButtonDoesNotApprove()
    {
        Fixture f;
        if (f.model().selectedDevelopmentVersion().isEmpty()) {
            QVERIFY(!f.button("pdInstallPrepare")->isEnabled());
            return;
        }
        QTest::mouseClick(f.button("pdInstallPrepare"), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(f.model().state(), AetherPDInstallationModel::State::AwaitingApproval, 30000);
        QVERIFY(!f.button("pdInstallPrepare")->isEnabled());
        QVERIFY(f.button("pdInstallApprove")->isEnabled());
        QCOMPARE(f.loads, 0); QVERIFY(f.network.sent.empty());
        QTest::mouseClick(f.button("pdInstallCancel"), Qt::LeftButton);
        QCOMPARE(f.model().state(), AetherPDInstallationModel::State::Finished);
        QVERIFY(f.button("pdInstallPrepare")->isEnabled());
    }
    void exactPreviewAndExplicitApproval()
    {
        Fixture f;
        QVERIFY(!f.button("pdInstallApprove")->isEnabled());
        QVERIFY(!f.button("pdInstallApprove")->accessibleDescription().isEmpty());
        const bool selected = !f.model().selectedDevelopmentVersion().isEmpty();
        QCOMPARE(f.button("pdInstallPrepare")->isEnabled(), selected);
        QVERIFY(f.preview().contains(selected ? "not qualified for production" : "Qualified package selection is not yet available"));
        QVERIFY(f.offer());
        const auto preview = f.model().preview(); QVERIFY(preview);
        QVERIFY(f.preview().contains(preview->serial)); QVERIFY(f.preview().contains(preview->firmware));
        QVERIFY(f.preview().contains(preview->candidateDigest)); QVERIFY(f.preview().contains(preview->planDigest));
        QVERIFY(f.preview().contains("Fixed boot/session helpers remain installed"));
        QCOMPARE(f.loads, 0); QVERIFY(f.network.sent.empty());
        QVERIFY(f.button("pdInstallApprove")->isEnabled());
        QTest::keyClick(f.dialog.get(), Qt::Key_Return); QCOMPARE(f.loads, 0);
        const QByteArray oldPlan = f.model().approvedPlan(); QVERIFY(f.offer());
        QVERIFY(!f.model().approve(oldPlan)); QCOMPARE(f.loads, 0);
        f.dialog->show(); QCoreApplication::processEvents();
        const auto* planWidget = f.dialog->findChild<QPlainTextEdit*>(QStringLiteral("pdInstallPlan"));
        const QRect planBounds(planWidget->mapTo(f.dialog.get(), QPoint()), planWidget->size());
        for (QPushButton* control : f.dialog->findChildren<QPushButton*>()) {
            QVERIFY2(control->height() >= control->minimumSizeHint().height(), qPrintable(control->text()));
            const QRect controlBounds(control->mapTo(f.dialog.get(), QPoint()), control->size());
            QVERIFY2(!planBounds.intersects(controlBounds), qPrintable(control->text()));
        }
        const QString screenshot = QString::fromLocal8Bit(qgetenv("AETHERPD_PANEL_SCREENSHOT"));
        if (!screenshot.isEmpty()) { QVERIFY(f.dialog->grab().save(screenshot)); }
        f.button("pdInstallApprove")->click(); QCOMPARE(f.loads, 1);
        QCOMPARE(f.model().state(), AetherPDInstallationModel::State::LoadingCredential);
        QVERIFY(!f.button("pdInstallApprove")->isEnabled());
        QVERIFY(f.button("pdInstallCancel")->isEnabled());
        f.credential({});
        QVERIFY(f.state().contains("No saved Remote Access credential")); QVERIFY(f.network.sent.empty());
    }
    void transmittingAndReconnectedWindowsCannotApprove()
    {
        Fixture f; QVERIFY(f.offer());
        RadioSetupControlTestAccess::transmitting(f.radio, true); emit f.radio.radioTransmittingChanged(true);
        QVERIFY(!f.button("pdInstallApprove")->isEnabled());
        QVERIFY(!f.model().approve(f.model().approvedPlan())); QCOMPARE(f.loads, 0);
        RadioSetupControlTestAccess::transmitting(f.radio, false); emit f.radio.radioTransmittingChanged(false);
        QVERIFY(f.button("pdInstallApprove")->isEnabled());
        RadioSetupControlTestAccess::replaceConnectionGeneration(f.radio);
        f.model().reconcile();
        QVERIFY(f.offer()); // New session's offer cannot be approved by the old window.
        QVERIFY(!f.button("pdInstallApprove")->isEnabled()); QCOMPARE(f.loads, 0);
        f.open(); QVERIFY(f.button("pdInstallApprove")->isEnabled());
    }
    void closeAndReopenPreservesPlanAndRunningOwnership()
    {
        Fixture f; QVERIFY(f.offer());
        const QByteArray plan = f.model().approvedPlan();
        f.dialog->reject(); f.dialog.reset();
        QCOMPARE(f.model().state(), AetherPDInstallationModel::State::AwaitingApproval);
        QCOMPARE(f.loads, 0); f.open(); QCOMPARE(f.model().approvedPlan(), plan);
        f.button("pdInstallApprove")->click(); QCOMPARE(f.loads, 1);
        f.dialog->reject(); f.dialog.reset();
        f.credential(QString(43, u'a')); QVERIFY(f.job);
        QCOMPARE(f.model().state(), AetherPDInstallationModel::State::Running);
        QCOMPARE(f.network.sent.size(), std::size_t(1));
        f.open(); QVERIFY(f.button("pdInstallCancel")->isEnabled());
        // Presentation test only: coordinator terminal correctness is tested separately.
        emit f.job->finished(Coordinator::Outcome::Committed, false);
        QVERIFY(f.state().contains("Radio confirmed the update"));
        QVERIFY(f.state().contains("cleanup is unconfirmed"));
        emit f.job->finished(Coordinator::Outcome::RolledBack, true);
        QVERIFY(f.state().contains("rollback")); QVERIFY(f.state().contains("cleanup confirmed"));
    }
    void cancelBeforeApprovalAndDeletionDuringRender()
    {
        Fixture f; QVERIFY(f.offer());
        f.button("pdInstallCancel")->click(); QCOMPARE(f.loads, 0); QVERIFY(f.network.sent.empty());
        QCOMPARE(f.model().state(), AetherPDInstallationModel::State::Finished);
        QVERIFY(!f.button("pdInstallApprove")->isEnabled());
        connect(&f.model(), &AetherPDInstallationModel::changed, &f.radio, [&] { f.dialog.reset(); });
        QVERIFY(f.offer()); QVERIFY(!f.dialog); QCOMPARE(f.loads, 0);
    }
};
QTEST_MAIN(InstallationDialogTest)
#include "aetherpd_installation_dialog_test.moc"
