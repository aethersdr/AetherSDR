// WHAT A REFUSED "Auto" TICK LEAVES BEHIND ON THE CHECKBOX, and what a later
// successful arm has to take away again.
//
// The GUI half of #5817 puts the backend's refusal sentence on the Auto
// checkbox's ACCESSIBLE DESCRIPTION, because the operator who cannot see the
// panadapter is the one least able to guess why a box sprang back -- a
// transient card and a status-bar message are both effectively invisible to AT
// clients. Review found that half regressing on the very path it fixes: the
// description was set and never cleared, so after the operator followed the
// remedy and armed for real, a running control went on announcing "declined --
// your setting has not been changed", and its standing help tooltip was gone
// for the session.
//
// So the assertions here are about the CLEAR, not the set. The ordering one is
// load-bearing and easy to get wrong: on the operator's retry the box is
// ALREADY checked (they ticked it, the toggle fired), so setAutoRfGainEnabled
// takes its "already there" early return -- a clear written after that return
// never runs on the one path it exists for.
//
// Offscreen and widget-only: a plain QWidget parent, no MainWindow, no
// SpectrumWidget, no backend, no sockets.

#include "gui/SpectrumOverlayMenu.h"

#include <QApplication>
#include <QCheckBox>
#include <QWidget>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

// The sentence Hl2Backend composes, shortened. Its exact text is the backend's
// business and is asserted in hl2_gain_split_test; all this file needs is that
// it is non-empty and distinguishable from the help text.
const QString kWhy = QStringLiteral(
    "Auto RF gain declined — the RF Gain baseline is 20 dB and this radio's "
    "gain axis is not trusted above 19 dB. Lower RF Gain to 19 dB or below and "
    "try again. Your setting has not been changed.");

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    QWidget parent;
    SpectrumOverlayMenu menu(&parent);

    auto* box = parent.findChild<QCheckBox*>(QStringLiteral("antennaAutoRfGainCheck"));
    report("the Auto checkbox is reachable by object name", box != nullptr);
    if (!box) {
        return 1;
    }

    // CAPTURED, NOT RETYPED. A test that retypes the help literal agrees with
    // itself while the code hands back something else entirely; this is the
    // widget's own standing tooltip, read before anything has disturbed it.
    const QString help = box->toolTip();
    const QString name = box->accessibleName();
    report("the checkbox ships with standing help text on its tooltip",
           !help.isEmpty() && !help.contains(QStringLiteral("declined")));

    // ---- A REFUSAL IS WRITTEN WHERE A SCREEN READER WILL FIND IT ----
    menu.setAutoRfGainEnabled(false);   // the springback the wiring does first
    menu.setAutoRfGainRefusalDescription(kWhy);
    report("a refusal lands on the accessible description",
           box->accessibleDescription() == kWhy);
    report("and on the tooltip, so a sighted operator gets it too",
           box->toolTip() == kWhy);
    report("and the accessible NAME still says what the control is",
           box->accessibleName() == name);

    // ---- THE OPERATOR'S RETRY, which is the path the review found broken ----
    //
    // They lower RF Gain and tick Auto again. The tick leaves the box CHECKED
    // before the backend is asked, so when the arm succeeds and the wiring
    // reflects it, setAutoRfGainEnabled(true) finds isChecked() already true
    // and returns early. The clear has to have happened before that return.
    box->setChecked(true);
    report("precondition: the retry leaves the box already checked",
           box->isChecked());
    menu.setAutoRfGainEnabled(true);
    report("a successful arm clears the refusal from the accessible description",
           box->accessibleDescription().isEmpty());
    report("and the clear runs even though the box was already checked "
           "(the early-return path)",
           box->accessibleDescription().isEmpty() && box->isChecked());

    // ---- AND THE CLEAR IS A RESTORE, NOT A BLANKING ----
    //
    // setToolTip(QString()) would leave the control with no tooltip at all,
    // which is not the state it was in before the refusal either.
    report("the standing help tooltip comes back rather than an empty one",
           box->toolTip() == help);

    // ---- A RADIO SWAP MUST NOT PARK A DEAD REASON ON A HIDDEN BOX ----
    //
    // accessibleDescription and toolTip both survive setVisible(false), so a
    // family with no loop would hide the checkbox with the previous radio's
    // refusal still written on it, ready to be read out when a family that has
    // one brings it back.
    menu.setAutoRfGainRefusalDescription(kWhy);
    report("precondition: a refusal is standing before the swap",
           box->accessibleDescription() == kWhy);
    menu.setAutoRfGainAvailable(false);
    report("hiding the control for a family without the loop clears the reason",
           box->accessibleDescription().isEmpty() && box->toolTip() == help);

    return g_failed == 0 ? 0 : 1;
}
