#include "gui/StatusBarMessageOverlay.h"
#include "gui/StatusBarLayout.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <QStatusBar>
#include <QTest>

#include <cstdio>

namespace {
int failures = 0;
QStringList announcements;
void accessibleUpdate(QAccessibleEvent* event)
{
    if (event->type() == QAccessible::Announcement) {
        announcements.append(static_cast<QAccessibleAnnouncementEvent*>(event)->message());
    }
}
void check(bool condition, const char* description)
{
    std::printf("[%s] %s\n", condition ? "OK" : "FAIL", description);
    if (!condition) {
        ++failures;
    }
}
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    const auto previousUpdateHandler = QAccessible::installUpdateHandler(accessibleUpdate);
    QAccessible::setActive(true);
    QMainWindow window;
    auto* surface = new QWidget(&window);
    window.setCentralWidget(surface);
    auto* input = new QLineEdit(surface);
    input->setGeometry(0, 0, 100, 24);
    auto* zoom = new QPushButton(QStringLiteral("Zoom"), surface);
    auto* scroll = new QPushButton(QStringLiteral("Scroll"), surface);
    QStatusBar* bar = window.statusBar();
    bar->setFixedHeight(46);
    bar->setSizeGripEnabled(false);
    auto* row = new QWidget(bar);
    auto* layout = new QHBoxLayout(row);
    auto* station = new QLabel(QStringLiteral("FLEX-8600"), row);
    auto* tx = new QLabel(QStringLiteral("TX"), row);
    layout->addWidget(station);
    layout->addStretch();
    layout->addWidget(tx);
    bar->addPermanentWidget(row, 1);
    AetherSDR::StatusBarMessageOverlay notice(bar, surface);
    window.resize(1024, 500);
    window.show();
    input->setFocus();
    QApplication::processEvents();
    zoom->setGeometry(4, surface->height() - 50, 46, 46);
    scroll->setGeometry(surface->width() - 44, surface->height() - 50, 40, 46);
    AetherSDR::updateStationLabelMinimumWidth(station);
    auto remeasure = [&] {
        AetherSDR::updateStatusBarWindowMinimum(&window, row, nullptr, false, 1920);
    };
    remeasure();
    QApplication::processEvents();
    const QRect rowGeometry = row->geometry();
    const QRect stationGeometry = station->geometry();
    const QRect txGeometry = tx->geometry();
    const QSize windowMinimum = window.minimumSizeHint();
    const int stationMinimum = station->minimumWidth();
    QWidget* focus = QApplication::focusWidget();

    bool rowVisible = true;
    bool geometryStable = true;
    bool messageVisible = true;
    for (int i = 0; i < 100; ++i) {
        bar->showMessage(QStringLiteral("Client connected"), 5000);
        QApplication::processEvents();
        AetherSDR::updateStationLabelMinimumWidth(station);
        remeasure();
        rowVisible &= row->isVisible() && station->isVisible() && tx->isVisible();
        geometryStable &= row->geometry() == rowGeometry && station->geometry() == stationGeometry
            && tx->geometry() == txGeometry && station->parentWidget() == row;
        messageVisible &= notice.isVisible() && notice.text() == QStringLiteral("Client connected");
        if (i == 0 && !messageVisible) {
            std::printf("notice visible=%d text='%s' width=%d\n", notice.isVisible(),
                        qPrintable(notice.text()), notice.width());
        }
        bar->clearMessage();
        QApplication::processEvents();
        AetherSDR::updateStationLabelMinimumWidth(station);
        remeasure();
        geometryStable &= row->geometry() == rowGeometry && station->geometry() == stationGeometry
            && tx->geometry() == txGeometry;
    }
    check(rowVisible, "100 message cycles leave the complete status row visible");
    check(geometryStable, "100 message cycles do not move or reparent controls");
    check(station->minimumWidth() == stationMinimum,
          "production station sizing does not ratchet during message cycles");
    check(messageVisible, "messages appear in the notice");
    check(window.minimumSizeHint() == windowMinimum && window.size() == QSize(1024, 500),
          "message cycles leave window size and minimum unchanged");
    check(!notice.isVisible() && notice.accessibleName().isEmpty(),
          "clearing the message hides and clears the notice");
    check(QApplication::focusWidget() == focus && notice.focusPolicy() == Qt::NoFocus
              && notice.testAttribute(Qt::WA_TransparentForMouseEvents),
          "notices leave focus and pointer input alone");

    const QString longMessage = QStringLiteral("A long status message ").repeated(100);
    bar->showMessage(longMessage);
    QApplication::processEvents();
    check(notice.width() <= 640 && notice.geometry().bottom() < bar->geometry().top(),
          "notice stays compact and above the controls");
    check(notice.accessibleName() == longMessage && notice.text().size() < longMessage.size(),
          "long text is elided visually and available in full to accessibility");
    const QPoint anchor = notice.pos();
    bar->showMessage(QStringLiteral("Saved"));
    check(notice.pos() == anchor, "short and long messages share a steady anchor");
    bar->showMessage(longMessage);
    auto controlsClear = [&] {
        const QRect zoomRect(zoom->mapTo(&window, QPoint()), zoom->size());
        const QRect scrollRect(scroll->mapTo(&window, QPoint()), scroll->size());
        return !notice.geometry().intersects(zoomRect) && !notice.geometry().intersects(scrollRect);
    };
    check(controlsClear(), "long notices clear the surface's zoom and scroll controls");
    window.setMinimumSize(0, 0);
    window.resize(400, 300);
    QApplication::processEvents();
    zoom->move(4, surface->height() - 50);
    scroll->move(surface->width() - 44, surface->height() - 50);
    bar->showMessage(longMessage + QStringLiteral(" Narrow"));
    check(notice.geometry().left() >= 0 && notice.geometry().right() < window.width(),
          "notice stays inside a resized window");
    check(controlsClear() && notice.width() < 400,
          "narrow notices elide between controls instead of covering them");
    bar->hide();
    QApplication::processEvents();
    check(!notice.isVisible(), "notice hides with the status bar in minimal mode");
    bar->show();
    QApplication::processEvents();
    check(notice.isVisible(), "an unexpired message returns with the status bar");
    bar->showMessage(QStringLiteral("Saved"), 20);
    check(QTest::qWaitFor([&] { return !notice.isVisible() && bar->currentMessage().isEmpty(); }, 2000),
          "the existing status message timer expires the notice");

    window.resize(1400, 500);
    auto* accessory = new QLabel(QStringLiteral("Accessory"), row);
    accessory->setFixedWidth(1200);
    layout->addWidget(accessory);
    accessory->show();
    bar->showMessage(QStringLiteral("Client connected"));
    remeasure();
    check(window.minimumWidth() > 1200,
          "an accessory arriving during a message raises the production window floor");
    accessory->hide();
    remeasure();
    check(window.minimumWidth() == 1024,
          "an accessory disappearing during a message lowers the production window floor");
    bar->clearMessage();

    bar->showMessage(QStringLiteral("Keyboard shortcuts are off"), 10000);
    QApplication::processEvents();
    const QRect beforeShortcut = tx->geometry();
    bar->showMessage(QStringLiteral("Saved"));
    QApplication::processEvents();
    check(tx->geometry() == beforeShortcut,
          "shortcut-disabled and ordinary notices use the same stationary row");

    announcements.clear();
    for (int i = 0; i < 10; ++i) {
        bar->showMessage(QStringLiteral("Step: %1 Hz").arg(i));
        QTest::qWait(10);
    }
    check(announcements.isEmpty(), "rapid updates do not announce intermediate messages");
    check(QTest::qWaitFor([&] { return announcements.size() == 1; }, 2000)
              && announcements.last() == QStringLiteral("Step: 9 Hz"),
          "accessibility announces only the settled final message");
    announcements.clear();
    bar->showMessage(QStringLiteral("Already expired"));
    bar->clearMessage();
    QTest::qWait(150);
    check(announcements.isEmpty(), "clearing cancels a pending announcement");
    // A notice kept off screen (minimal mode) is still announced.
    bar->hide();
    QApplication::processEvents();
    announcements.clear();
    bar->showMessage(QStringLiteral("Keyboard shortcuts are off"), 5000);
    check(!notice.isVisible()
              && QTest::qWaitFor([&] { return announcements.contains(QStringLiteral("Keyboard shortcuts are off")); }, 2000),
          "a notice hidden with the status bar is still announced");
    bar->clearMessage();
    bar->show();
    QApplication::processEvents();

    // With the pan surface hidden (workspace canvas mounted), the notice uses
    // the alternate surface and ignores controls in other top-level windows.
    auto* canvas = new QWidget(&window);
    auto* canvasButton = new QPushButton(QStringLiteral("Canvas"), canvas);
    surface->hide();
    // Narrower than the window: the notice must stay within the canvas.
    canvas->setGeometry(surface->x(), surface->y(), 300, surface->height());
    canvas->show();
    canvasButton->setGeometry(4, canvas->height() - 50, 46, 46);
    // A floating pan window is a top-level child; a button across its bottom
    // would, if mapped into this window, cover the whole notice band.
    auto* floating = new QWidget(canvas, Qt::Window);
    floating->setGeometry(0, 0, canvas->width(), canvas->height());
    auto* floatingButton = new QPushButton(QStringLiteral("Floating"), floating);
    floatingButton->setGeometry(0, canvas->height() - 60, canvas->width(), 60);
    floating->show();
    notice.setAlternateSurface(canvas);
    bar->showMessage(longMessage, 5000);
    QApplication::processEvents();
    const QRect canvasRect(canvas->mapTo(&window, QPoint()), canvas->size());
    const QRect canvasButtonRect(canvasButton->mapTo(&window, QPoint()), canvasButton->size());
    check(notice.isVisible() && canvasRect.contains(notice.geometry())
              && !notice.geometry().intersects(canvasButtonRect),
          "the canvas surface places the notice clear of its controls");
    check(notice.isVisible() && notice.width() > 200,
          "controls in other top-level windows do not take the notice's gap");
    floating->hide();
    bar->clearMessage();

    QAccessible::installUpdateHandler(previousUpdateHandler);
    return failures ? 1 : 0;
}
