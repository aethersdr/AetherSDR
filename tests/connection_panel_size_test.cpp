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
        std::string fontDetail;
        const bool fontScaled =
            setScaledApplicationFont(app, originalFont, scale, &fontDetail);
        report("font scale is representable",
               fontScaled,
               " scale=" + std::to_string(scale) + " " + fontDetail);
        if (!fontScaled) {
            continue;
        }

        ConnectionPanel panel;
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

        const std::string suffix = " scale=" + std::to_string(scale);
        report("scrollable connection body exists",
               body != nullptr,
               suffix);
        report("body overflows into a vertical scrollbar",
               body && body->verticalScrollBar()->maximum() > 0,
               suffix + " maximum="
                   + std::to_string(body ? body->verticalScrollBar()->maximum() : -1));
        report("horizontal overflow remains reachable",
               body
                   && body->horizontalScrollBarPolicy() == Qt::ScrollBarAsNeeded,
               suffix);
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

        panel.hide();
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
    const QMargins frameMargins = oversizedPanel.screenFitFrameMargins();
    report("native-frame screen fitting accounts for title-bar margins",
           frameMargins.top() > 0,
           "top=" + std::to_string(frameMargins.top()));
    if (screen) {
        const QRect available = screen->availableGeometry();
        const QPoint preferred(available.right(), available.bottom());
        const QPoint constrained =
            oversizedPanel.constrainedFrameTopLeft(preferred, available);
        const QSize frameSize(
            oversizedPanel.width() + frameMargins.left() + frameMargins.right(),
            oversizedPanel.height() + frameMargins.top() + frameMargins.bottom());
        report("constrained native frame remains inside available geometry",
               available.contains(QRect(constrained, frameSize)),
               "frameX=" + std::to_string(constrained.x())
                   + " frameY=" + std::to_string(constrained.y())
                   + " frameW=" + std::to_string(frameSize.width())
                   + " frameH=" + std::to_string(frameSize.height()));
    }

    std::printf("\n%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failed == 0 ? 0 : 1;
}
