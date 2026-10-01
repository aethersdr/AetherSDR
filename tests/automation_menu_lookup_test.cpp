// Menu discovery must observe the existing QWidget tree: QMainWindow::menuBar()
// creates a replacement after MainWindow moves its real bar into TitleBar.
#include "TestSettingsProfile.h"
#include "core/AutomationServer.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QEventLoop>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QWidget>
#include <cstdio>

namespace AetherSDR {
class AutomationServerTestAccess
{
public:
    static QJsonObject request(AutomationServer& server, const QByteArray& line)
    {
        return server.handleLine(line, nullptr);
    }
};
} // namespace AetherSDR

namespace {
int failures = 0;

void check(bool ok, const char* description)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", description);
    if (!ok) {
        ++failures;
    }
}

bool listsMenu(const QJsonObject& reply, const QString& title)
{
    for (const QJsonValue& value : reply.value(QStringLiteral("menus")).toArray()) {
        if (value.toObject().value(QStringLiteral("title")).toString() == title) {
            return true;
        }
    }
    return false;
}

void flushDeferredEvents()
{
    // doInvoke() defers QAction triggers to a clean event-loop turn.
    for (int pass = 0; pass != 4; ++pass) {
        QCoreApplication::processEvents(QEventLoop::AllEvents);
    }
}

void closeMenuBeforeResolution(QMenu* menu)
{
    menu->close();
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    check(!menu->isVisible(), "menu is closed before direct QAction resolution");
}

void exerciseMenuBar(AetherSDR::AutomationServer& server, QMainWindow& window,
                     QMenuBar* menuBar, QMenu* menu, QAction* directAction,
                     const QString& menuTitle, const QString& actionTarget)
{
    const QList<QAction*> originalActions = menuBar->actions();
    const int originalMenuBarCount = window.findChildren<QMenuBar*>().size();
    QWidget* const originalParent = menuBar->parentWidget();
    QWidget* const originalMenuWidget = window.menuWidget();
    int triggered = 0;
    QObject::connect(directAction, &QAction::triggered, [&triggered] { ++triggered; });

    const auto request = [&server](const QByteArray& line) {
        return AetherSDR::AutomationServerTestAccess::request(server, line);
    };
    for (int attempt = 1; attempt <= 2; ++attempt) {
        const QJsonObject listed = request("menu list");
        check(listed.value(QStringLiteral("ok")).toBool() && listsMenu(listed, menuTitle),
              "menu list finds the original top-level menu");

        const QByteArray openRequest = QByteArrayLiteral("menu open ") + menuTitle.toUtf8();
        const QJsonObject opened = request(openRequest);
        check(opened.value(QStringLiteral("ok")).toBool()
                  && opened.value(QStringLiteral("title")).toString() == menuTitle,
              "menu open resolves the original top-level menu");

        // Also cover a bar action without an application-provided submenu.
        // Close the menu so visible-popup matching cannot hide bar discovery.
        closeMenuBeforeResolution(menu);
        const QByteArray invokeRequest = QByteArrayLiteral("invoke ") + actionTarget.toUtf8()
            + QByteArrayLiteral(" trigger");
        const QJsonObject invoked = request(invokeRequest);
        check(invoked.value(QStringLiteral("ok")).toBool()
                  && invoked.value(QStringLiteral("class")).toString()
                      == QStringLiteral("QAction")
                  && invoked.value(QStringLiteral("deferred")).toBool(),
              "invoke resolves the direct closed-menu-bar QAction");
        flushDeferredEvents();
        check(triggered == attempt, "direct menu-bar QAction triggers exactly once");

        check(menuBar->actions() == originalActions
                  && window.findChildren<QMenuBar*>().size() == originalMenuBarCount
                  && menuBar->parentWidget() == originalParent
                  && window.menuWidget() == originalMenuWidget
                  && directAction->parent() == menuBar
                  && menu->parent() == menuBar,
              "menu lookup preserves original action ownership and menu-bar count");
    }
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("automation-menu-lookup"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    AetherSDR::AutomationServer server; // Direct dispatch only: no listener or radio.
    const auto request = [&server](const QByteArray& line) {
        return AetherSDR::AutomationServerTestAccess::request(server, line);
    };

    {
        QMainWindow empty;
        check(!request("menu list").value(QStringLiteral("ok")).toBool(),
              "menu list reports no menus for an empty window");
        check(!request("invoke missingMenuAction trigger").value(QStringLiteral("ok")).toBool(),
              "invoke reports a missing action for an empty window");
        check(empty.findChildren<QMenuBar*>().isEmpty() && !empty.menuWidget(),
              "lookup does not create a menu bar for an empty window");
    }

    // Calling either bridge path must not create a QMenuBar or displace an
    // application's custom menu widget when the window has no menu bar.
    {
        QMainWindow absent;
        auto* customMenuWidget = new QWidget;
        customMenuWidget->setObjectName(QStringLiteral("customMenuWidget"));
        absent.setMenuWidget(customMenuWidget);
        check(absent.findChildren<QMenuBar*>().isEmpty(),
              "absent window starts without a QMenuBar");
        check(!request("menu list").value(QStringLiteral("ok")).toBool(),
              "menu list reports no menus for an absent menu bar");
        check(!request("invoke missingMenuAction trigger").value(QStringLiteral("ok")).toBool(),
              "invoke reports a missing action for an absent menu bar");
        check(absent.findChildren<QMenuBar*>().isEmpty()
                  && absent.menuWidget() == customMenuWidget,
              "lookup leaves an absent bar absent and the custom menu widget intact");
    }

    // The conventional QMainWindow slot remains supported.
    {
        QMainWindow normal;
        auto* menuBar = new QMenuBar(&normal);
        normal.setMenuBar(menuBar);
        QMenu* menu = menuBar->addMenu(QStringLiteral("normalLookupMenu"));
        menu->addAction(QStringLiteral("normalLeaf"));
        auto* directAction = new QAction(QStringLiteral("normalDirectAction"), menuBar);
        directAction->setObjectName(QStringLiteral("normalDirectAction"));
        menuBar->addAction(directAction);
        normal.resize(1000, 200); // Keep the direct action out of Qt's overflow menu.
        normal.show();
        flushDeferredEvents();
        exerciseMenuBar(server, normal, menuBar, menu, directAction,
                        QStringLiteral("normalLookupMenu"),
                        QStringLiteral("normalDirectAction"));
    }

    // MainWindow::buildUI() creates the bar, then reparents it into TitleBar's
    // layout. Reproduce that tree without constructing the full radio UI.
    {
        QMainWindow reparented;
        QMenuBar* menuBar = reparented.menuBar(); // Initial UI construction only.
        auto* titleBar = new QWidget;
        auto* titleLayout = new QHBoxLayout(titleBar);
        titleLayout->setContentsMargins(0, 0, 0, 0);
        titleLayout->addWidget(menuBar);
        auto* central = new QWidget(&reparented);
        auto* centralLayout = new QHBoxLayout(central);
        centralLayout->setContentsMargins(0, 0, 0, 0);
        centralLayout->addWidget(titleBar);
        reparented.setCentralWidget(central);

        QMenu* menu = menuBar->addMenu(QStringLiteral("reparentedLookupMenu"));
        menu->addAction(QStringLiteral("reparentedLeaf"));
        auto* directAction = new QAction(QStringLiteral("reparentedDirectAction"), menuBar);
        directAction->setObjectName(QStringLiteral("reparentedDirectAction"));
        menuBar->addAction(directAction);
        check(menuBar->parentWidget() == titleBar,
              "original menu bar is nested in the title-bar layout");
        reparented.resize(1000, 200);
        reparented.show();
        flushDeferredEvents();
        exerciseMenuBar(server, reparented, menuBar, menu, directAction,
                        QStringLiteral("reparentedLookupMenu"),
                        QStringLiteral("reparentedDirectAction"));
    }

    return failures == 0 ? 0 : 1;
}
