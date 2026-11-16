#include "RadioSetupControlFixture.h"
#include "AetherPDPackageFixture.h"
#include "AetherPDTestTransport.h"
#include "gui/AetherPDDialog.h"
#include "models/AetherPDInstallationModel.h"
#include "core/backends/AetherPDCarrierUpload.h"
#include <QLabel>
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
    int loads{0}, writes{0};
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
            }, AetherPDInstallationModel::RecoveryServices{[](const QByteArray&, const QByteArray&, bool) { return true; }, {}, {}, {}});
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
