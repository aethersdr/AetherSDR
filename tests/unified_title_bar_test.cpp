// The unified 52 px title bar: geometry, radio tabs, and the accessibility
// contract the design leans on.
//
// WHY THESE ASSERTIONS AND NOT OTHERS
// -----------------------------------
// Three of these pin down defects that actually shipped during development and
// were invisible to every other check:
//
//   * BAR HEIGHT / OFFSET.  The bar replaced a 32 px strip whose background
//     token was identical to the window backdrop, which hid a 32 px band of
//     reserved-but-empty space above it for as long as the two matched.  Give
//     the bar its own colour and the band becomes a dead row next to the window
//     controls.  `offsetInWindow` is asserted at 0 because that band is exactly
//     what a regression would restore.
//
//   * STATUS IS NOT COLOUR-ONLY.  WCAG 1.4.1, and this project's audience,
//     forbid encoding state in a dot's colour alone.  The tab's rendered second
//     line and its accessible name both have to name the state in words.  A
//     screenshot review passes happily without them, so the guard lives here.
//
//   * UNICODE ENCODING.  The status line's U+00B7 and the discovery popover's
//     U+2026 must be Unicode escapes or source characters inside QStringLiteral.
//     Raw UTF-8 bytes land as mojibake, look like a font problem, and are only
//     visible if something compares the actual string.
//
// Runs headless (offscreen); asserts on widget state.  The one pixel check is
// the bar's own fill: a stylesheet rule that never matches (the bar shipped that
// way — `TitleBar {}` cannot select a namespaced class) is invisible to every
// state assertion, so only the rendered bar can catch it.

#include "gui/RadioTabBar.h"
#include "gui/TitleBar.h"
#include "gui/WindowCaptionButtons.h"
#include "gui/WindowChrome.h"
#include "core/ThemeManager.h"

#include <QAbstractButton>
#include <QApplication>
#include <QFocusEvent>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QMenuBar>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QTest>
#include <QWheelEvent>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;

static void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static void checkEqual(int got, int want, const char* what)
{
    if (got != want) {
        std::fprintf(stderr, "FAIL: %s (got %d, want %d)\n", what, got, want);
        ++g_failures;
    }
}

static int paintedPixelCount(const QImage& image)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(line[x]) > 0) {
                ++count;
            }
        }
    }
    return count;
}

static RadioTab* tabWithId(TitleBar& bar, const QString& id)
{
    const auto tabs = bar.findChildren<RadioTab*>();
    for (RadioTab* t : tabs) {
        if (t->entry().id == id) {
            return t;
        }
    }
    return nullptr;
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    QWidget host;
    // MainWindow carries a window-wide `QWidget { background-color }` rule,
    // which styles every plain QWidget container inside the bar.  Mirror it so
    // the bar is tested under the cascade it actually ships in.
    host.setStyleSheet(QStringLiteral("QWidget { background-color: %1; }")
        .arg(ThemeManager::instance().color(QStringLiteral("color.background.app")).name()));
    // ── Window-chrome policy per platform (pure; runs on any host) ─────────
    {
        const Qt::WindowFlags base = Qt::Window;
        const QString win = QStringLiteral("windows");
        const Qt::WindowFlags winOn = WindowChrome::chromeFlags(base, true, win);
        check(winOn.testFlag(Qt::ExpandedClientAreaHint) && winOn.testFlag(Qt::CustomizeWindowHint),
              "Windows frameless uses Qt's expanded client area");
        check(!winOn.testFlag(Qt::WindowTitleHint) && !winOn.testFlag(Qt::WindowMinimizeButtonHint)
                  && !winOn.testFlag(Qt::WindowMaximizeButtonHint)
                  && !winOn.testFlag(Qt::WindowCloseButtonHint),
              "Windows frameless drops the hints that make Qt draw its own title and buttons");
        check(!WindowChrome::usesNativeCaption(winOn, win),
              "Windows frameless shows the bar's own caption buttons");
        const Qt::WindowFlags winOff = WindowChrome::chromeFlags(winOn, false, win);
        check(!winOff.testFlag(Qt::ExpandedClientAreaHint) && !winOff.testFlag(Qt::CustomizeWindowHint)
                  && winOff.testFlag(Qt::WindowTitleHint) && winOff.testFlag(Qt::WindowCloseButtonHint),
              "turning frameless off on Windows restores the system title and buttons");
        check(WindowChrome::usesNativeCaption(winOff, win),
              "Windows system decorations hide the bar's caption buttons");
        const Qt::WindowFlags macOn = WindowChrome::chromeFlags(base, true, QStringLiteral("cocoa"));
        check(WindowChrome::usesNativeCaption(macOn, QStringLiteral("cocoa")),
              "macOS frameless keeps the native traffic lights");
        const Qt::WindowFlags linuxOn = WindowChrome::chromeFlags(base, true, QStringLiteral("wayland"));
        check(linuxOn.testFlag(Qt::FramelessWindowHint)
                  && !WindowChrome::usesNativeCaption(linuxOn, QStringLiteral("wayland")),
              "Linux frameless draws the bar's caption buttons");
    }

    WindowChrome::configure(&host, true);
    auto* bar = new TitleBar(&host);
    host.resize(1400, 200);
    host.show();
    app.processEvents();

#ifdef Q_OS_MAC
    QWidget uncreatedHost;
    mac::updateNativeTitleVisibility(&uncreatedHost);
    check(mac::nativeCaptionBounds(&uncreatedHost).isEmpty(), "uncreated window has no native caption bounds");
    check(!uncreatedHost.internalWinId(), "native caption inspection does not create a window");
    if (QGuiApplication::platformName() != QStringLiteral("cocoa")) {
        check(mac::nativeCaptionBounds(&host).isEmpty(), "offscreen window IDs never reach AppKit");
    } else {
        const QRectF controls = mac::nativeCaptionBounds(&host);
        QWidget* mark = bar->findChild<QWidget*>(QStringLiteral("brandMark"));
        check(!controls.isEmpty() && mark, "native caption and brand expose measurable bounds");
        if (!controls.isEmpty() && mark) {
            checkEqual(mark->x() - qCeil(controls.right()), 16, "brand follows native controls with one 16 px gap");
            check(qAbs(controls.center().y() - 26.0) <= 1.0, "native traffic lights center in the 52 px bar");
        }
    }
#endif

    // ── Geometry ────────────────────────────────────────────────────────────
    checkEqual(bar->height(), TitleBar::kUnifiedBarHeight,
               "bar is kUnifiedBarHeight tall");
    checkEqual(TitleBar::kUnifiedBarHeight, 52, "kUnifiedBarHeight is 52");

    const QVariantMap state = bar->barState();
    checkEqual(state.value(QStringLiteral("height")).toInt(), 52,
               "barState reports 52 px");
    checkEqual(state.value(QStringLiteral("offsetInWindow")).toInt(), 0,
               "nothing reserves a strip above the bar");

    // The bar paints its own fill and bottom hairline, opaque, so a tab's
    // heartbeat repaint stops at the bar instead of repainting the window.
    {
        check(bar->testAttribute(Qt::WA_OpaquePaintEvent),
              "bar paints opaquely so child repaints stop at the bar");
        const QImage painted = bar->grab().toImage();
        const QColor app = ThemeManager::instance().color(bar, QStringLiteral("color.background.app"));
        // Sample inside the bar's 16 px left margin, where no child can sit.
        const QColor fill = painted.pixelColor(4, painted.height() / 2);
        const QColor edge = painted.pixelColor(4, painted.height() - 1);
        check(fill.alpha() == 255, "bar fill is opaque");
        check(fill.rgb() != app.rgb(), "bar fill differs from the window background");
        check(edge.rgb() != fill.rgb(), "bar draws a bottom border distinct from its fill");

        // No container inside the bar may repaint the window colour over it.
        for (const char* name : {"titleBarDragGutter", "titleBarAudioCluster", "radioTabScroller"}) {
            QWidget* w = bar->findChild<QWidget*>(QLatin1String(name));
            check(w != nullptr, "bar container exists");
            if (!w || w->width() <= 0) continue;
            const QPoint at = w->mapTo(bar, QPoint(w->width() / 2, 2));
            const qreal dpr = painted.devicePixelRatio();
            const QColor seen = painted.pixelColor(int(at.x() * dpr), int(at.y() * dpr));
            if (seen.rgb() != fill.rgb()) {
                std::fprintf(stderr, "  %s paints %s over the bar\n", name, qPrintable(seen.name()));
            }
            check(seen.rgb() == fill.rgb(), "bar containers are transparent over the bar fill");
        }
    }

    // ── Brand ───────────────────────────────────────────────────────────────
    const QVariantMap brand = state.value(QStringLiteral("brand")).toMap();
    check(brand.value(QStringLiteral("wordmark")).toString()
              == QLatin1String("AetherSDR"),
          "wordmark reads AetherSDR");
    check(brand.value(QStringLiteral("logoLoaded")).toBool(),
          "brand logo resource resolves (qrc alias :/images/logo-96.png)");

    // ── Audio cluster ───────────────────────────────────────────────────────
    // 64 px is the design's slider width; the app-wide default is wider, so a
    // stylesheet regression that dropped the per-bar metrics would show here.
    const QVariantMap audio = state.value(QStringLiteral("audio")).toMap();
    checkEqual(audio.value(QStringLiteral("sliderWidth")).toInt(), 64,
               "audio-cluster sliders are 64 px wide");

    // ── Window controls ─────────────────────────────────────────────────────
    WindowCaptionButtons* caption = bar->captionButtons();
    check(caption != nullptr, "the bar owns caption controls");
    if (caption) {
        const QVariantMap chrome = caption->state();
        for (const char* role : {"close", "minimize", "maximize"}) {
            const QVariantMap b = chrome.value(QLatin1String(role)).toMap();
            check(!b.value(QStringLiteral("accessibleName")).toString().isEmpty(),
                  "every caption control is screen-reader named");
        }
        const auto buttons = caption->findChildren<CaptionButton*>();
        checkEqual(int(buttons.size()), 3, "three caption controls");
        for (CaptionButton* b : buttons) {
            check(b->focusPolicy() != Qt::NoFocus,
                  "every caption control is keyboard-reachable");
        }
    }

    struct CaptionContract {
        const char* stateName;
        int buttonWidth;
        int buttonHeight;
    };
    const CaptionContract captionContracts[] = {
        {"shared", 36, 36},
    };
    for (const CaptionContract& contract : captionContracts) {
        WindowCaptionButtons controls;
        controls.adjustSize();
        controls.show();
        app.processEvents();
        const QVariantMap controlState = controls.state();
        check(controlState.value(QStringLiteral("style")).toString()
                  == QLatin1String(contract.stateName),
              "caption cluster reports the shared fallback style");
        for (const char* role : {"close", "minimize", "maximize"}) {
            const QVariantMap button = controlState.value(QLatin1String(role)).toMap();
            checkEqual(button.value(QStringLiteral("width")).toInt(),
                       contract.buttonWidth,
                       "caption button width matches its platform contract");
            checkEqual(button.value(QStringLiteral("height")).toInt(),
                       contract.buttonHeight,
                       "caption button height matches its platform contract");
        }
        QImage rendered(controls.size(), QImage::Format_ARGB32_Premultiplied);
        rendered.fill(Qt::transparent);
        controls.render(&rendered);
        check(paintedPixelCount(rendered) > 0,
              "caption cluster paints visible platform controls");
    }

    // Minimal mode hides the dock trio — and the separator that leads it,
    // which once dangled alone at the end of the bar because the member that
    // should have hidden it was never assigned.
    {
        auto visibleSeparators = [bar]() {
            int n = 0;
            for (QFrame* f : bar->findChildren<QFrame*>(QString(), Qt::FindDirectChildrenOnly)) {
                if (f->isVisible() && f->width() == 1) ++n;
            }
            return n;
        };
        check(visibleSeparators() > 0, "dock separator shows in the full bar");
        bar->setMinimalMode(true);
        app.processEvents();
        checkEqual(visibleSeparators(), 0, "minimal mode leaves no dangling separator");
        bar->setMinimalMode(false);
        app.processEvents();
        check(visibleSeparators() > 0, "dock separator returns after minimal mode");
    }

    // ── Radio tabs ──────────────────────────────────────────────────────────
    RadioTabEntry connected;
    connected.id = QStringLiteral("SERIAL-1");
    connected.name = QStringLiteral("Hermes-Lite 2");
    connected.transport = QStringLiteral("192.168.1.21");
    connected.status = RadioTabStatus::Connected;
    connected.canRename = true;

    RadioTabEntry inUse;
    inUse.id = QStringLiteral("SERIAL-2");
    inUse.name = QStringLiteral("FLEX-6600");
    inUse.transport = QStringLiteral("SmartLink");
    inUse.status = RadioTabStatus::InUse;

    bar->setRadioTabs({connected, inUse});
    bar->setActiveRadio(connected.id);

    RadioTab* connectedTab = tabWithId(*bar, connected.id);
    RadioTab* inUseTab = tabWithId(*bar, inUse.id);
    check(connectedTab != nullptr && inUseTab != nullptr, "a tab per radio");

    if (connectedTab && inUseTab) {
        check(connectedTab->isChecked(), "the active radio's tab is checked");
        check(!inUseTab->isChecked(), "only the active radio's tab is checked");
        check(connectedTab->focusPolicy() != Qt::NoFocus,
              "radio tabs are keyboard-reachable");

        // Status in words, not just in the dot's colour.
        check(connectedTab->accessibleDescription().contains(
                  QLatin1String("connected")),
              "connected tab spells its state on the rendered status line");
        check(connectedTab->accessibleName().contains(QLatin1String("connected")),
              "connected tab spells its state in its accessible name");
        check(inUseTab->accessibleDescription().contains(QLatin1String("in use")),
              "in-use tab spells its state on the rendered status line");

        // Line two is the state, not the name again: the name is line one, and
        // repeating it pushed the state off the end of every narrow tab.
        check(!connectedTab->statusLine().contains(connected.name),
              "rendered status line does not repeat the tab's name");
        check(connectedTab->statusLine() == QLatin1String("connected"),
              "status line with no model or detail is just the state");
        check(connectedTab->accessibleDescription().contains(connected.name),
              "accessible description still carries the name");

        // Keep tabs 8 px clear of the bar's edges: Qt's Windows frame keeps a
        // ~8 px top resize border, and a tab there would start a resize.
        app.processEvents();
        const int tabTop = connectedTab->mapTo(bar, QPoint()).y();
        check(tabTop >= 8, "radio tab starts below the top resize border");
        check(tabTop + connectedTab->height() <= TitleBar::kUnifiedBarHeight - 8,
              "radio tab ends above the bar's bottom 8 px");

        // U+00B7, one code unit — not the two that raw UTF-8 bytes would give.
        const QString line = connectedTab->accessibleDescription();
        check(line.contains(QChar(0x00B7)),
              "status line joins with a real MIDDLE DOT");
        check(!line.contains(QChar(0x00C2)),
              "status line is not mojibake (Â from byte-escaped UTF-8)");
    }

    // A nickname hides the hardware, so the model rides on line two.
    {
        RadioTabEntry nick = inUse;
        nick.id = QStringLiteral("SERIAL-NICK");
        nick.name = QStringLiteral("Shack Rig");
        nick.model = QStringLiteral("FLEX-6600");
        RadioTab probe(nick);
        check(probe.statusLine().startsWith(nick.model),
              "nicknamed radio shows its model on the status line");
        check(!probe.statusLine().contains(nick.name),
              "nicknamed radio does not repeat the nickname");
        RadioTabEntry plain = nick;
        plain.name = plain.model;
        RadioTab plainProbe(plain);
        check(!plainProbe.statusLine().contains(plain.model),
              "model is not shown twice when it is the name");
    }

    // The "+" rings for keyboard focus only — it is the window's first
    // focusable widget, so a plain :focus rule lit it on every launch.
    if (QAbstractButton* add = bar->findChild<QAbstractButton*>(QStringLiteral("radioTabAddButton"))) {
        // Delivered directly: an offscreen window is never active, so
        // setFocus() alone would never send these.
        auto focus = [add](QEvent::Type type, Qt::FocusReason reason) {
            QFocusEvent ev(type, reason);
            QApplication::sendEvent(add, &ev);
        };
        focus(QEvent::FocusIn, Qt::ActiveWindowFocusReason);
        check(!add->property("focusVisible").toBool(), "+ shows no ring for initial window focus");
        focus(QEvent::FocusOut, Qt::OtherFocusReason);
        focus(QEvent::FocusIn, Qt::TabFocusReason);
        check(add->property("focusVisible").toBool(), "+ shows its ring for keyboard focus");
        focus(QEvent::FocusOut, Qt::TabFocusReason);
        check(!add->property("focusVisible").toBool(), "+ drops its ring on focus out");
    } else {
        check(false, "+ button exists");
    }

    // Re-pushing an identical list must not rebuild the widgets: discovery
    // re-announces every radio every 5 s, and a rebuild would drop keyboard
    // focus and restart the connected dot's pulse forty times a minute.
    bar->setRadioTabs({connected, inUse});
    check(tabWithId(*bar, connected.id) == connectedTab,
          "an unchanged radio list reuses the existing tab widgets");

    // A status change reuses the widget too, and updates what it announces.
    RadioTabEntry nowAvailable = connected;
    nowAvailable.status = RadioTabStatus::Available;
    bar->setRadioTabs({nowAvailable, inUse});
    check(tabWithId(*bar, connected.id) == connectedTab,
          "a status-only change reuses the tab widget");
    if (connectedTab) {
        check(connectedTab->accessibleName().contains(QLatin1String("available")),
              "the tab re-announces its new state");
    }

    // ── Tab activation is a request, not a switch (PR #4906 review) ─────────
    // Three defects that a scratch harness caught and nothing in CI did.
    {
        RadioTabBar* strip = bar->radioTabBar();
        bar->setRadioTabs({connected, inUse});
        bar->setActiveRadio(connected.id);
        RadioTab* activeTab = tabWithId(*bar, connected.id);
        RadioTab* otherTab = tabWithId(*bar, inUse.id);

        if (activeTab && otherTab && strip) {
            // Clicking the ALREADY-active tab must not leave it unchecked.
            // RadioTab is checkable and in no exclusive group, so QAbstractButton
            // toggles it off on press; setActiveRadio() then early-returns on an
            // unchanged id, so without an explicit re-assert nothing ever
            // re-checks it and the strip stops showing which radio you are on.
            activeTab->click();
            check(activeTab->isChecked(),
                  "re-clicking the active tab leaves it checked");

            // Clicking an INACTIVE tab must not claim it as active: MainWindow
            // opens the picker rather than switching, so an optimistic claim
            // would have the strip (and the bridge's activeId) assert a radio
            // the client never connected to.
            otherTab->click();
            check(strip->activeRadioId() == connected.id,
                  "clicking an inactive tab does not move the active radio");
            check(!otherTab->isChecked(),
                  "clicking an inactive tab does not check it");

            // The link indicator needs a carrier even with nothing connected —
            // "searching" is reported precisely when no radio is active, and
            // that is the state the indicator exists for.
            bar->setActiveRadio(QString());
            strip->setLinkIndicator(QColor("#e0a020"), /*alarm=*/false);
            int carriers = 0;
            for (RadioTab* t : bar->findChildren<RadioTab*>()) {
                if (t->isLinkCarrier()) ++carriers;
            }
            checkEqual(carriers, 1,
                       "exactly one tab carries the link state with no active radio");
        }
        // Put the fixture back for whatever runs after this block.
        bar->setActiveRadio(connected.id);
    }

    // ── Discovered-radios popover ───────────────────────────────────────────
    RadioTabBar* tabs = bar->radioTabBar();
    check(tabs != nullptr, "the bar owns a radio tab strip");
    if (tabs) {
        check(!tabs->isDiscoveryPopoverVisible(), "popover starts closed");
        tabs->setDiscoveredRadios({connected, inUse});
        tabs->showDiscoveryPopover();
        check(tabs->isDiscoveryPopoverVisible(), "the + popover opens");
        const QVariantMap radios = tabs->state();
        checkEqual(radios.value(QStringLiteral("discovered")).toList().size(), 2,
                   "the popover lists every discovered radio");
        if (QWidget* popover = tabs->findChild<QWidget*>(
                QStringLiteral("discoveredRadiosPopover"))) {
            QPushButton* manual = popover->findChild<QPushButton*>(
                QStringLiteral("connectManuallyRow"));
            QLabel* heading = popover->findChild<QLabel*>(
                QStringLiteral("discoveredRadiosHeading"));
            check(manual != nullptr, "the popover exposes its manual-connect row");
            check(heading != nullptr, "the popover exposes its heading");
            if (manual) {
                check(manual->text() == QStringLiteral("Connect manually\u2026"),
                      "the manual-connect ellipsis is valid Unicode");
                check(!manual->styleSheet().contains(QStringLiteral("{{")),
                      "the popover row resolves every theme token");
            }
            const QString panelColor = ThemeManager::instance()
                .color(popover, QStringLiteral("color.background.1"))
                .name(QColor::HexRgb);
            if (manual) {
                check(manual->styleSheet().contains(panelColor, Qt::CaseInsensitive),
                      "the manual row explicitly paints the panel background");
            }
            if (heading) {
                check(heading->styleSheet().contains(panelColor, Qt::CaseInsensitive),
                      "the heading explicitly paints the panel background");
            }
            QLineEdit* search = popover->findChild<QLineEdit*>(QStringLiteral("radioSwitcherSearch"));
            QPushButton* connectedRow = popover->findChild<QPushButton*>(QStringLiteral("radioSwitcherRow_SERIAL-1"));
            QPushButton* otherRow = popover->findChild<QPushButton*>(QStringLiteral("radioSwitcherRow_SERIAL-2"));
            QLabel* empty = popover->findChild<QLabel*>(QStringLiteral("radioSwitcherEmpty"));
            check(search && connectedRow && otherRow && empty, "search and radio rows have stable automation targets");
            if (search && connectedRow && otherRow && empty) {
                search->setText(QStringLiteral("smartlink"));
                check(!connectedRow->isVisible() && otherRow->isVisible(), "search filters transport case-insensitively");
                search->setText(QStringLiteral("unmatched-radio"));
                check(empty->isVisible(), "no matches is explicit");
                search->clear();
                check(connectedRow->isVisible() && otherRow->isVisible() && !empty->isVisible(), "clearing search restores all radios");
            }
            QMenu* connectedMenu = popover->findChild<QMenu*>(QStringLiteral("radioSwitcherMenu_SERIAL-1"));
            QMenu* otherMenu = popover->findChild<QMenu*>(QStringLiteral("radioSwitcherMenu_SERIAL-2"));
            check(connectedMenu && otherMenu, "each radio owns an action menu");
            if (connectedMenu && otherMenu) {
                auto actionFor = [](QMenu* menu, const QString& action, const QString& id) {
                    return menu->findChild<QAction*>(QStringLiteral("radioSwitcher_") + action + '_' + id);
                };
                QAction* disconnect = actionFor(connectedMenu, QStringLiteral("disconnect"), connected.id);
                QAction* remove = actionFor(connectedMenu, QStringLiteral("remove"), connected.id);
                QAction* rename = actionFor(connectedMenu, QStringLiteral("rename"), connected.id);
                QAction* otherDisconnect = actionFor(otherMenu, QStringLiteral("disconnect"), inUse.id);
                QAction* otherRemove = actionFor(otherMenu, QStringLiteral("remove"), inUse.id);
                QAction* otherRename = actionFor(otherMenu, QStringLiteral("rename"), inUse.id);
                check(disconnect && disconnect->isEnabled(), "connected radio can disconnect");
                check(remove && !remove->isEnabled(), "connected radio cannot be removed");
                check(rename && rename->isEnabled(), "rename follows the supplied capability");
                check(otherDisconnect && !otherDisconnect->isEnabled(), "another station's radio cannot be disconnected");
                check(otherRemove && otherRemove->isEnabled(), "inactive radio can be hidden");
                check(otherRename && !otherRename->isEnabled(), "unsupported nickname mutation is disabled");
                QString requestedId;
                QString requestedAction;
                const QMetaObject::Connection request = QObject::connect(tabs, &RadioTabBar::radioActionRequested,
                    [&requestedId, &requestedAction](const QString& id, const QString& action) {
                        requestedId = id;
                        requestedAction = action;
                    });
                if (otherRemove) {
                    otherRemove->trigger();
                    check(requestedId == inUse.id && requestedAction == QStringLiteral("remove"),
                          "remove requests the correct radio without changing its connection state");
                    check(!popover->isVisible(), "selecting an action dismisses the switcher");
                }
                QObject::disconnect(request);
            }
            QImage rendered(popover->size(), QImage::Format_ARGB32_Premultiplied);
            rendered.fill(Qt::transparent);
            popover->render(&rendered);
            check(paintedPixelCount(rendered) > 0,
                  "the discovered-radios popover paints an opaque themed panel");
            popover->close();
        }

        // Discovery changes while a row's Actions menu is open must not delete
        // that menu under the cursor; the popover rebuilds once it closes.
        tabs->showDiscoveryPopover();
        if (QWidget* reopened = QApplication::activePopupWidget()) {
            QPointer<QMenu> rowMenu = reopened->findChild<QMenu*>(QStringLiteral("radioSwitcherMenu_SERIAL-2"));
            check(!rowMenu.isNull(), "the reopened popover has the in-use radio's menu");
            if (rowMenu) {
                rowMenu->popup(reopened->mapToGlobal(QPoint(0, 0)));
                app.processEvents();
                tabs->setDiscoveredRadios({connected});
                app.processEvents();
                // No event loop runs here, so flush deleteLater() by hand: a
                // rebuild would delete the menu exactly as the live app does.
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                check(!rowMenu.isNull() && rowMenu->isVisible(),
                      "an open row menu survives a discovery change");
                if (rowMenu) rowMenu->close();
                app.processEvents();
                app.processEvents();
                // The old popover is still pending deleteLater(); the rebuilt
                // one is the open popup.
                QWidget* rebuilt = QApplication::activePopupWidget();
                check(rebuilt && !rebuilt->findChild<QPushButton*>(QStringLiteral("radioSwitcherRow_SERIAL-2")),
                      "the popover rebuilds from the new list once the menu closes");
                if (rebuilt) rebuilt->close();
            }
            tabs->setDiscoveredRadios({connected, inUse});
        }

        // The strip must not make the whole title bar wider for every radio.
        // All configured tabs still exist and remain keyboard-reachable inside
        // a clipped horizontal viewport; selecting an off-screen active radio
        // scrolls it into view while the + button remains outside the viewport.
        const int twoRadioMinimum = tabs->minimumSizeHint().width();

        // While the bar has room, every tab is shown whole: the strip must not
        // scroll the first tab off behind the brand with spare width left over.
        {
            const QStringList names{QStringLiteral("Hermes-Lite 2"), QStringLiteral("70CM-RXA-XVTR"),
                                    QStringLiteral("ANT1-AV640"), QStringLiteral("FLEX-6600")};
            QList<RadioTabEntry> four;
            for (int i = 0; i < names.size(); ++i) {
                RadioTabEntry entry;
                entry.id = QStringLiteral("FOUR-%1").arg(i);
                entry.name = names.at(i);
                entry.model = QStringLiteral("FLEX-6700");
                entry.detail = QStringLiteral("K6OZY");
                entry.transport = QStringLiteral("192.0.2.%1").arg(i + 40);
                entry.status = i == 2 ? RadioTabStatus::Connected : RadioTabStatus::Available;
                four.append(entry);
            }
            const QSize barSize = bar->size();
            bar->resize(1800, barSize.height());
            tabs->setRadios(four);
            tabs->setActiveRadio(four.at(2).id);
            app.processEvents();
            app.processEvents();
            QScrollArea* strip = tabs->findChild<QScrollArea*>(QStringLiteral("radioTabScroller"));
            bool allWhole = strip != nullptr;
            for (const RadioTabEntry& entry : four) {
                RadioTab* t = tabWithId(*bar, entry.id);
                if (!t || !strip) { allWhole = false; continue; }
                const QRect inView(t->mapTo(strip->viewport(), QPoint()), t->size());
                allWhole = allWhole && strip->viewport()->rect().contains(inView);
            }
            check(allWhole && strip && strip->horizontalScrollBar()->maximum() == 0,
                  "four radios fit whole in an 1800 px bar, none scrolled behind the brand");
            bar->resize(barSize);
            app.processEvents();
        }
        QList<RadioTabEntry> manyRadios;
        for (int index = 0; index < 8; ++index) {
            RadioTabEntry entry;
            entry.id = QStringLiteral("RADIO-%1").arg(index);
            entry.name = QStringLiteral("Configured Radio %1").arg(index + 1);
            entry.transport = QStringLiteral("192.0.2.%1").arg(index + 10);
            entry.status = index == 7 ? RadioTabStatus::Connected
                                      : RadioTabStatus::Available;
            manyRadios.append(entry);
        }
        tabs->setRadios(manyRadios);
        tabs->setActiveRadio(manyRadios.last().id);
        app.processEvents();
        app.processEvents();
        checkEqual(tabs->findChildren<RadioTab*>().size(), 8,
                   "overflow keeps one keyboard-reachable tab per configured radio");
        checkEqual(tabs->minimumSizeHint().width(), twoRadioMinimum,
                   "radio-strip minimum width does not grow with radio count");
        QScrollArea* scroller =
            tabs->findChild<QScrollArea*>(QStringLiteral("radioTabScroller"));
        RadioTab* lastTab = tabWithId(*bar, manyRadios.last().id);
        check(scroller != nullptr && lastTab != nullptr,
              "overflow strip exposes its viewport and final tab");
        if (scroller && lastTab) {
            const QRect lastInViewport(lastTab->mapTo(scroller->viewport(), QPoint()),
                                       lastTab->size());
            check(scroller->viewport()->rect().intersects(lastInViewport),
                  "activating an overflow tab scrolls it into view");

            // A vertical-only mouse wheel must reach overflow tabs too: the
            // strip's scroll bars are hidden, so nothing else could.
            QScrollBar* hbar = scroller->horizontalScrollBar();
            check(hbar->maximum() > 0, "eight radios overflow the strip");
            const int before = hbar->value();
            const QPointF at = scroller->viewport()->rect().center();
            QWheelEvent wheel(at, scroller->viewport()->mapToGlobal(at), QPoint(),
                              QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                              Qt::NoScrollPhase, false);
            QApplication::sendEvent(scroller->viewport(), &wheel);
            check(hbar->value() < before,
                  "a vertical wheel notch scrolls the overflowing strip sideways");

            // Dragging a tab sideways pans the overflowing strip and is not a
            // click; a press that does not move still activates the tab.
            hbar->setValue(hbar->maximum() / 2);
            RadioTab* grabbed = nullptr;
            int grabX = 0;   // a point on the tab that is inside the viewport
            for (RadioTab* t : tabs->findChildren<RadioTab*>()) {
                const QRect inView(t->mapTo(scroller->viewport(), QPoint()), t->size());
                const QRect seen = inView & scroller->viewport()->rect();
                if (t->isVisible() && seen.width() > 8) {
                    grabbed = t;
                    grabX = seen.center().x() - inView.x();
                    break;
                }
            }
            check(grabbed != nullptr, "a tab is in view to drag");
            if (grabbed) {
                int activations = 0;
                const QMetaObject::Connection activation = QObject::connect(
                    tabs, &RadioTabBar::radioActivated, [&activations](const QString&) { ++activations; });
                // A real pointer's screen position does not move with the strip.
                const QPointF pressGlobal = grabbed->mapToGlobal(QPointF(grabX, grabbed->height() / 2.0));
                auto send = [grabbed, pressGlobal](QEvent::Type type, int dx, Qt::MouseButtons held) {
                    const QPointF global = pressGlobal + QPointF(dx, 0);
                    QMouseEvent e(type, grabbed->mapFromGlobal(global), global,
                                  type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                                  held, Qt::NoModifier);
                    QApplication::sendEvent(grabbed, &e);
                };
                const int start = hbar->value();
                send(QEvent::MouseButtonPress, 0, Qt::LeftButton);
                send(QEvent::MouseMove, -20, Qt::LeftButton);
                send(QEvent::MouseMove, -60, Qt::LeftButton);
                send(QEvent::MouseButtonRelease, -60, Qt::NoButton);
                checkEqual(hbar->value() - start, 60, "dragging a tab 60 px left pans the strip 60 px");
                checkEqual(activations, 0, "a drag does not activate the tab it started on");
                // Click where that tab is now, after the pan.
                const QPointF here(grabbed->width() / 2.0, grabbed->height() / 2.0);
                QMouseEvent press(QEvent::MouseButtonPress, here, grabbed->mapToGlobal(here),
                                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(grabbed, &press);
                QMouseEvent release(QEvent::MouseButtonRelease, here, grabbed->mapToGlobal(here),
                                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(grabbed, &release);
                checkEqual(activations, 1, "a still click still activates the tab after a drag");
                QObject::disconnect(activation);
            }
        }
        nowAvailable.visibleInTabs = false;
        tabs->setRadios({nowAvailable, inUse});
        tabs->setActiveRadio(QString());
        tabs->setCompactMode(true);
        check(tabWithId(*bar, inUse.id)->isVisible(), "compact mode selects a visible tab, not a removed radio");
        check(tabWithId(*bar, nowAvailable.id)->isHidden(), "removed radio stays hidden in compact mode");
        check(tabWithId(*bar, inUse.id)->isLinkCarrier(), "link status stays on a visible tab after removal");
        tabs->setCompactMode(false);

        // A lost link raises the alarm; an operator disconnect clears it.
        for (int miss = 0; miss < 3; ++miss) {
            bar->onHeartbeatLost();
        }
        check(tabs->state().value(QStringLiteral("linkAlarm")).toBool(),
              "three missed beats raise the link alarm");
        bar->clearLinkAlarm();
        check(!tabs->state().value(QStringLiteral("linkAlarm")).toBool(),
              "an operator disconnect clears the link alarm");

        // After an unexpected drop the alarm stays on the radio that dropped,
        // not on whichever tab comes first (LAN radios are listed first, so on
        // a two-radio network the idle one used to go red).
        {
            RadioTabEntry first;
            first.id = QStringLiteral("LAN-FIRST");
            first.name = QStringLiteral("FLEX-8600");
            first.status = RadioTabStatus::Available;
            RadioTabEntry dropped;
            dropped.id = QStringLiteral("DROPPED");
            dropped.name = QStringLiteral("Simulator");
            dropped.status = RadioTabStatus::Connected;
            bar->setRadioTabs({first, dropped});
            bar->setActiveRadio(dropped.id);
            app.processEvents();
            dropped.status = RadioTabStatus::Available;
            bar->setRadioTabs({first, dropped});
            bar->setActiveRadio(QString());
            for (int miss = 0; miss < 3; ++miss) {
                bar->onHeartbeatLost();
            }
            RadioTab* firstTab = tabWithId(*bar, first.id);
            RadioTab* droppedTab = tabWithId(*bar, dropped.id);
            check(firstTab && droppedTab, "both radios have tabs");
            if (firstTab && droppedTab) {
                check(droppedTab->isLinkCarrier(), "after a drop the link stays on the radio that dropped");
                check(!firstTab->isLinkCarrier(), "the first tab does not inherit another radio's link");
                check(droppedTab->statusLine().contains(QLatin1String("link lost")),
                      "a lost link is spelled out on the dropped radio's tab");
                check(droppedTab->accessibleName().contains(QLatin1String("link lost")),
                      "a lost link is in the dropped radio's accessible name");
                check(!firstTab->statusLine().contains(QLatin1String("link lost")),
                      "an idle radio's tab never claims a lost link");
                bar->clearLinkAlarm();
                check(!droppedTab->statusLine().contains(QLatin1String("link lost")),
                      "clearing the alarm restores the radio's own status word");
            }
        }

        check(ThemeManager::instance().setActiveTheme(QStringLiteral("Default Light")), "light theme loads");
        tabs->showDiscoveryPopover();
        QWidget* lightPopover = QApplication::activePopupWidget();
        check(lightPopover != nullptr, "switcher opens in the light theme");
        if (lightPopover) {
            const QString lightPanel = ThemeManager::instance().color(lightPopover,
                QStringLiteral("color.background.1")).name(QColor::HexRgb);
            QLabel* lightHeading = lightPopover->findChild<QLabel*>(QStringLiteral("discoveredRadiosHeading"));
            check(lightHeading && lightHeading->styleSheet().contains(lightPanel, Qt::CaseInsensitive),
                  "switcher heading follows the light panel token");
            lightPopover->close();
        }
        ThemeManager::instance().setActiveTheme(QStringLiteral("Default Dark"));
    }

    // ── Hamburger application menu ──────────────────────────────────────────
    // A non-native menu bar moves into a hamburger that leads the bar; the
    // bar itself stays hidden, and its menu shortcuts must keep firing.
    {
        QWidget menuHost;
        auto* menuBar = new QMenuBar(&menuHost);
        menuBar->setNativeMenuBar(false);
        QMenu* fileMenu = menuBar->addMenu(QStringLiteral("&File"));
        QAction* shortcutAct = fileMenu->addAction(QStringLiteral("Shortcut probe"));
        shortcutAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F9")));
        int shortcutFired = 0;
        QObject::connect(shortcutAct, &QAction::triggered, [&shortcutFired]() { ++shortcutFired; });

        auto* menuBarHost = new TitleBar(&menuHost);
        menuBarHost->setMenuBar(menuBar);
        QMenu* viewMenu = menuBar->addMenu(QStringLiteral("&View"));
        viewMenu->addAction(QStringLiteral("Plain item"));
        QAction* checkItem = viewMenu->addAction(QStringLiteral("Checked item"));
        checkItem->setCheckable(true);
        checkItem->setChecked(true);
        QMenu* helpMenu = menuBar->addMenu(QStringLiteral("&Help"));   // added after the hand-off
        menuHost.resize(1400, 200);
        menuHost.show();
        QApplication::setActiveWindow(&menuHost);
        app.processEvents();

        auto* menuBtn = menuBarHost->findChild<QPushButton*>(QStringLiteral("titleBarAppMenuButton"));
        auto* appMenu = menuBarHost->findChild<QMenu*>(QStringLiteral("titleBarAppMenu"));
        QWidget* brand = menuBarHost->findChild<QWidget*>(QStringLiteral("brandMark"));
        check(menuBtn && menuBtn->isVisible(), "non-native menu bar becomes a visible hamburger");
        check(!menuBar->isVisible(), "the inline menu bar is hidden");
        if (menuBtn && brand)
            check(menuBtn->geometry().right() < brand->x(), "hamburger leads the brand");
        if (appMenu) {
            checkEqual(appMenu->actions().size(), 3,
                       "hamburger mirrors menus added before and after setMenuBar");
        }
        if (menuBtn) {
            checkEqual(menuBtn->y() + menuBtn->height() / 2, menuBarHost->height() / 2,
                       "hamburger sits on the bar's centre line");
        }

        QTest::keyClick(&menuHost, Qt::Key_F9, Qt::ControlModifier | Qt::ShiftModifier);
        checkEqual(shortcutFired, 1, "menu shortcuts still fire with the menu bar hidden");

        menuBarHost->setMinimalMode(true);
        check(menuBtn && !menuBtn->isVisible() && !menuBar->isVisible(),
              "minimal mode hides the hamburger and does not resurrect the menu bar");
        QTest::keyClick(&menuHost, Qt::Key_F, Qt::AltModifier);
        check(QApplication::activePopupWidget() == nullptr,
              "minimal mode leaves the menu mnemonics inert, as the hidden bar did");
        menuBarHost->setMinimalMode(false);
        check(menuBtn && menuBtn->isVisible() && !menuBar->isVisible(),
              "leaving minimal mode restores only the hamburger");

        // Rounded corners need a see-through, shadowless window under the
        // stylesheet radius, on every menu the hamburger reaches.
        auto rounded = [](const QMenu* m) {
            return m && m->testAttribute(Qt::WA_TranslucentBackground)
                && m->windowFlags().testFlag(Qt::NoDropShadowWindowHint);
        };
        check(rounded(appMenu) && rounded(fileMenu) && rounded(helpMenu),
              "the hamburger and its menus, including one added later, are rounded");
        check(appMenu && appMenu->styleSheet().contains(QStringLiteral("border-radius: 8px")),
              "the hamburger menu panel carries the 8 px radius");
        // Text starts at one x in every menu, with or without a check column.
        auto textLeft = [](QMenu* m, QAction* row) {
            QMetaObject::invokeMethod(m, "aboutToShow");   // as popup() would
            m->adjustSize();
            QImage img(m->size(), QImage::Format_ARGB32);
            img.fill(Qt::black);
            m->render(&img);
            const QRect r = m->actionGeometry(row);
            for (int x = 0; x < img.width(); ++x)
                for (int y = r.top(); y <= r.bottom(); ++y)
                    if (img.pixelColor(x, y).lightness() > 150)
                        return x;
            return -1;
        };
        const int plainLeft = textLeft(fileMenu, shortcutAct);
        const int checkMenuLeft = textLeft(viewMenu, viewMenu->actions().first());
        check(plainLeft > 0 && plainLeft == checkMenuLeft,
              "menu text starts at the same x with or without a check column");
        std::fprintf(stderr, "  menu text x: plain=%d with-checks=%d\n", plainLeft, checkMenuLeft);
        const QColor menuBg = ThemeManager::instance().color(appMenu, QStringLiteral("color.titlebar.menu.background"));
        const QString menuBgCss = QStringLiteral("background: rgba(%1, %2, %3, ")
            .arg(menuBg.red()).arg(menuBg.green()).arg(menuBg.blue());
        check(appMenu && menuBg.alpha() < 255 && menuBg.alpha() > 200
                  && appMenu->styleSheet().contains(menuBgCss),
              "menu panels use the slightly see-through menu background");

        QMenu* lateSubmenu = fileMenu->addMenu(QStringLiteral("Recent"));
        check(!rounded(lateSubmenu), "a submenu added later is not rounded until its parent opens");

        QTest::keyClick(&menuHost, Qt::Key_F, Qt::AltModifier);
        check(QApplication::activePopupWidget() == fileMenu,
              "Alt+F still opens the File menu with the menu bar hidden");
        check(rounded(lateSubmenu), "opening a menu rounds the submenus added to it since");
        fileMenu->close();
        app.processEvents();

        // Last: offscreen does not hand activation back to this window after a
        // popup closes, so nothing keyboard-driven may follow one.
        // popup(), not exec(): an exec()'d menu would block this click until
        // something closed it, so returning here at all is half the check.
        if (menuBtn && appMenu) {
            QTest::mouseClick(menuBtn, Qt::LeftButton);
            check(QApplication::activePopupWidget() == appMenu,
                  "clicking the hamburger pops its menu without a nested loop");
            appMenu->close();
            app.processEvents();
        }
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "unified_title_bar_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
