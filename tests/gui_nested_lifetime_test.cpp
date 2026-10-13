#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/NetSchedulerDialog.h"
#include "gui/RxApplet.h"
#include "gui/ScopedChildWidget.h"
#include "gui/VfoWidget.h"
#include "models/SliceModel.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QMenu>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QtTest>

#include <memory>

using namespace AetherSDR;

namespace {

QPushButton* buttonByAccessibleName(QWidget& parent, const QString& name)
{
    for (QPushButton* button : parent.findChildren<QPushButton*>()) {
        if (button->accessibleName() == name) {
            return button;
        }
    }
    return nullptr;
}

QPushButton* buttonByText(QWidget& parent, const QString& text)
{
    for (QPushButton* button : parent.findChildren<QPushButton*>()) {
        if (button->text() == text) {
            return button;
        }
    }
    return nullptr;
}

QPushButton* customFilterButton(QWidget& parent)
{
    for (QPushButton* button : parent.findChildren<QPushButton*>()) {
        if (button->contextMenuPolicy() == Qt::CustomContextMenu) {
            return button;
        }
    }
    return nullptr;
}

QMenu* activeMenu()
{
    return qobject_cast<QMenu*>(QApplication::activePopupWidget());
}

void openContextMenu(QPushButton* button)
{
    QMetaObject::invokeMethod(button, "customContextMenuRequested",
                              Qt::DirectConnection,
                              Q_ARG(QPoint, button->rect().center()));
}

} // namespace

// These are component-lifetime contracts: destruction is injected while the
// production widget owns a nested modal loop. They do not claim to reproduce
// a particular persistent-dialog shutdown path.
class GuiNestedLifetimeTest : public QObject
{
    Q_OBJECT

private slots:
    // VfoWidget filters events on its collapsed-frequency label, a SIBLING
    // (child of the VFO's parent) it tracks by QPointer<QLabel>. Parent
    // teardown destroys children in order, so either can die first:
    //  - VFO first (creation order): ~VfoWidget deletes the label itself;
    //  - label first: beginDirectEntry() raise()s the VFO to the end of the
    //    child list, so the parent deletes the label while the VFO and its
    //    filter are alive.
    // Either way the label's own teardown events reach eventFilter() once it
    // is only a QWidget, and comparing them against the QPointer<QLabel> was
    // an invalid downcast (UBSan, #6154). That only shows under the
    // sanitizer lane, which aborts the process on it.
    void vfoCollapsedLabelTeardownInEitherOrder_data()
    {
        QTest::addColumn<bool>("directEntryFirst");
        QTest::newRow("vfo-dies-first") << false;
        QTest::newRow("label-dies-first") << true;
    }

    void vfoCollapsedLabelTeardownInEitherOrder()
    {
        QFETCH(bool, directEntryFirst);
        auto parent = std::make_unique<QWidget>();
        auto* vfo = new VfoWidget(parent.get());
        QLabel* label = nullptr;
        for (QLabel* candidate :
             parent->findChildren<QLabel*>(Qt::FindDirectChildrenOnly)) {
            label = candidate;   // the only direct QLabel child of the parent
        }
        QVERIFY(label);
        if (directEntryFirst) {
            vfo->beginDirectEntry(QStringLiteral("lifetime-test"));
        }
        // The row must really exercise the order it names.
        const QObjectList& order = parent->children();
        QCOMPARE(order.indexOf(label) < order.indexOf(vfo), directEntryFirst);

        QPointer<VfoWidget> vfoObserver(vfo);
        QPointer<QLabel> labelObserver(label);
        parent.reset();
        QVERIFY(vfoObserver.isNull());
        QVERIFY(labelObserver.isNull());
    }

    void txAntennaMenuReturnsAfterAppletDeletion()
    {
        SliceModel slice(0);
        slice.setTxAntenna(QStringLiteral("ANT1"));
        auto rx = std::make_unique<RxApplet>();
        rx->setSlice(&slice);
        QPushButton* button = buttonByAccessibleName(*rx, QStringLiteral("TX antenna"));
        QVERIFY(button);

        QPointer<RxApplet> observer(rx.get());
        QTimer::singleShot(0, qApp, [&rx] { rx.reset(); });
        QMetaObject::invokeMethod(button, "click", Qt::DirectConnection);

        QVERIFY(observer.isNull());
        QCOMPARE(slice.txAntenna(), QStringLiteral("ANT1"));
    }

    void customFilterDialogAppliesOnlyToOriginalSlice_data()
    {
        QTest::addColumn<bool>("rebind");
        QTest::newRow("live-owner-accepts") << false;
        QTest::newRow("rebound-owner-rejects") << true;
    }

    void customFilterDialogAppliesOnlyToOriginalSlice()
    {
        QFETCH(bool, rebind);
        SliceModel original(0);
        SliceModel replacement(1);
        auto rx = std::make_unique<RxApplet>();
        rx->setSlice(&original);
        QPushButton* button = customFilterButton(*rx);
        QVERIFY(button);

        QSignalSpy originalWrites(&original, &SliceModel::filterCommandIssued);
        QSignalSpy replacementWrites(&replacement, &SliceModel::filterCommandIssued);
        bool openedDialog = false;
        QTimer::singleShot(0, qApp, [&] {
            QMenu* menu = activeMenu();
            QVERIFY(menu);
            if (!menu) {
                return;
            }
            QAction* action = nullptr;
            for (QAction* candidate : menu->actions()) {
                if (candidate->text() == QStringLiteral("Set Custom Edges...")) {
                    action = candidate;
                    break;
                }
            }
            QVERIFY(action);
            if (!action) {
                return;
            }
            menu->close();
            openedDialog = true;
            QMetaObject::invokeMethod(qApp, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                if (!dialog) {
                    return;
                }
                const auto spinBoxes = dialog->findChildren<QSpinBox*>();
                QCOMPARE(spinBoxes.size(), 2);
                if (spinBoxes.size() != 2) {
                    return;
                }
                spinBoxes.at(0)->setValue(100);
                spinBoxes.at(1)->setValue(2400);
                if (rebind) {
                    rx->setSlice(&replacement);
                }
                dialog->accept();
            }, Qt::QueuedConnection);
            action->trigger();
        });
        openContextMenu(button);

        QVERIFY(openedDialog);
        if (rebind) {
            QVERIFY(originalWrites.isEmpty());
            QVERIFY(replacementWrites.isEmpty());
            return;
        }
        QCOMPARE(originalWrites.size(), 1);
        QCOMPARE(replacementWrites.size(), 0);
        QCOMPARE(originalWrites.at(0).at(0).toInt(), 100);
        QCOMPARE(originalWrites.at(0).at(1).toInt(), 2400);
        QCOMPARE(original.filterLow(), 100);
        QCOMPARE(original.filterHigh(), 2400);
    }

    void customFilterDialogReturnsAfterAppletDeletion()
    {
        SliceModel slice(0);
        auto rx = std::make_unique<RxApplet>();
        rx->setSlice(&slice);
        QPushButton* button = customFilterButton(*rx);
        QVERIFY(button);

        QPointer<RxApplet> observer(rx.get());
        QTimer::singleShot(0, qApp, [&] {
            QMenu* menu = activeMenu();
            QVERIFY(menu);
            if (!menu) {
                return;
            }
            QAction* action = nullptr;
            for (QAction* candidate : menu->actions()) {
                if (candidate->text() == QStringLiteral("Set Custom Edges...")) {
                    action = candidate;
                    break;
                }
            }
            QVERIFY(action);
            if (!action) {
                return;
            }
            menu->close();
            QMetaObject::invokeMethod(qApp, [&rx] { rx.reset(); }, Qt::QueuedConnection);
            action->trigger();
        });
        openContextMenu(button);

        QVERIFY(observer.isNull());
    }

    void netSchedulerEditorReturnsAfterOwnerDeletion_data()
    {
        QTest::addColumn<bool>("edit");
        QTest::newRow("add-net") << false;
        QTest::newRow("doubleclick-edit-net") << true;
    }

    void netSchedulerEditorReturnsAfterOwnerDeletion()
    {
        QFETCH(bool, edit);

        QList<NetEntry> entries;
        if (edit) {
            NetEntry entry;
            entry.id = QStringLiteral("test-net");
            entry.name = QStringLiteral("Test net");
            entries.append(entry);
        }

        ScopedChildWidget<NetSchedulerDialog> ownerHolder(entries, [] {
            return MemoryEntry{};
        });
        NetSchedulerDialog* owner = ownerHolder.get();
        QVERIFY(owner);
        owner->setAttribute(Qt::WA_DeleteOnClose);
        owner->show();
        QTest::qWait(20);

        QPointer<NetSchedulerDialog> ownerObserver(owner);
        QPointer<QDialog> editorObserver;
        bool openedEditor = false;
        QTimer closer;
        closer.setInterval(10);
        QObject::connect(&closer, &QTimer::timeout, qApp, [&] {
            auto* editor = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!editor || editor->windowTitle() != (edit ? QStringLiteral("Edit Net")
                                                           : QStringLiteral("Add Net"))) {
                return;
            }
            openedEditor = true;
            editorObserver = editor;
            if (ownerObserver) {
                ownerObserver->close();
            }
            closer.stop();
        });
        closer.start();

        if (edit) {
            QTableWidget* table = owner->findChild<QTableWidget*>();
            QVERIFY(table);
            const QPoint cellCenter = table->visualRect(table->model()->index(0, 1)).center();
            QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, cellCenter);
            QTest::mouseDClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, cellCenter);
            QTRY_VERIFY(openedEditor);
        } else {
            QPushButton* add = buttonByText(*owner, QStringLiteral("Add…"));
            QVERIFY(add);
            QMetaObject::invokeMethod(add, "click", Qt::DirectConnection);
        }
        closer.stop();

        QVERIFY(openedEditor);
        QTRY_VERIFY(ownerObserver.isNull());
        QVERIFY(editorObserver.isNull());
    }


};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("gui-nested-lifetime"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();
    QTimer watchdog;
    QObject::connect(&watchdog, &QTimer::timeout, &app, [] {
        qFatal("Nested lifetime test failed to leave its modal loop");
    });
    watchdog.start(15000);
    GuiNestedLifetimeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "gui_nested_lifetime_test.moc"
