// Socket-free: the real menu and its sibling panels sit over a recording
// QWidget. A leaked event is observable without connecting or tuning a radio.
#include "TestSettingsProfile.h"
#include "gui/MemoryBrowsePanel.h"
#include "gui/SpectrumOverlayMenu.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPoint>
#include <QPushButton>
#include <QString>
#include <QWidget>

#include <cstdio>
#include <initializer_list>

using namespace AetherSDR;

namespace {

class MouseHost final : public QWidget {
public:
    int received{0};

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        ++received;
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override
    {
        ++received;
        event->accept();
    }
    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        ++received;
        event->accept();
    }
};

int failures = 0;

void check(bool ok, const QString& description)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", qPrintable(description));
    if (!ok) {
        ++failures;
    }
}

void sendMouse(QWidget* target, QEvent::Type type, Qt::MouseButton button,
               const QPoint& position)
{
    QMouseEvent event(type, QPointF(position), QPointF(target->mapToGlobal(position)),
                      button, type == QEvent::MouseButtonRelease ? Qt::NoButton : button,
                      Qt::NoModifier);
    QApplication::sendEvent(target, &event);
}

void checkBoundary(MouseHost& host, QWidget* target, const QString& description,
                   const QPoint& position)
{
    check(target != nullptr, description + QStringLiteral(" exists"));
    if (!target) {
        return;
    }
    const int before = host.received;
    sendMouse(target, QEvent::MouseButtonPress, Qt::RightButton, position);
    sendMouse(target, QEvent::MouseButtonRelease, Qt::RightButton, position);
    sendMouse(target, QEvent::MouseButtonDblClick, Qt::RightButton, position);
    check(host.received == before, description + QStringLiteral(" keeps right clicks off the spectrum"));
}

void checkPanel(MouseHost& host, QWidget* panel, const QString& name)
{
    check(panel != nullptr, name + QStringLiteral(" panel exists"));
    if (!panel) {
        return;
    }
    panel->show();
    panel->adjustSize();
    QApplication::processEvents();
    checkBoundary(host, panel, name + QStringLiteral(" background"), QPoint(1, 1));
    if (QLabel* label = panel->findChild<QLabel*>()) {
        checkBoundary(host, label, name + QStringLiteral(" label"), label->rect().center());
    }
    if (QPushButton* button = panel->findChild<QPushButton*>()) {
        checkBoundary(host, button, name + QStringLiteral(" button"), button->rect().center());
    }
    if (QWidget* content = panel->findChild<QWidget*>(QStringLiteral("displayPanelContent"))) {
        checkBoundary(host, content, QStringLiteral("Display scroll content margin"), QPoint(1, 1));
    }
    const int before = host.received;
    sendMouse(panel, QEvent::MouseButtonDblClick, Qt::LeftButton, QPoint(1, 1));
    check(host.received == before, name + QStringLiteral(" background keeps left double clicks off the spectrum"));
    panel->hide();
}

} // namespace

int main(int argc, char* argv[])
{
    TestSettingsProfile profile(QStringLiteral("spectrum-overlay-mouse-boundary"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    MouseHost host;
    host.resize(1200, 900);
    SpectrumOverlayMenu menu(&host);
    host.show();

    for (const char* name : {"bandPanel", "antPanel", "daxPanel", "displayPanel"}) {
        checkPanel(host, host.findChild<QWidget*>(QString::fromLatin1(name)),
                   QString::fromLatin1(name));
    }
    checkPanel(host, host.findChild<MemoryBrowsePanel*>(), QStringLiteral("Memory"));

    menu.setXvtrBands({{QStringLiteral("2m"), 144.2, QStringLiteral("X1")}});
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    checkPanel(host, host.findChild<QWidget*>(QStringLiteral("bandPanel")),
               QStringLiteral("Rebuilt Band"));
    checkPanel(host, host.findChild<QWidget*>(QStringLiteral("xvtrPanel")),
               QStringLiteral("XVTR"));

    QWidget* bandPanel = host.findChild<QWidget*>(QStringLiteral("bandPanel"));
    QPushButton* bandButton = nullptr;
    if (bandPanel) {
        for (QPushButton* button : bandPanel->findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("20")) {
                bandButton = button;
                break;
            }
        }
    }
    check(bandButton != nullptr, QStringLiteral("20 m button exists"));
    if (bandButton) {
        int selections = 0;
        QObject::connect(&menu, &SpectrumOverlayMenu::bandSelected,
                         [&selections](const QString& band, double, const QString&) {
            if (band == QStringLiteral("20m")) {
                ++selections;
            }
        });
        bandPanel->show();
        const int before = host.received;
        sendMouse(bandButton, QEvent::MouseButtonPress, Qt::LeftButton,
                  bandButton->rect().center());
        sendMouse(bandButton, QEvent::MouseButtonRelease, Qt::LeftButton,
                  bandButton->rect().center());
        check(selections == 1 && host.received == before,
              QStringLiteral("left click still selects the band exactly once"));
    }
    return failures == 0 ? 0 : 1;
}
