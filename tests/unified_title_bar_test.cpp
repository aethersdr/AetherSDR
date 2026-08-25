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
#include "core/ThemeManager.h"

#include <QAbstractButton>
#include <QApplication>
#include <QImage>
#include <QLabel>
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
    auto* bar = new TitleBar(&host);
    host.resize(1400, 200);
    host.show();

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
    // Present on every platform: the window is frameless everywhere, so there
    // is never a native control to fall back on.
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

    // Every caption language is constructible and paintable in one headless
    // process. This catches the prior macOS-only coverage gap: compile-time
    // platform selection still chooses the production style, but the shared
    // painter contract for Windows, Linux and macOS is exercised here.
    struct CaptionContract {
        CaptionStyle style;
        const char* stateName;
        int buttonWidth;
        int buttonHeight;
    };
    const CaptionContract captionContracts[] = {
        {CaptionStyle::WindowsCaption, "windows", 46, 52},
        {CaptionStyle::LinuxChips, "linuxChips", 23, 23},
        {CaptionStyle::MacTrafficLights, "macTrafficLights", 20, 22},
    };
    for (const CaptionContract& contract : captionContracts) {
        WindowCaptionButtons controls(contract.style);
        controls.adjustSize();
        controls.show();
        app.processEvents();
        const QVariantMap controlState = controls.state();
        check(controlState.value(QStringLiteral("style")).toString()
                  == QLatin1String(contract.stateName),
              "caption cluster reports the requested platform style");
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
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "unified_title_bar_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
