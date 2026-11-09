// The overlay menu's DAX button stays hidden on a radio with no DAX plane,
// across expand/collapse; the Scope row appears only for a radio with a scope
// mode declaration, while preserving the Icom unavailable-row exception.
//
// setDaxStreamsAvailable(false) hid the button with a bare setVisible(), and
// updateLayout() -- which runs on every expand/collapse and assigns every menu
// button's visibility outright -- knew only about +TNF. So on a Hermes-Lite 2
// the first collapse-and-reopen brought the DAX button back, and with it the
// WFM toggle on a DAX IQ stream nothing feeds. The capability now lives where
// updateLayout() reads it.
//
// Same shape as spectrum_overlay_band_highlight_test: widget only, a plain
// QWidget parent, offscreen, no MainWindow and no backend. Visibility is read
// with isHidden() (the explicit flag), since the parent is never shown.
//
// Mutation-checked: with updateLayout()'s kBtnDax arm replaced by `true`,
// "no DAX plane -> DAX button hidden", "DAX stays hidden after collapse and
// re-expand" and "and after a second round trip" fail, and nothing else in the
// suite does. The "precondition" check pins the default (available until told
// otherwise), so the hide is asserted as a change of state, not assumed.

#include "gui/SpectrumOverlayMenu.h"

#include <QApplication>
#include <QPushButton>
#include <QWidget>

#include <cstdio>
#include <vector>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

static QPushButton* g_toggle = nullptr;   // the ←/→ collapse button

static void clickToggle()
{
    g_toggle->click();
}

static void setExpanded(QPushButton* probe, bool expanded)
{
    // The +RX button is never capability-hidden, so it says which state the
    // menu is in; the ←/→ button is how the operator flips it.
    if (probe->isHidden() == expanded) {
        clickToggle();
    }
}

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QWidget parent;
    SpectrumOverlayMenu menu(&parent);

    auto* dax = parent.findChild<QPushButton*>(QStringLiteral("panMenuDaxBtn"));
    QPushButton* addRx = nullptr;
    for (auto* b : parent.findChildren<QPushButton*>()) {
        if (b->text() == QLatin1String("+RX")) addRx = b;
        if (b->text() == QStringLiteral("\u2190") || b->text() == QStringLiteral("\u2192"))
            g_toggle = b;
    }
    check(dax && addRx && g_toggle, "DAX, +RX and collapse buttons are discoverable");
    if (!dax || !addRx || !g_toggle) return 1;

    setExpanded(addRx, true);
    check(!dax->isHidden(), "precondition: DAX shows on an expanded menu by default");

    menu.setDaxStreamsAvailable(false);
    check(dax->isHidden(), "no DAX plane -> DAX button hidden");

    setExpanded(addRx, false);
    setExpanded(addRx, true);
    check(!addRx->isHidden(), "the menu really did re-expand");
    check(dax->isHidden(), "DAX stays hidden after collapse and re-expand");

    clickToggle();
    clickToggle();
    check(dax->isHidden(), "and after a second round trip");

    setExpanded(addRx, false);
    menu.setDaxStreamsAvailable(true);
    check(dax->isHidden(),
          "DAX becoming available on a COLLAPSED menu does not show it alone");
    setExpanded(addRx, true);
    check(!dax->isHidden(), "DAX available -> shown again on expand");

    // Explicit backend mode labels admit the scope row; the Icom exception
    // keeps its row discoverable even when the model offers no mode choice.
    auto* scopeRow = parent.findChild<QWidget*>(QStringLiteral("displayScopeModeRow"));
    check(scopeRow != nullptr, "the Scope row is discoverable");
    if (scopeRow) {
        menu.setRadioFamily(QStringLiteral("flex"));
        menu.setScopeModeLabels({});
        check(scopeRow->isHidden(), "default Flex without native mode labels keeps the Scope row hidden");
        menu.setRadioFamily(QStringLiteral("kenwood"));
        menu.setScopeModeHelp(QStringLiteral("Backend-declared native scope help."));
        menu.setScopeModeLabels({QStringLiteral("Center"), QStringLiteral("Fixed"),
                                 QStringLiteral("Auto-scroll")});
        check(!scopeRow->isHidden() && scopeRow->isEnabled(),
              "a backend declaring native mode labels exposes the Scope row for Kenwood");
        check(scopeRow->toolTip().contains(QStringLiteral("Backend-declared native scope help."))
                  && !scopeRow->toolTip().contains(QStringLiteral("preset 4")),
              "native help comes from an explicit descriptor rather than Icom instructions");
        auto* nativeFixed = parent.findChild<QPushButton*>(QStringLiteral("displayScopeMode_fixed"));
        auto* nativeCenter = parent.findChild<QPushButton*>(QStringLiteral("displayScopeMode_center"));
        auto* nativeScroll = parent.findChild<QPushButton*>(QStringLiteral("displayScopeMode_autoscroll"));
        check(nativeFixed && nativeCenter && nativeScroll, "all three declared native mode buttons exist");
        if (nativeFixed && nativeCenter && nativeScroll) {
            menu.setScopeModeIndex(1);
            check(nativeFixed->isChecked() && !nativeCenter->isChecked() && !nativeScroll->isChecked(),
                  "native Fixed readback selects its declared index");
            std::vector<int> nativePicks;
            const auto nativeConnection = QObject::connect(&menu, &SpectrumOverlayMenu::scopeModeChanged,
                [&nativePicks](int index) { nativePicks.push_back(index); });
            nativeCenter->click();
            check(nativePicks.size() == 1 && nativePicks.back() == 0 && nativeFixed->isChecked()
                      && !nativeCenter->isChecked(),
                  "clicking native Center requests its index while keeping confirmed Fixed lit");
            menu.setScopeModeIndex(0);
            check(nativeCenter->isChecked() && !nativeFixed->isChecked(),
                  "native Center readback, rather than the click, updates the checked mode");
            QObject::disconnect(nativeConnection);
        }
        menu.setScopeModeHelp({});
        menu.setRadioFamily(QStringLiteral("icom"));
        check(scopeRow->toolTip().contains(QStringLiteral("preset 4; presets 1-3 are left as you set them.")),
              "undeclared scope help restores the existing Icom preset instructions");
        menu.setScopeModeLabels({QStringLiteral("Center")});
        check(!scopeRow->isHidden() && !scopeRow->isEnabled()
                  && !scopeRow->accessibleDescription().isEmpty(),
              "an Icom without a choice -> Scope row shown, dimmed, with a reason");
        menu.setScopeModeLabels({QStringLiteral("Center"), QStringLiteral("Scroll-F"),
                                 QStringLiteral("Fixed")});
        check(!scopeRow->isHidden() && scopeRow->isEnabled(),
              "an Icom offering modes -> Scope row shown and enabled");
        // A Fixed on one of the radio's own presets is published as "nothing
        // lit" (index -1): a click on Fixed must then ask for Fixed, so the
        // backend can reclaim its slot; with Fixed already lit it asks nothing.
        std::vector<int> picks;
        QObject::connect(&menu, &SpectrumOverlayMenu::scopeModeChanged,
                         [&picks](int i) { picks.push_back(i); });
        auto* fixedBtn = parent.findChild<QPushButton*>(QStringLiteral("displayScopeMode_fixed"));
        check(fixedBtn != nullptr, "the Fixed button is discoverable");
        if (fixedBtn) {
            menu.setScopeModeIndex(-1);
            fixedBtn->click();
            check(picks.size() == 1 && picks.back() == 2,
                  "with nothing lit, clicking Fixed asks for Fixed");
            menu.setScopeModeIndex(2);
            fixedBtn->click();
            check(picks.size() == 1, "with Fixed already lit, clicking it asks for nothing");
        }
        menu.setScopeModeLabels({});
        menu.setRadioFamily(QStringLiteral("hl2"));
        check(scopeRow->isHidden(), "the next radio without a native mode declaration has no Scope row");
    }

    std::printf("spectrum_overlay_dax_availability_test: %s\n",
                g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
