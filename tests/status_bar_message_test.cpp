#include "gui/StatusBarMessageOverlay.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QStatusBar>
#include <QTest>

#include <cstdio>

namespace {
int failures = 0;
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
    QMainWindow window;
    auto* input = new QLineEdit(&window);
    window.setCentralWidget(input);
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
    AetherSDR::StatusBarMessageOverlay notice(bar, row);
    window.resize(1024, 500);
    window.show();
    input->setFocus();
    QApplication::processEvents();
    const QRect rowGeometry = row->geometry();
    const QRect stationGeometry = station->geometry();
    const QRect txGeometry = tx->geometry();
    const QSize windowMinimum = window.minimumSizeHint();
    QWidget* focus = QApplication::focusWidget();

    bool rowVisible = true;
    bool geometryStable = true;
    bool messageVisible = true;
    for (int i = 0; i < 100; ++i) {
        bar->showMessage(QStringLiteral("Client connected"), 5000);
        QApplication::processEvents();
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
        geometryStable &= row->geometry() == rowGeometry && station->geometry() == stationGeometry
            && tx->geometry() == txGeometry;
    }
    check(rowVisible, "100 message cycles leave the complete status row visible");
    check(geometryStable, "100 message cycles do not move or reparent controls");
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
    window.resize(400, 300);
    QApplication::processEvents();
    check(notice.geometry().left() >= 0 && notice.geometry().right() < window.width(),
          "notice stays inside a resized window");
    bar->hide();
    QApplication::processEvents();
    check(!notice.isVisible(), "notice hides with the status bar in minimal mode");
    bar->show();
    QApplication::processEvents();
    check(notice.isVisible(), "an unexpired message returns with the status bar");
    bar->showMessage(QStringLiteral("Saved"), 20);
    QTest::qWait(80);
    check(!notice.isVisible() && bar->currentMessage().isEmpty(),
          "the existing status message timer expires the notice");
    return failures ? 1 : 0;
}
