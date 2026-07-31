// Regression harness for issue #4515: the Connect to Radio window must keep
// its Disconnect footer reachable when its body is taller than the screen.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/ConnectionPanel.h"

#include <QApplication>
#include <QFont>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>

#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-58s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail.c_str());
    if (!ok) {
        ++g_failed;
    }
}

int bottomInPanel(QWidget* widget, QWidget* panel)
{
    return widget->mapTo(panel, QPoint(0, widget->height())).y();
}

bool setScaledApplicationFont(QApplication& app,
                              const QFont& originalFont,
                              qreal scale,
                              std::string* detail)
{
    QFont scaledFont = originalFont;
    if (originalFont.pointSizeF() > 0.0) {
        scaledFont.setPointSizeF(originalFont.pointSizeF() * scale);
        *detail = "pointSizeF=" + std::to_string(scaledFont.pointSizeF());
    } else if (originalFont.pixelSize() > 0) {
        scaledFont.setPixelSize(
            qMax(1, qRound(static_cast<qreal>(originalFont.pixelSize()) * scale)));
        *detail = "pixelSize=" + std::to_string(scaledFont.pixelSize());
    } else {
        *detail = "font exposes neither a point nor pixel size";
        return false;
    }
    app.setFont(scaledFont);
    return true;
}

// The footer invariant, in one shipped frameless configuration at one font
// scale. Both halves of the configuration matter: the app only ever pairs the
// window flag with the custom title bar's visibility, and screenFitFrameMargins()
// branches on that flag, so a mixed state would test something nobody runs.
void checkFooterReachable(QApplication& app,
                          const QFont& originalFont,
                          qreal scale,
                          bool frameless)
{
    std::string fontDetail;
    const bool fontScaled =
        setScaledApplicationFont(app, originalFont, scale, &fontDetail);
    const std::string suffix = " scale=" + std::to_string(scale)
        + (frameless ? " frameless" : " native");
    report("font scale is representable", fontScaled, suffix + " " + fontDetail);
    if (!fontScaled) {
        return;
    }

    ConnectionPanel panel;
    panel.setFramelessMode(frameless);
    panel.setMinimumSize(ConnectionPanel::kSafeMinimumWidth,
                         ConnectionPanel::kSafeMinimumHeight);
    panel.resize(ConnectionPanel::kSafeMinimumWidth,
                 ConnectionPanel::kSafeMinimumHeight);
    panel.setConnected(true);
    panel.show();
    QApplication::processEvents();

    QScrollArea* body =
        panel.findChild<QScrollArea*>(QStringLiteral("connectionBodyScrollArea"));
    QWidget* bodyContent =
        panel.findChild<QWidget*>(QStringLiteral("connectionBodyContent"));
    QPushButton* disconnect =
        panel.findChild<QPushButton*>(QStringLiteral("connectionDisconnectButton"));

    report("scrollable connection body exists",
           body != nullptr,
           suffix);
    report("body overflows into a vertical scrollbar",
           body && body->verticalScrollBar()->maximum() > 0,
           suffix + " maximum="
               + std::to_string(body ? body->verticalScrollBar()->maximum() : -1));
    report("vertical scrollbar stays clear of the resize edge",
           body
               && body->verticalScrollBar()
                      ->mapTo(&panel,
                             QPoint(body->verticalScrollBar()->width(), 0))
                      .x()
                   <= panel.width() - 12,
           suffix);
    report("Disconnect remains visible",
           disconnect && disconnect->isVisible(),
           suffix);
    // The load-bearing assertion. Verified by mutation: putting the footer back
    // inside the scrolling body fails this at every scale while leaving the
    // rest of the harness green.
    report("Disconnect remains inside the panel",
           disconnect && bottomInPanel(disconnect, &panel) <= panel.height(),
           suffix + " bottom="
               + std::to_string(disconnect ? bottomInPanel(disconnect, &panel) : -1)
               + " panelH=" + std::to_string(panel.height()));
    report("Disconnect footer stays below the scrolling body",
           body && disconnect
               && disconnect->mapTo(&panel, QPoint()).y()
                   >= body->mapTo(&panel, QPoint(0, body->height())).y(),
           suffix);

    bool wrappedLabelsFit = bodyContent != nullptr;
    if (bodyContent) {
        const QList<QLabel*> labels = bodyContent->findChildren<QLabel*>();
        for (QLabel* label : labels) {
            if (!label->wordWrap() || !label->isVisibleTo(bodyContent)) {
                continue;
            }
            const int requiredHeight = label->heightForWidth(label->width());
            if (requiredHeight > 0 && label->height() < requiredHeight) {
                wrappedLabelsFit = false;
                break;
            }
        }
    }
    report("visible wrapped labels receive their required height",
           wrappedLabelsFit,
           suffix);

    // Behavioural, not a policy read-back: squeeze the window below the body's
    // own minimum width and require a usable horizontal scrollbar. Asserting
    // horizontalScrollBarPolicy() instead would pass just as happily on a panel
    // whose right-hand controls were simply cut off.
    if (body && bodyContent) {
        panel.setMinimumWidth(200);
        panel.resize(200, panel.height());
        QApplication::processEvents();
        report("horizontal overflow is reachable, not clipped",
               body->horizontalScrollBar()->maximum() > 0,
               suffix + " maximum="
                   + std::to_string(body->horizontalScrollBar()->maximum())
                   + " contentMinW="
                   + std::to_string(bodyContent->minimumSizeHint().width()));
    }

    panel.hide();
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-connection-panel-size-test"));
    if (!settingsProfile.isValid()) {
        return 1;
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    std::printf("ConnectionPanel screen-fit test harness (#4515)\n\n");

    const QFont originalFont = app.font();
    for (const qreal scale : {1.0, 1.25, 1.5}) {
        for (const bool frameless : {true, false}) {
            checkFooterReachable(app, originalFont, scale, frameless);
        }
    }
    app.setFont(originalFont);

    ConnectionPanel oversizedPanel;
    oversizedPanel.setFramelessMode(false);
    oversizedPanel.setMinimumSize(ConnectionPanel::kSafeMinimumWidth,
                                  ConnectionPanel::kSafeMinimumHeight);
    oversizedPanel.resize(760, 10000);
    oversizedPanel.show();
    QApplication::processEvents();
    QScreen* screen = QApplication::primaryScreen();
    oversizedPanel.fitToScreen(screen);
    QApplication::processEvents();
    const int availableHeight = screen ? screen->availableGeometry().height() : 0;
    report("screen-fit clamps an oversized panel to available height",
           availableHeight > 0 && oversizedPanel.frameGeometry().height() <= availableHeight,
           "frameH=" + std::to_string(oversizedPanel.frameGeometry().height())
               + " availableH=" + std::to_string(availableHeight));

    // The style fallback, on its own. A shown panel gets its margins from the
    // platform — offscreen reports a uniform 2 px box — so asserting top() > 0
    // there proves nothing about the branch that runs before the native window
    // exists, which is the one the first open on Windows depends on.
    {
        ConnectionPanel unshownPanel;
        unshownPanel.setFramelessMode(false);
        const QMargins estimate = unshownPanel.screenFitFrameMargins();
        const int titleBarHeight = unshownPanel.style()->pixelMetric(
            QStyle::PM_TitleBarHeight, nullptr, &unshownPanel);
        report("pre-show frame estimate reserves a title bar, not a border",
               estimate.top() >= titleBarHeight && estimate.top() > estimate.bottom(),
               "top=" + std::to_string(estimate.top())
                   + " bottom=" + std::to_string(estimate.bottom())
                   + " PM_TitleBarHeight=" + std::to_string(titleBarHeight));

        ConnectionPanel unshownFrameless;
        unshownFrameless.setFramelessMode(true);
        report("frameless mode reserves no frame at all",
               unshownFrameless.screenFitFrameMargins().isNull(),
               "top=" + std::to_string(
                            unshownFrameless.screenFitFrameMargins().top()));
    }

    if (screen) {
        const QRect available = screen->availableGeometry();
        const QPoint preferred(available.right(), available.bottom());
        const QPoint constrained =
            oversizedPanel.constrainedFrameTopLeft(preferred, available);
        const QSize frameSize = oversizedPanel.screenFitFrameSize();
        report("constrained native frame remains inside available geometry",
               available.contains(QRect(constrained, frameSize)),
               "frameX=" + std::to_string(constrained.x())
                   + " frameY=" + std::to_string(constrained.y())
                   + " frameW=" + std::to_string(frameSize.width())
                   + " frameH=" + std::to_string(frameSize.height()));

        // fitAndClampToScreen() is the show path ConnectionPanel owns itself:
        // the frameless toggle, the automation bridge, and the deferred re-fit
        // after a screen/DPI/font change all land here. It must pull a window
        // back inside the work area without otherwise relocating it.
        ConnectionPanel strayPanel;
        strayPanel.setFramelessMode(false);
        strayPanel.show();
        QApplication::processEvents();
        strayPanel.move(available.right() - 40, available.bottom() - 40);
        strayPanel.fitAndClampToScreen(screen);
        QApplication::processEvents();
        report("fit-and-clamp pulls an off-work-area window back inside",
               available.contains(strayPanel.frameGeometry()),
               "frame=" + std::to_string(strayPanel.frameGeometry().x()) + ","
                   + std::to_string(strayPanel.frameGeometry().y()) + " "
                   + std::to_string(strayPanel.frameGeometry().width()) + "x"
                   + std::to_string(strayPanel.frameGeometry().height()));
    }

    // Growth policy: a height fitToScreen() chose is reclaimed toward what the
    // body wants; a height the operator dragged to is left alone. Without the
    // first half the dialog opens pre-scrolled on a roomy screen and never
    // recovers; without the second half a deliberately short window springs
    // back on every reopen.
    //
    // Run at 150 %, because that is where the fixed 660 px opening height was
    // short of the body — at 100 % it happens to be enough and growth would go
    // untested.
    {
        std::string fontDetail;
        setScaledApplicationFont(app, originalFont, 1.5, &fontDetail);
        ConnectionPanel growPanel;
        growPanel.setFramelessMode(true);
        growPanel.show();
        QApplication::processEvents();
        growPanel.fitToScreen(screen);
        QApplication::processEvents();
        const int autoHeight = growPanel.height();
        QScrollArea* body = growPanel.findChild<QScrollArea*>(
            QStringLiteral("connectionBodyScrollArea"));
        report("auto-sized panel opens without needing to scroll",
               body && body->verticalScrollBar()->maximum() == 0,
               "height=" + std::to_string(autoHeight) + " vbarMax="
                   + std::to_string(body ? body->verticalScrollBar()->maximum() : -1));
        report("auto-sized panel grows past the nominal opening height",
               autoHeight > ConnectionPanel::kPreferredHeight,
               "height=" + std::to_string(autoHeight) + " kPreferredHeight="
                   + std::to_string(ConnectionPanel::kPreferredHeight));

        // Simulate the operator dragging it short, then reopening.
        growPanel.resize(growPanel.width(), ConnectionPanel::kSafeMinimumHeight);
        growPanel.fitToScreen(screen);
        QApplication::processEvents();
        report("a height the operator chose survives a re-fit",
               growPanel.height() == ConnectionPanel::kSafeMinimumHeight,
               "height=" + std::to_string(growPanel.height()));

        // A height fitToScreen() itself set is fair game to reclaim.
        growPanel.resize(growPanel.width(), autoHeight);
        growPanel.fitToScreen(screen);
        QApplication::processEvents();
        report("a height fit-to-screen set is reclaimed toward the body",
               growPanel.height() == autoHeight,
               "height=" + std::to_string(growPanel.height())
                   + " autoHeight=" + std::to_string(autoHeight));
    }

    app.setFont(originalFont);
    std::printf("\n%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failed == 0 ? 0 : 1;
}
