// Regression harness for issue #4515: the Connect to Radio window must keep
// its Disconnect footer reachable when its body is taller than the screen.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/ConnectionPanel.h"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QFont>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpacerItem>
#include <QStyle>
#include <QToolButton>

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

// Not every environment can host every proof: a work area with no room above
// the panel's auto-fit height cannot exercise the growth branch at all. Say so
// out loud rather than failing. #4679 was exactly a precondition the
// environment could not meet being reported as a product defect.
void reportSkipped(const char* name, const std::string& detail = {})
{
    std::printf("[SKIP] %-58s %s\n", name, detail.c_str());
}

int bottomInPanel(QWidget* widget, QWidget* panel)
{
    return widget->mapTo(panel, QPoint(0, widget->height())).y();
}

// Every word-wrapped label currently on screen has room for the lines it will
// actually draw. The failure this catches is a label handed one line's height
// for two lines of text, which Qt renders as a sliced row overlapping its
// neighbour rather than as anything obviously broken.
bool wrappedLabelsFit(QWidget* bodyContent)
{
    if (!bodyContent) {
        return false;
    }
    const QList<QLabel*> labels = bodyContent->findChildren<QLabel*>();
    for (QLabel* label : labels) {
        if (!label->wordWrap() || !label->isVisibleTo(bodyContent)) {
            continue;
        }
        const int requiredHeight = label->heightForWidth(label->width());
        if (requiredHeight > 0 && label->height() < requiredHeight) {
            return false;
        }
    }
    return true;
}

// Put the panel on the manual/VPN page with "Advanced: choose the VPN source
// path" open. The section only auto-reveals when the host has more than one
// IPv4 candidate, which a test machine cannot be relied on to have, so drive
// the widgets directly and keep the geometry deterministic.
bool expandManualAdvancedSection(ConnectionPanel& panel)
{
    auto* manualMode =
        panel.findChild<QAbstractButton*>(QStringLiteral("connectionManualModeButton"));
    auto* toggle =
        panel.findChild<QToolButton*>(QStringLiteral("connectionManualAdvancedToggle"));
    auto* section =
        panel.findChild<QWidget*>(QStringLiteral("connectionManualAdvancedSection"));
    if (!manualMode || !toggle || !section) {
        return false;
    }
    manualMode->click();
    toggle->setVisible(true);
    toggle->setChecked(true);
    section->setVisible(true);
    QApplication::processEvents();
    return true;
}

void checkIcomManualGuidanceIsModelNeutral()
{
    ConnectionPanel panel;
    auto* manualMode =
        panel.findChild<QAbstractButton*>(QStringLiteral("connectionManualModeButton"));
    auto* radioType =
        panel.findChild<QComboBox*>(QStringLiteral("connectionManualRadioType"));
    auto* hint =
        panel.findChild<QLabel*>(QStringLiteral("connectionManualHintLabel"));
    auto* ip = panel.findChild<QLineEdit*>(QStringLiteral("connectionManualIp"));

    if (manualMode) {
        manualMode->click();
    }
    const int icomIndex = radioType
        ? radioType->findData(QString::fromLatin1(ConnectionPanel::kFamilyIcom))
        : -1;
    if (radioType && icomIndex >= 0) {
        radioType->setCurrentIndex(icomIndex);
    }
    QApplication::processEvents();

    const QString guidance = hint ? hint->text() : QString();
    report("Icom IP guidance remains credential-focused",
           guidance.contains(QStringLiteral("configured for network control")));
    report("Icom IP guidance does not name one radio model",
           !guidance.contains(QStringLiteral("IC-705")));
    report("Icom IP guidance omits model-specific MOD settings",
           !guidance.contains(QStringLiteral("MOD Input"))
               && !guidance.contains(QStringLiteral("DATA OFF MOD"))
               && !guidance.contains(QStringLiteral("DATA MOD")));
    report("Icom address example is model-neutral",
           ip && !ip->placeholderText().contains(QStringLiteral("IC-705"),
                                                  Qt::CaseInsensitive));
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

    report("visible wrapped labels receive their required height",
           wrappedLabelsFit(bodyContent),
           suffix);

    // The manual/VPN page with Advanced open is the tallest state this dialog
    // has, and it is the one an operator reported squashed: the source-path
    // combo drawn as a sliver under a hint clipped to a sliced single line.
    // Worth its own pass because the section is collapsed by default, so the
    // sweep above skips both widgets via isVisibleTo().
    report("manual page reveals its Advanced source-path section",
           expandManualAdvancedSection(panel),
           suffix);
    report("visible wrapped labels still fit with Advanced expanded",
           wrappedLabelsFit(bodyContent),
           suffix);

    auto* sourcePath =
        panel.findChild<QComboBox*>(QStringLiteral("connectionManualSourcePath"));
    report("source-path combo is allocated the height it asks for",
           sourcePath && sourcePath->isVisible()
               && sourcePath->height() >= sourcePath->minimumSizeHint().height(),
           suffix + " h="
               + std::to_string(sourcePath ? sourcePath->height() : -1)
               + " wants="
               + std::to_string(sourcePath ? sourcePath->minimumSizeHint().height() : -1));

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

// A networked Icom is reached through the manual-IP page rather than broadcast
// discovery. It must carry the same routed marker as manual Flex/HL2 sessions
// or MainWindow will discard LastRoutedRadioIp and startup auto-connect will
// have no host to dial.
void checkIcomUsesRoutedRetention()
{
    auto& settings = AppSettings::instance();
    const QString previousProfiles =
        settings.value(QStringLiteral("RoutedProfilesJson"), QStringLiteral("{}")).toString();
    settings.setValue(
        QStringLiteral("RoutedProfilesJson"),
        QStringLiteral(
            R"({"127.0.0.1":{"identity":{"target_address":"127.0.0.1","family":"icom"},"bind":{"mode":"auto"}}})"));
    settings.save();

    ConnectionPanel panel;
    QComboBox* radioType = panel.findChild<QComboBox*>(
        QStringLiteral("connectionManualRadioType"));
    QLineEdit* user = panel.findChild<QLineEdit*>(
        QStringLiteral("connectionManualIcomUser"));
    QLineEdit* password = panel.findChild<QLineEdit*>(
        QStringLiteral("connectionManualIcomPassword"));

    const int flexIndex = radioType
        ? radioType->findData(QString::fromLatin1(ConnectionPanel::kFamilyFlex))
        : -1;
    if (radioType && flexIndex >= 0) {
        radioType->setCurrentIndex(flexIndex);
    }
    if (user) {
        user->setText(QStringLiteral("test-user"));
    }
    if (password) {
        password->setText(QStringLiteral("test-password"));
    }

    bool emitted = false;
    RadioInfo requested;
    QObject::connect(&panel, &ConnectionPanel::connectRequested,
                     [&emitted, &requested](const RadioInfo& info) {
        emitted = true;
        requested = info;
    });
    panel.probeRadio(QStringLiteral("127.0.0.1"), /*restoreSavedFamily=*/true);

    report("retained route restores the Icom radio family",
           radioType
               && radioType->currentData().toString()
                      == QString::fromLatin1(ConnectionPanel::kFamilyIcom));
    report("manual Icom emits a connection request", emitted);
    report("manual Icom is retained as a routed radio",
           emitted && requested.isRouted);
    report("manual Icom keeps its stable host identity",
           emitted && requested.serial == QStringLiteral("icom:127.0.0.1"));
    report("manual Icom carries the selected source-path settings",
           emitted && requested.bindSettings.mode == RadioBindMode::Auto);

    settings.setValue(QStringLiteral("RoutedProfilesJson"), previousProfiles);
    settings.save();
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
    // Font scaling is not a deterministic way to reach the growth branch: the
    // platform font and style metrics may still fit at the nominal opening
    // height. Instead, first let fitToScreen() own the current height, then
    // drive the scroll body's preferred height above it and back down again
    // with a harness-only spacer. Both directions are proved as behaviour — a
    // scrollbar before the re-fit, the pixels handed back after the spacer
    // goes away — rather than assumed from a particular metric.
    {
        ConnectionPanel growPanel;
        growPanel.setFramelessMode(true);
        growPanel.show();
        QApplication::processEvents();
        growPanel.fitToScreen(screen);
        QApplication::processEvents();
        const int initialAutoHeight = growPanel.height();
        QScrollArea* body = growPanel.findChild<QScrollArea*>(
            QStringLiteral("connectionBodyScrollArea"));
        QWidget* bodyContent = growPanel.findChild<QWidget*>(
            QStringLiteral("connectionBodyContent"));

        // Forcing growth needs room above the height fitToScreen() just chose.
        // Offscreen — what CTest runs, and what #4679 was reported from — has
        // hundreds of pixels spare; a hand run against a work area already
        // clamping the panel has none, and there the branch is unreachable
        // rather than broken.
        constexpr int kGrowthHeadroom = 40;
        const int growthWorkAreaHeight =
            screen ? screen->availableGeometry().height() : 0;

        if (growthWorkAreaHeight - initialAutoHeight < kGrowthHeadroom) {
            reportSkipped("growth proof needs headroom above the auto-fit height",
                          "autoHeight=" + std::to_string(initialAutoHeight)
                              + " workAreaH=" + std::to_string(growthWorkAreaHeight));
        } else {
            const int forcedPreferredHeight = initialAutoHeight + kGrowthHeadroom;
            QSpacerItem* spacer = nullptr;
            if (body && bodyContent && bodyContent->layout()) {
                const int chromeHeight =
                    growPanel.sizeHint().height() - body->sizeHint().height();
                const int naturalPreferredHeight =
                    chromeHeight + bodyContent->sizeHint().height();
                spacer = new QSpacerItem(
                    0,
                    qMax(1, forcedPreferredHeight - naturalPreferredHeight),
                    QSizePolicy::Minimum,
                    QSizePolicy::Fixed);
                bodyContent->layout()->addItem(spacer);
                QApplication::processEvents();
            }
            report("growth setup overflows an auto-fit-owned height",
                   body && body->verticalScrollBar()->maximum() > 0,
                   "height=" + std::to_string(initialAutoHeight) + " target="
                       + std::to_string(forcedPreferredHeight) + " vbarMax="
                       + std::to_string(body ? body->verticalScrollBar()->maximum() : -1));

            growPanel.fitToScreen(screen);
            QApplication::processEvents();
            const int grownHeight = growPanel.height();
            report("auto-sized panel opens without needing to scroll",
                   body && body->verticalScrollBar()->maximum() == 0,
                   "height=" + std::to_string(grownHeight) + " vbarMax="
                       + std::to_string(body ? body->verticalScrollBar()->maximum() : -1));
            report("auto-sized panel grows from its prior auto-fit height",
                   grownHeight > initialAutoHeight,
                   "height=" + std::to_string(grownHeight) + " initialAutoHeight="
                       + std::to_string(initialAutoHeight));

            // The other direction, and the only non-vacuous route to it: take
            // the extra height back out of the body while fitToScreen() still
            // owns the window height, and require the re-fit to hand the pixels
            // back. Resizing to a height fitToScreen() already chose and
            // re-fitting proves nothing — that lands on the shrink path, which
            // stays green with the production growth branch deleted.
            if (spacer && bodyContent && bodyContent->layout()) {
                bodyContent->layout()->removeItem(spacer);
                delete spacer;
                QApplication::processEvents();
            }
            growPanel.fitToScreen(screen);
            QApplication::processEvents();
            report("a height fit-to-screen set is reclaimed toward the body",
                   growPanel.height() == initialAutoHeight,
                   "height=" + std::to_string(growPanel.height())
                       + " initialAutoHeight=" + std::to_string(initialAutoHeight));
        }

        // Simulate the operator dragging it short, then reopening.
        growPanel.resize(growPanel.width(), ConnectionPanel::kSafeMinimumHeight);
        growPanel.fitToScreen(screen);
        QApplication::processEvents();
        report("a height the operator chose survives a re-fit",
               growPanel.height() == ConnectionPanel::kSafeMinimumHeight,
               "height=" + std::to_string(growPanel.height()));
    }

    app.setFont(originalFont);
    checkIcomManualGuidanceIsModelNeutral();
    checkIcomUsesRoutedRetention();
    std::printf("\n%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failed == 0 ? 0 : 1;
}
