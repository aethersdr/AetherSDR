#include "TestSettingsProfile.h"
#include "core/FirmwareStager.h"
#include "core/FirmwareUploader.h"
#include "UpdateTestNetwork.h"
#include "core/UpdateChecker.h"
#include "gui/CanonPrompt.h"
#include "gui/FirmwareUpdateCard.h"
#include "gui/UpdatesWindow.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QLabel>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QTimer>
#include <QtTest>
#include <memory>

namespace AetherSDR {
// No upload(), port request, socket, peer, firmware file or radio connection.
// Inject only local write acceptance and delivery into production handlers.
class FirmwareUploaderTestAccess {
public:
    static void start(FirmwareUploader& uploader, int phase)
    {
        uploader.beginOperation(QByteArray(8, 'x'), QStringLiteral("test.ssdr"));
        if (phase > 0) {
            uploader.markUploadDispatched();
            uploader.startSending(uploader.m_generation,
                [](const char*, qint64 size) { return size; });
        }
        if (phase > 1) {
            drain(uploader);
        }
    }
    static void drain(FirmwareUploader& uploader)
    {
        uploader.acknowledgeBytes(uploader.m_generation, 8);
    }
    static void succeed(FirmwareUploader& uploader)
    {
        uploader.onRadioStatus(uploader.m_generation, QStringLiteral("file update"),
                               {{QStringLiteral("failed"), QStringLiteral("0")}});
    }
    static bool blocked(FirmwareUploader& uploader) { return uploader.retryBarrierActive(); }
};
class UpdatesWindowTestAccess {
public:
    static void attach(UpdatesWindow& window, FirmwareUploader* uploader)
    {
        window.m_firmware->m_uploader = uploader;
    }
    static FirmwareStager* stager(UpdatesWindow& window) { return window.m_firmware->m_stager; }
    static QLabel* status(UpdatesWindow& window) { return window.m_firmware->m_status; }
    static void showOutcome(UpdatesWindow& window, const QString& text)
    {
        window.m_firmware->setStatus(text, QStringLiteral("current"));
    }
    // An Install (network download) that has just finished staging.
    static void beginNetworkInstall(UpdatesWindow& window)
    {
        window.m_firmware->m_staging = true;
        window.m_firmware->m_uploadAfterStage = true;
    }
};
}
using namespace AetherSDR;

class FirmwareCloseDialogTest : public QObject {
    Q_OBJECT
private slots:
    void closePaths_data()
    {
        QTest::addColumn<int>("phase");
        QTest::addColumn<int>("path");
        QTest::addColumn<bool>("confirm");
        for (int phase = 0; phase < 3; ++phase) {
            for (int path = 0; path < 4; ++path) {
                for (bool confirm : {false, true}) {
                    const QByteArray name = QByteArray::number(phase) + "-"
                        + QByteArray::number(path) + (confirm ? "-confirm" : "-keep");
                    QTest::newRow(name.constData()) << phase << path << confirm;
                }
            }
        }
    }
    void closePaths()
    {
        QFETCH(int, phase);
        QFETCH(int, path);
        QFETCH(bool, confirm);
        RadioModel model; // cold, disconnected; never calls connectRadio()
        HeldNetwork network; // no update request leaves the process
        UpdateChecker checker(nullptr, &network);
        UpdatesWindow dialog(&checker, &model);
        FirmwareUploader uploader(nullptr);
        UpdatesWindowTestAccess::attach(dialog, &uploader);
        FirmwareUploaderTestAccess::start(uploader, phase);
        QSignalSpy finished(&uploader, &FirmwareUploader::finished);
        dialog.show();
        bool prompted = false;
        bool safeDefault = false;
        bool reentrantRefused = false;
        QString prompt;
        QTimer::singleShot(0, &dialog, [&] {
            auto* box = qobject_cast<CanonPrompt*>(QApplication::activeModalWidget());
            if (!box) { return; }
            prompted = true;
            prompt = box->text();
            safeDefault = box->cancelButton()->isDefault();
            // A second programmatic dismissal must not bypass the modal guard.
            dialog.reject();
            reentrantRefused = dialog.isVisible() && uploader.isUploading();
            if (!confirm && path == 1) {
                QTest::keyClick(box, Qt::Key_Escape);
            } else {
                (confirm ? box->confirmButton() : box->cancelButton())->click();
            }
        });
        if (path == 0) { dialog.close(); }
        else if (path == 1) { QTest::keyClick(&dialog, Qt::Key_Escape); }
        else if (path == 2) { dialog.reject(); }
        else { dialog.accept(); }
        QCoreApplication::processEvents();
        QVERIFY(prompted);
        QVERIFY(safeDefault);
        QVERIFY(reentrantRefused);
        QVERIFY(prompt.contains(phase == 0 ? QStringLiteral("No image bytes")
                                : phase == 1 ? QStringLiteral("incomplete image")
                                : QStringLiteral("does not undo")));
        QCOMPARE(dialog.isVisible(), !confirm);
        QCOMPARE(uploader.isUploading(), !confirm);
        QCOMPARE(finished.size(), confirm ? 1 : 0);
        QCOMPARE(FirmwareUploaderTestAccess::blocked(uploader), phase > 0);
        if (confirm) {
            QCOMPARE(qvariant_cast<FirmwareUploader::Outcome>(finished.front().front()),
                     phase == 2 ? FirmwareUploader::Outcome::Unconfirmed
                                : FirmwareUploader::Outcome::Failed);
        }
        QVERIFY(!model.isConnected());
    }
    void advancesInsidePrompt()
    {
        RadioModel model;
        HeldNetwork network; // no update request leaves the process
        UpdateChecker checker(nullptr, &network);
        UpdatesWindow dialog(&checker, &model);
        FirmwareUploader uploader(nullptr);
        UpdatesWindowTestAccess::attach(dialog, &uploader);
        FirmwareUploaderTestAccess::start(uploader, 1);
        QSignalSpy finished(&uploader, &FirmwareUploader::finished);
        dialog.show();
        QTimer::singleShot(0, &dialog, [&] {
            auto* box = qobject_cast<CanonPrompt*>(QApplication::activeModalWidget());
            QVERIFY(box);
            FirmwareUploaderTestAccess::drain(uploader);
            QVERIFY(box->text().contains(QStringLiteral("does not undo")));
            box->confirmButton()->click();
        });
        dialog.close();
        QCOMPARE(finished.size(), 1);
        QCOMPARE(qvariant_cast<FirmwareUploader::Outcome>(finished.front().front()),
                 FirmwareUploader::Outcome::Unconfirmed);
    }
    void terminalInsidePrompt()
    {
        RadioModel model;
        HeldNetwork network; // no update request leaves the process
        UpdateChecker checker(nullptr, &network);
        UpdatesWindow dialog(&checker, &model);
        FirmwareUploader uploader(nullptr);
        UpdatesWindowTestAccess::attach(dialog, &uploader);
        FirmwareUploaderTestAccess::start(uploader, 2);
        QSignalSpy finished(&uploader, &FirmwareUploader::finished);
        dialog.show();
        QTimer::singleShot(0, &dialog, [&] {
            auto* box = qobject_cast<CanonPrompt*>(QApplication::activeModalWidget());
            QVERIFY(box);
            FirmwareUploaderTestAccess::succeed(uploader);
            QVERIFY(box->text().contains(QStringLiteral("has ended")));
            box->confirmButton()->click();
        });
        dialog.close();
        QCOMPARE(finished.size(), 1);
        QCOMPARE(qvariant_cast<FirmwareUploader::Outcome>(finished.front().front()),
                 FirmwareUploader::Outcome::Succeeded);
        QVERIFY(!FirmwareUploaderTestAccess::blocked(uploader));
    }
    void deleteOnClose()
    {
        RadioModel model;
        HeldNetwork network;
        UpdateChecker checker(nullptr, &network);
        auto* dialog = new UpdatesWindow(&checker, &model);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        QPointer<UpdatesWindow> guard(dialog);
        auto* uploader = new FirmwareUploader(nullptr, dialog);
        UpdatesWindowTestAccess::attach(*dialog, uploader);
        FirmwareUploaderTestAccess::start(*uploader, 1);
        dialog->show();
        QTimer::singleShot(0, dialog, [] {
            auto* box = qobject_cast<CanonPrompt*>(QApplication::activeModalWidget());
            QVERIFY(box);
            box->confirmButton()->click();
        });
        QTest::keyClick(dialog, Qt::Key_Escape);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(guard.isNull());
    }
    void idleDismissalNeedsNoConfirmation()
    {
        RadioModel model;
        HeldNetwork network; // no update request leaves the process
        UpdateChecker checker(nullptr, &network);
        UpdatesWindow dialog(&checker, &model);
        dialog.show();
        QTest::keyClick(&dialog, Qt::Key_Escape);
        QVERIFY(!dialog.isVisible());
        QVERIFY(!QApplication::activeModalWidget());
    }
    void finishedReceiverDeletesOwner()
    {
        RadioModel model;
        HeldNetwork network;
        UpdateChecker checker(nullptr, &network);
        auto dialog = std::make_unique<UpdatesWindow>(&checker, &model);
        auto* uploader = new FirmwareUploader(nullptr, dialog.get());
        UpdatesWindowTestAccess::attach(*dialog, uploader);
        FirmwareUploaderTestAccess::start(*uploader, 2);
        connect(uploader, &FirmwareUploader::finished, qApp, [&] { dialog.reset(); });
        dialog->show();
        QTimer::singleShot(0, dialog.get(), [] {
            auto* box = qobject_cast<CanonPrompt*>(QApplication::activeModalWidget());
            QVERIFY(box);
            box->confirmButton()->click();
        });
        dialog->close();
        QVERIFY(!dialog);
    }
    void indexCheckIsNotAnInstall()
    {
        // FirmwareStager reports its release-index check on stageProgress
        // too; that must not paint an install that is not happening.
        RadioModel model;
        HeldNetwork network;
        UpdateChecker checker(nullptr, &network);
        UpdatesWindow window(&checker, &model);
        window.show();
        auto* progress = window.findChild<QProgressBar*>(QStringLiteral("firmwareProgress"));
        QVERIFY(progress);
        emit UpdatesWindowTestAccess::stager(window)->stageProgress(37, QStringLiteral("Checking for updates..."));
        QVERIFY(progress->value() != 37);
        QVERIFY(UpdatesWindowTestAccess::status(window)->isHidden());
    }
    void indexCheckLeavesTheOutcome()
    {
        // After an install the radio reboots and reconnects, which re-runs
        // the release-list check; its start, answer or failure must leave
        // the install's outcome and progress bar alone.
        RadioModel model;
        HeldNetwork network;
        UpdateChecker checker(nullptr, &network);
        UpdatesWindow window(&checker, &model);
        window.show();
        const QString outcome = QStringLiteral("The radio confirmed the update.");
        UpdatesWindowTestAccess::showOutcome(window, outcome);
        FirmwareStager* stager = UpdatesWindowTestAccess::stager(window);
        emit stager->stageProgress(37, QStringLiteral("Checking for updates..."));
        emit stager->updateCheckComplete(QStringLiteral("4.3.16"), true, {QStringLiteral("4.3.16")});
        emit stager->updateCheckFailed(QStringLiteral("index unavailable"));
        QCOMPARE(UpdatesWindowTestAccess::status(window)->text(), outcome);
        auto* progress = window.findChild<QProgressBar*>(QStringLiteral("firmwareProgress"));
        QVERIFY(progress);
        QVERIFY(progress->value() != 37);
    }
    void unverifiedDownloadAsksAgain()
    {
        // FlexRadio's MD5 could not be fetched: the Install the operator
        // confirmed on the promise of a check must ask again and say so.
        RadioModel model;
        HeldNetwork network;
        UpdateChecker checker(nullptr, &network);
        UpdatesWindow window(&checker, &model);
        window.show();
        FirmwareStager* stager = UpdatesWindowTestAccess::stager(window);
        QVERIFY(!stager->stagedVerified());
        UpdatesWindowTestAccess::beginNetworkInstall(window);
        QString prompt;
        QTimer::singleShot(0, &window, [&] {
            auto* box = qobject_cast<CanonPrompt*>(QApplication::activeModalWidget());
            QVERIFY(box);
            prompt = box->text();
            QVERIFY(box->cancelButton()->isDefault());
            box->cancelButton()->click();
        });
        emit stager->stageComplete(QStringLiteral("FLEX-9600_v4.3.16.ssdr"), QStringLiteral("4.3.16"));
        QVERIFY(prompt.contains(QStringLiteral("was NOT checked")));
        QVERIFY(UpdatesWindowTestAccess::status(window)->text().contains(QStringLiteral("not started")));
    }
    void ownerDeletedInsidePrompt()
    {
        RadioModel model;
        HeldNetwork network;
        UpdateChecker checker(nullptr, &network);
        auto dialog = std::make_unique<UpdatesWindow>(&checker, &model);
        auto* uploader = new FirmwareUploader(nullptr, dialog.get());
        UpdatesWindowTestAccess::attach(*dialog, uploader);
        FirmwareUploaderTestAccess::start(*uploader, 1);
        dialog->show();
        QTimer::singleShot(0, qApp, [&] { dialog.reset(); });
        dialog->close();
        QVERIFY(!dialog);
    }
};
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-firmware-close-dialog-test"));
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    FirmwareCloseDialogTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "firmware_close_dialog_test.moc"
