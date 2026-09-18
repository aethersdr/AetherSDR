#include "TestSettingsProfile.h"
#include "core/FirmwareUploader.h"
#include "gui/RadioSetupDialog.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QMessageBox>
#include <QPointer>
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
class RadioSetupDialogTestAccess {
public:
    static void attach(RadioSetupDialog& dialog, FirmwareUploader* uploader)
    {
        dialog.m_uploader = uploader;
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
        RadioSetupDialog dialog(&model);
        FirmwareUploader uploader(nullptr);
        RadioSetupDialogTestAccess::attach(dialog, &uploader);
        FirmwareUploaderTestAccess::start(uploader, phase);
        QSignalSpy finished(&uploader, &FirmwareUploader::finished);
        dialog.show();
        bool prompted = false;
        bool safeDefault = false;
        bool reentrantRefused = false;
        QString prompt;
        QTimer::singleShot(0, &dialog, [&] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!box) { return; }
            prompted = true;
            prompt = box->text();
            safeDefault = box->defaultButton() == box->button(QMessageBox::Cancel);
            // A second programmatic dismissal must not bypass the modal guard.
            dialog.reject();
            reentrantRefused = dialog.isVisible() && uploader.isUploading();
            if (!confirm && path == 1) {
                QTest::keyClick(box, Qt::Key_Escape);
            } else {
                box->button(confirm ? QMessageBox::Ok : QMessageBox::Cancel)->click();
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
        RadioSetupDialog dialog(&model);
        FirmwareUploader uploader(nullptr);
        RadioSetupDialogTestAccess::attach(dialog, &uploader);
        FirmwareUploaderTestAccess::start(uploader, 1);
        QSignalSpy finished(&uploader, &FirmwareUploader::finished);
        dialog.show();
        QTimer::singleShot(0, &dialog, [&] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            QVERIFY(box);
            FirmwareUploaderTestAccess::drain(uploader);
            QVERIFY(box->text().contains(QStringLiteral("does not undo")));
            box->button(QMessageBox::Ok)->click();
        });
        dialog.close();
        QCOMPARE(finished.size(), 1);
        QCOMPARE(qvariant_cast<FirmwareUploader::Outcome>(finished.front().front()),
                 FirmwareUploader::Outcome::Unconfirmed);
    }
    void terminalInsidePrompt()
    {
        RadioModel model;
        RadioSetupDialog dialog(&model);
        FirmwareUploader uploader(nullptr);
        RadioSetupDialogTestAccess::attach(dialog, &uploader);
        FirmwareUploaderTestAccess::start(uploader, 2);
        QSignalSpy finished(&uploader, &FirmwareUploader::finished);
        dialog.show();
        QTimer::singleShot(0, &dialog, [&] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            QVERIFY(box);
            FirmwareUploaderTestAccess::succeed(uploader);
            QVERIFY(box->text().contains(QStringLiteral("has ended")));
            box->button(QMessageBox::Ok)->click();
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
        auto* dialog = new RadioSetupDialog(&model);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        QPointer<RadioSetupDialog> guard(dialog);
        auto* uploader = new FirmwareUploader(nullptr, dialog);
        RadioSetupDialogTestAccess::attach(*dialog, uploader);
        FirmwareUploaderTestAccess::start(*uploader, 1);
        dialog->show();
        QTimer::singleShot(0, dialog, [] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            QVERIFY(box);
            box->button(QMessageBox::Ok)->click();
        });
        QTest::keyClick(dialog, Qt::Key_Escape);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(guard.isNull());
    }
    void idleDismissalNeedsNoConfirmation()
    {
        RadioModel model;
        RadioSetupDialog dialog(&model);
        dialog.show();
        QTest::keyClick(&dialog, Qt::Key_Escape);
        QVERIFY(!dialog.isVisible());
        QVERIFY(!QApplication::activeModalWidget());
    }
    void finishedReceiverDeletesOwner()
    {
        RadioModel model;
        auto dialog = std::make_unique<RadioSetupDialog>(&model);
        auto* uploader = new FirmwareUploader(nullptr, dialog.get());
        RadioSetupDialogTestAccess::attach(*dialog, uploader);
        FirmwareUploaderTestAccess::start(*uploader, 2);
        connect(uploader, &FirmwareUploader::finished, qApp, [&] { dialog.reset(); });
        dialog->show();
        QTimer::singleShot(0, dialog.get(), [] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            QVERIFY(box);
            box->button(QMessageBox::Ok)->click();
        });
        dialog->close();
        QVERIFY(!dialog);
    }
    void ownerDeletedInsidePrompt()
    {
        RadioModel model;
        auto dialog = std::make_unique<RadioSetupDialog>(&model);
        auto* uploader = new FirmwareUploader(nullptr, dialog.get());
        RadioSetupDialogTestAccess::attach(*dialog, uploader);
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
