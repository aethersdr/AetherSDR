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
//   * MIDDLE-DOT ENCODING.  The status line joins its parts with U+00B7.  Built
//     from raw UTF-8 bytes inside QStringLiteral it lands as two UTF-16 code
//     units and renders "Â·" — which shipped once, looked like a font problem,
//     and is only visible if something compares the actual string.
//
// Runs headless (offscreen); asserts on widget state, never on pixels.

#include "gui/RadioTabBar.h"
#include "gui/TitleBar.h"
#include "gui/WindowCaptionButtons.h"

#include <QAbstractButton>
#include <QApplication>
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
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "unified_title_bar_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
