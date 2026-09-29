// +ACC is Unavailable -- dimmed, with its reason announced -- on a radio whose
// transmit inputs this client cannot select.
//
// +ACC mixes a Flex rear ACCESSORY connector in with the selected input, and
// sends `mic acc`, Flex wire text a radio without a command plane drops. On a
// Hermes-Lite 2 the source combo beside it already collapses to PC
// (hasSelectableMicInputs=false), but +ACC stayed live: it toggled, lit green
// and stayed lit while nothing reached the radio.
//
// The three-state doctrine (theme-style-guide.md "Three-state controls"): an
// unavailable control is dimmed and its reason reaches a screen reader through
// accessibleDescription, not a tooltip alone. Pinned here:
//   * selectable=false: disabled, unchecked, reason in accessibleDescription
//     and tooltip, and no `mic acc` command on the way;
//   * selectable=true: live again with its original description.
//
// Not yet mutation-checked. The mutation to run before a PR: removing
// setEnabled(selectable) fails "dimmed"; removing the accessibleDescription
// write fails "reason is announced".

#include "TestSettingsProfile.h"
#include "gui/PhoneCwApplet.h"
#include "models/TransmitModel.h"

#include <QApplication>
#include <QPushButton>
#include <QSignalSpy>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

static QPushButton* accButton(PhoneCwApplet& applet)
{
    for (QPushButton* b : applet.findChildren<QPushButton*>()) {
        if (b->accessibleName() == QLatin1String("Accessory mic input"))
            return b;
    }
    return nullptr;
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("phone-cw-acc-availability-test"));
    QApplication app(argc, argv);

    TransmitModel model;
    PhoneCwApplet applet;
    applet.setTransmitModel(&model);
    QPushButton* acc = accButton(applet);
    check(acc != nullptr, "+ACC is discoverable by accessibleName");
    if (!acc) return 1;

    const QString liveDescription = acc->accessibleDescription();
    check(acc->isEnabled(), "precondition: +ACC is live before any radio says otherwise");

    // Light it first, as an operator on a Flex would have, then connect a
    // radio that cannot select inputs.
    acc->setChecked(true);
    QSignalSpy commands(&model, &TransmitModel::commandReady);
    applet.setSelectableMicInputs(false);

    check(!acc->isEnabled(), "no selectable inputs -> +ACC is dimmed");
    check(!acc->isChecked(), "and not left lit");
    check(acc->accessibleDescription().startsWith(QLatin1String("Unavailable:")),
          "the reason is announced through accessibleDescription");
    check(acc->toolTip() == acc->accessibleDescription(),
          "tooltip and accessibleDescription carry the same reason");
    check(commands.count() == 0,
          "dimming sends nothing -- no `mic acc 0` on a radio that drops it");

    applet.setSelectableMicInputs(true);
    check(acc->isEnabled(), "selectable inputs -> +ACC is live again");
    check(acc->accessibleDescription() == liveDescription,
          "and its own description is restored");

    std::printf("phone_cw_acc_availability_test: %s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
