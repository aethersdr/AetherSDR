#include "TestSettingsProfile.h"

#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "gui/CanonMessageBox.h"
#include "gui/CanonWindow.h"
#include "gui/NetSchedulerDialog.h"
#include "gui/ScopedChildWidget.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QImage>
#include <QLineEdit>
#include <QMenu>
#include <QElapsedTimer>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTimer>
#include <QtTest>

#include <functional>

using namespace AetherSDR;

namespace {

const QString kGeometryKey = QStringLiteral("NetSchedulerDialogGeometry");

QList<NetEntry> twoNets()
{
    NetEntry on;
    on.id = QStringLiteral("net-on");
    on.name = QStringLiteral("Tuesday 20 m Net");
    on.rrule = QStringLiteral("FREQ=WEEKLY;BYDAY=TU");
    NetEntry off = on;
    off.id = QStringLiteral("net-off");
    off.name = QStringLiteral("Sunday 20 m Net");
    off.enabled = false;
    return {on, off};
}

QPushButton* buttonByText(QWidget& parent, const QString& text)
{
    for (QPushButton* button : parent.findChildren<QPushButton*>()) {
        if (button->text() == text)
            return button;
    }
    return nullptr;
}

// Runs `act` on the modal box the next click opens, from inside its exec().
// The box is closed afterwards even when a check in `act` fails, and the poll
// gives up after two seconds, so a failure ends the case instead of hanging it
// or reaching into the next one.
void onNextModal(std::function<void(QWidget*)> act)
{
    auto* poll = new QTimer(qApp);
    poll->setInterval(10);
    auto* deadline = new QElapsedTimer;
    deadline->start();
    QObject::connect(poll, &QTimer::timeout, qApp, [poll, deadline, act] {
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal && deadline->elapsed() < 2000)
            return;
        poll->stop();
        poll->deleteLater();
        delete deadline;
        if (!modal)
            return;
        const QPointer<QWidget> guard(modal);
        act(modal);
        if (guard && guard->isVisible()) {
            if (auto* dialog = qobject_cast<QDialog*>(guard.data()))
                dialog->reject();
        }
    });
    poll->start();
}

} // namespace

class NetSchedulerDialogTest : public QObject {
    Q_OBJECT

private slots:
    void init() { AppSettings::instance().remove(kGeometryKey); }

    void isACanonWorkspaceInTheCanonScope()
    {
        NetSchedulerDialog dlg(twoNets(), [] { return MemoryEntry{}; });
        QVERIFY(qobject_cast<CanonWindow*>(&dlg));
        QVERIFY(dlg.windowFlags().testFlag(Qt::FramelessWindowHint));
        QCOMPARE(dlg.windowFlags() & Qt::WindowType_Mask, Qt::Window);
        QCOMPARE(ThemeManager::instance().containerPathFor(&dlg),
                 QStringLiteral("canon/netScheduler"));
    }

    // A rectangle the PersistentDialog era saved restores in the canon window.
    void legacyGeometryCarriesOver()
    {
        QWidget legacy;
        legacy.setGeometry(10, 20, 780, 520);  // inside the offscreen 800x600 screen
        AppSettings::instance().setValue(kGeometryKey,
                                         QString::fromLatin1(legacy.saveGeometry().toBase64()));

        NetSchedulerDialog dlg(twoNets(), [] { return MemoryEntry{}; });
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        QCOMPARE(dlg.size(), QSize(780, 520));
    }

    // Every button keeps its whole label at the window's minimum size.
    void buttonsFitAtMinimumSize()
    {
        NetSchedulerDialog dlg(twoNets(), [] { return MemoryEntry{}; });
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        dlg.resize(dlg.minimumSize());
        QTRY_COMPARE(dlg.size(), dlg.minimumSize());
        for (QPushButton* button : dlg.findChildren<QPushButton*>()) {
            if (!button->isVisible() || button->text().isEmpty())
                continue;
            QVERIFY2(button->width() >= button->sizeHint().width(),
                     qPrintable(QStringLiteral("%1 is %2 px, needs %3")
                                    .arg(button->text())
                                    .arg(button->width())
                                    .arg(button->sizeHint().width())));
        }
    }

    // Return in the Remove box takes Cancel, the default: the net stays.
    void removeDefaultsToCancel()
    {
        NetSchedulerDialog dlg(twoNets(), [] { return MemoryEntry{}; });
        QSignalSpy changed(&dlg, &NetSchedulerDialog::entriesChanged);
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        dlg.findChild<QTableWidget*>()->setCurrentCell(0, 1);

        QString defaultText;
        onNextModal([&defaultText](QWidget* modal) {
            auto* box = qobject_cast<CanonMessageBox*>(modal);
            QVERIFY(box);
            for (QPushButton* b : box->findChildren<QPushButton*>()) {
                if (b->isDefault())
                    defaultText = b->text();
            }
            QTest::keyClick(box, Qt::Key_Return);
        });
        QPushButton* remove = buttonByText(dlg, QStringLiteral("Remove"));
        QVERIFY(remove && remove->isEnabled());
        remove->click();

        QCOMPARE(defaultText, QStringLiteral("Cancel"));
        QCOMPARE(dlg.entries().size(), 2);
        QCOMPARE(changed.count(), 0);
    }

    void removeConfirmedDeletesTheSelectedNet()
    {
        NetSchedulerDialog dlg(twoNets(), [] { return MemoryEntry{}; });
        QSignalSpy changed(&dlg, &NetSchedulerDialog::entriesChanged);
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        dlg.findChild<QTableWidget*>()->setCurrentCell(0, 1);

        onNextModal([](QWidget* modal) {
            auto* confirm = modal->findChild<QPushButton*>(QStringLiteral("netRemoveConfirm"));
            QVERIFY(confirm);
            confirm->click();
        });
        QPushButton* remove = buttonByText(dlg, QStringLiteral("Remove"));
        QVERIFY(remove && remove->isEnabled());
        remove->click();

        QCOMPARE(dlg.entries().size(), 1);
        QCOMPARE(dlg.entries().constFirst().id, QStringLiteral("net-off"));
        QCOMPARE(changed.count(), 1);
    }

    // A disabled net's muted ink follows a theme change.
    void disabledRowFollowsTheTheme()
    {
        NetSchedulerDialog dlg(twoNets(), [] { return MemoryEntry{}; });
        auto* table = dlg.findChild<QTableWidget*>();
        const QColor fresh(0x12, 0x34, 0x56);
        ThemeManager::instance().setColor(QStringLiteral("color.canon.muted"), fresh);
        QCOMPARE(table->item(1, 1)->foreground().color(), fresh);
        QVERIFY(table->item(0, 1)->foreground().color() != fresh);
    }

    // The Add / Edit editor is a canon dialog whose OK takes Return.
    void editorIsACanonDialogWithOkDefault()
    {
        NetSchedulerDialog dlg(twoNets(), [] { return MemoryEntry{}; });
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));

        bool sawEditor = false;
        bool okDefault = false;
        onNextModal([&](QWidget* modal) {
            sawEditor = qobject_cast<CanonWindow*>(modal)
                && modal->windowTitle() == QStringLiteral("Add Net");
            if (auto* ok = buttonByText(*modal, QStringLiteral("OK")))
                okDefault = ok->isDefault();
            qobject_cast<QDialog*>(modal)->reject();
        });
        QPushButton* add = buttonByText(dlg, QStringLiteral("Add…"));
        QVERIFY(add);
        add->click();

        QVERIFY(sawEditor);
        QVERIFY(okDefault);
        QCOMPARE(dlg.entries().size(), 2);
    }

    // A spin box's text sits as far in from its border as a line edit's does.
    void editorSpinTextIsInset()
    {
        NetSchedulerDialog dlg(twoNets(), [] { return MemoryEntry{}; });
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));

        QStringList flush;
        int checked = 0;
        onNextModal([&](QWidget* modal) {
            for (auto* spin : modal->findChildren<QAbstractSpinBox*>()) {
                auto* editor = spin->findChild<QLineEdit*>();
                if (!editor)
                    continue;
                ++checked;
                if (editor->x() + editor->textMargins().left() < 12)
                    flush << spin->metaObject()->className();
            }
            qobject_cast<QDialog*>(modal)->reject();
        });
        QPushButton* add = buttonByText(dlg, QStringLiteral("Add…"));
        QVERIFY(add);
        add->click();

        QVERIFY(checked >= 6);
        QVERIFY2(flush.isEmpty(), qPrintable(flush.join(QStringLiteral(", "))));
    }

    // A field's standard right-click menu is a child of the field, so the
    // canon sheet's transparent-everything rule reaches it unless QMenu gets
    // its panel back.
    void editorFieldMenuIsOpaque()
    {
        NetSchedulerDialog dlg(twoNets(), [] { return MemoryEntry{}; });
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));

        bool sawField = false;
        bool parentedInside = false;
        int alpha = -1;
        onNextModal([&](QWidget* modal) {
            auto* canon = qobject_cast<CanonWindow*>(modal);
            const QList<QLineEdit*> edits = canon
                ? canon->bodyWidget()->findChildren<QLineEdit*>()
                : QList<QLineEdit*>{};
            if (!edits.isEmpty()) {
                sawField = true;
                QMenu* menu = edits.first()->createStandardContextMenu();
                parentedInside = menu->parentWidget() == edits.first();
                menu->popup(modal->mapToGlobal(QPoint(40, 40)));
                QTest::qWait(100);
                const QImage image = menu->grab().toImage();
                if (!image.isNull())
                    alpha = qAlpha(image.pixel(image.width() / 2, image.height() / 2));
                menu->close();
                delete menu;
            }
            qobject_cast<QDialog*>(modal)->reject();
        });
        QPushButton* add = buttonByText(dlg, QStringLiteral("Add…"));
        QVERIFY(add);
        add->click();

        QVERIFY(sawField);
        QVERIFY(parentedInside);
        QCOMPARE(alpha, 255);
    }

    // MainWindow opens the window with WA_DeleteOnClose. Every way out
    // destroys it, Esc included (QDialog::done() closes, it does not only
    // hide), so the next open starts from the schedule MainWindow passes in.
    void everyCloseDestroysTheWindow_data()
    {
        QTest::addColumn<int>("path");
        QTest::newRow("Esc") << 0;
        QTest::newRow("Ctrl+W / Cmd+W") << 1;
        QTest::newRow("corner close button") << 2;
    }

    void everyCloseDestroysTheWindow()
    {
        QFETCH(int, path);
        QPointer<NetSchedulerDialog> dlg =
            new NetSchedulerDialog(twoNets(), [] { return MemoryEntry{}; });
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
        QVERIFY(QTest::qWaitForWindowExposed(dlg));
        if (path == 0)
            QTest::keyClick(dlg, Qt::Key_Escape);
        else if (path == 1)
            QTest::keySequence(dlg, QKeySequence::Close);
        else
            dlg->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        const bool destroyed = dlg.isNull();
        delete dlg.data();
        QVERIFY(destroyed);
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("net-scheduler-dialog"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();
    NetSchedulerDialogTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "net_scheduler_dialog_test.moc"
