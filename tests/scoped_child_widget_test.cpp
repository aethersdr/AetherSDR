#include "gui/ScopedChildWidget.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QMenu>
#include <QMessageBox>
#include <QKeyEvent>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <QPoint>
#include <QObject>
#include <QEvent>
#include <QString>
#include <QVariant>

#include <cstdio>
#include <memory>

namespace {
int failures = 0;
void check(const char* name, bool passed)
{
    std::printf("[%s] %s\n", passed ? "OK" : "FAIL", name);
    if (!passed) {
        ++failures;
    }
}

void menuLifetime(bool destroyParent)
{
    auto parent = std::make_unique<QWidget>();
    QPointer<QMenu> observer;
    int destroyed = 0;
    {
        AetherSDR::ScopedChildWidget<QMenu> child(parent.get());
        observer = child.get();
        observer->addAction("Example");
        QObject::connect(observer, &QObject::destroyed, qApp,
                         [&destroyed] { ++destroyed; });
        QTimer::singleShot(0, observer, [&] {
            if (destroyParent) {
                parent.reset();
            } else {
                observer->close();
            }
        });
        child.get()->exec(QPoint(20, 20));
        check("menu guard reflects parent lifetime", bool(child) != destroyParent);
    }
    check("menu destroyed exactly once", observer.isNull() && destroyed == 1);
}

void dialogLifetime(bool destroyParent, bool accept)
{
    auto parent = std::make_unique<QWidget>();
    QPointer<QDialog> observer;
    int destroyed = 0;
    {
        AetherSDR::ScopedChildWidget<QDialog> child(parent.get());
        observer = child.get();
        QObject::connect(observer, &QObject::destroyed, qApp,
                         [&destroyed] { ++destroyed; });
        QTimer::singleShot(0, observer, [&] {
            if (destroyParent) {
                parent.reset();
            } else if (accept) {
                observer->accept();
            } else {
                observer->reject();
            }
        });
        const int result = child.get()->exec();
        check("dialog guard reflects parent lifetime", bool(child) != destroyParent);
        check("dialog result preserved", result ==
              ((!destroyParent && accept) ? QDialog::Accepted : QDialog::Rejected));
    }
    check("dialog destroyed exactly once", observer.isNull() && destroyed == 1);
}

void nestedDialogLifetime()
{
    auto parent = std::make_unique<QWidget>();
    QPointer<QMenu> menuObserver;
    QPointer<QDialog> dialogObserver;
    bool returnedFromDialog = false;
    {
        AetherSDR::ScopedChildWidget<QMenu> menu(parent.get());
        menuObserver = menu.get();
        QAction* action = menu.get()->addAction("Open dialog");
        QObject::connect(action, &QAction::triggered, parent.get(), [&] {
            AetherSDR::ScopedChildWidget<QDialog> dialog(parent.get());
            dialogObserver = dialog.get();
            QTimer::singleShot(0, dialog.get(), [&] { parent.reset(); });
            dialog.get()->exec();
            returnedFromDialog = true;
            check("nested dialog observes owner destruction", !dialog);
        });
        QTimer::singleShot(0, action, &QAction::trigger);
        menu.get()->exec(QPoint(20, 20));
        check("outer menu observes owner destruction", !menu);
    }
    check("both nested loops return after owner destruction",
          returnedFromDialog && menuObserver.isNull() && dialogObserver.isNull());
}

void forwardedConstructorLifetime()
{
    auto parent = std::make_unique<QWidget>();
    AetherSDR::ScopedChildWidget<QMessageBox> child(
        QMessageBox::Information, QStringLiteral("Original title"),
        QStringLiteral("Original text"), QMessageBox::Ok, parent.get());
    // Compare with Qt's direct constructor under the SAME parent, so the check
    // isolates argument forwarding rather than parentage: macOS may normalize
    // message-box window titles even though the caller supplied an explicit
    // one, and an unparented control would diverge for that reason instead.
    // Heap-allocated deliberately — a stack QMessageBox parented here is the
    // invalid free this header exists to prevent, and the parent is destroyed
    // below.
    auto* direct = new QMessageBox(QMessageBox::Information,
                                   QStringLiteral("Original title"),
                                   QStringLiteral("Original text"),
                                   QMessageBox::Ok, parent.get());
    check("forwarded constructor preserves dialog configuration",
          child.get()->windowTitle() == direct->windowTitle()
              && child.get()->text() == direct->text()
              && child.get()->icon() == direct->icon()
              && child.get()->standardButtons() == direct->standardButtons()
              && child.get()->parentWidget() == parent.get());
    QTimer::singleShot(0, child.get(), [&] { parent.reset(); });
    child.get()->exec();
    check("forwarded constructor retains guarded parent ownership", !child);
}

void selectedActionLifetime()
{
    QWidget parent;
    QPointer<QAction> actionObserver;
    {
        AetherSDR::ScopedChildWidget<QMenu> child(&parent);
        QAction* action = child.get()->addAction("Select antenna");
        action->setData(QStringLiteral("ANT1"));
        actionObserver = action;
        QTimer::singleShot(0, child.get(), [&] {
            child.get()->setActiveAction(action);
            QKeyEvent key(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(child.get(), &key);
        });
        QAction* selected = child.get()->exec(QPoint(20, 20));
        check("selected action remains readable after exec",
              selected == action && selected->data().toString() == QStringLiteral("ANT1"));
    }
    check("selected action is released with menu", actionObserver.isNull());
}

void actionLifetime()
{
    QWidget parent;
    AetherSDR::ScopedChildWidget<QMenu> child(&parent);
    auto receiver = std::make_unique<QObject>();
    int calls = 0;
    QAction* action = child.get()->addAction("Example");
    QObject::connect(action, &QAction::triggered, receiver.get(), [&] { ++calls; });
    action->trigger();
    check("live action receiver runs", calls == 1);
    receiver.reset();
    action->trigger();
    check("deleted action receiver is disconnected", calls == 1);
}
} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    menuLifetime(false);
    menuLifetime(true);
    dialogLifetime(false, false);
    dialogLifetime(false, true);
    dialogLifetime(true, false);
    nestedDialogLifetime();
    forwardedConstructorLifetime();
    selectedActionLifetime();
    actionLifetime();
    return failures ? 1 : 0;
}
