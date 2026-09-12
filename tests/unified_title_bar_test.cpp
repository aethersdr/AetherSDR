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
// Runs headless (offscreen); asserts on widget state, never on pixels.

#include "gui/RadioTabBar.h"
#include "gui/TitleBar.h"
#include "gui/WindowCaptionButtons.h"
#include "gui/WindowChrome.h"
#include "core/ThemeManager.h"

#include <QAbstractButton>
#include <QApplication>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>

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

        // U+00B7, one code unit — not the two that raw UTF-8 bytes would give.
        const QString line = connectedTab->accessibleDescription();
        check(line.contains(QChar(0x00B7)),
              "status line joins with a real MIDDLE DOT");
        check(!line.contains(QChar(0x00C2)),
              "status line is not mojibake (Â from byte-escaped UTF-8)");
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

        // The strip must not make the whole title bar wider for every radio.
        // All configured tabs still exist and remain keyboard-reachable inside
        // a clipped horizontal viewport; selecting an off-screen active radio
        // scrolls it into view while the + button remains outside the viewport.
        const int twoRadioMinimum = tabs->minimumSizeHint().width();
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
        check(tabs->maximumWidth() <= 560,
              "radio strip has a finite title-bar width ceiling");
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
        }
        nowAvailable.visibleInTabs = false;
        tabs->setRadios({nowAvailable, inUse});
        tabs->setActiveRadio(QString());
        tabs->setCompactMode(true);
        check(tabWithId(*bar, inUse.id)->isVisible(), "compact mode selects a visible tab, not a removed radio");
        check(tabWithId(*bar, nowAvailable.id)->isHidden(), "removed radio stays hidden in compact mode");
        check(tabWithId(*bar, inUse.id)->isLinkCarrier(), "link status stays on a visible tab after removal");
        tabs->setCompactMode(false);
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

    if (g_failures == 0) {
        std::fprintf(stderr, "unified_title_bar_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
