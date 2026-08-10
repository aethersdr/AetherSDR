// PhoneCwApplet — APF row on the CW face (#4879).
//
// Three things are pinned here, and each of them broke (or would have broken)
// something an operator can see:
//
//   1. THE SLICE BINDING DOESN'T STACK. AppletPanel::setSlice() is called on
//      every active-slice change, and the connection it used to make had no
//      matching disconnect — so returning to a slice you had used before added
//      another modeChanged handler each time. The applet now owns both edges of
//      the binding. §3 counts the receivers directly: revisiting a slice must
//      leave exactly one.
//
//   2. THE ROW IS CAPABILITY-GATED. APF's only effect is `slice set <n> apf=`,
//      a verb the radio's firmware executes — the test
//      docs/architecture/radio-capabilities-map.md gives for a control that
//      belongs behind hasRadioSideDsp. Ungated on the always-visible CW face it
//      is the HERMES §17 shape on an Icom or an HL2: the button moves, the
//      state persists, the audio never changes. §4.
//
//   3. THE TWO DIRECTIONS ROUND-TRIP, AND THE LEVEL FOLLOWS ENGAGEMENT. The
//      radio-side echo drives the UI (Principle II) and the slider is inert
//      while the filter is disengaged (#4658) — a slider that talks to a
//      filter that is off reads as "APF is broken". §1, §2.
//
// Deliberately NOT asserted: pixel geometry or styling. Those belong to the
// visual pass; what matters here is that the control is wired to the model and
// gated on the radio.

#include "gui/PhoneCwApplet.h"
#include "models/SliceModel.h"

#include <QApplication>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>

#include <cstdio>

using AetherSDR::PhoneCwApplet;
using AetherSDR::SliceModel;

static int g_failures = 0;

static void check(const char* what, bool ok, const QString& detail = {})
{
    std::printf("%s %s%s\n", ok ? "  ok  " : "  FAIL", what,
                (ok || detail.isEmpty())
                    ? ""
                    : qPrintable(QStringLiteral("  (%1)").arg(detail)));
    if (!ok) {
        ++g_failures;
    }
}

// receivers() is protected on QObject, which is exactly the introspection §3
// needs: the stacking bug is invisible from the outside, because a duplicated
// modeChanged handler just runs an idempotent setMode() twice.
class ProbeSlice : public SliceModel {
public:
    using SliceModel::SliceModel;
    int modeReceivers() const { return receivers(SIGNAL(modeChanged(QString))); }
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    PhoneCwApplet applet;
    ProbeSlice sliceA(0);
    ProbeSlice sliceB(1);

    auto* apfBtn = applet.findChild<QPushButton*>(QStringLiteral("cwApfBtn"));
    auto* apfSlider = applet.findChild<QSlider*>(QStringLiteral("cwApfSlider"));
    auto* apfRow = applet.findChild<QWidget*>(QStringLiteral("cwApfRow"));
    check("APF row, button and slider carry their automation objectNames",
          apfBtn && apfSlider && apfRow);
    if (!apfBtn || !apfSlider || !apfRow) {
        return 1;   // nothing below can run
    }

    // ── §1  Unbound, then bound ─────────────────────────────────────────────
    std::printf("\nSection 1 — binding\n");
    check("1.1  inert before any slice is bound", !apfBtn->isEnabled());

    applet.setSlice(&sliceA);
    check("1.2  enabled once a slice is bound", apfBtn->isEnabled());
    check("1.3  level row still inert while APF is off (#4658)",
          !apfSlider->isEnabled());

    // ── §2  Both directions ─────────────────────────────────────────────────
    std::printf("\nSection 2 — round trip\n");
    QSignalSpy cmds(&sliceA, &SliceModel::commandReady);

    apfBtn->click();
    check("2.1  clicking APF asks the radio to engage it",
          !cmds.isEmpty()
              && cmds.last().at(0).toString() == QStringLiteral("slice set 0 apf=1"),
          cmds.isEmpty() ? QStringLiteral("(no command)")
                         : cmds.last().at(0).toString());
    check("2.2  level row follows engagement", apfSlider->isEnabled());

    // Radio-side echo drives the UI, not the button (Principle II): an APF
    // change made from the DSP tab, another Multi-Flex client or the front
    // panel has to land on this face too.
    sliceA.setApfLevel(72);
    check("2.3  a level echoed by the radio reaches the slider",
          apfSlider->value() == 72, QString::number(apfSlider->value()));

    sliceA.setApf(false);
    check("2.4  disengaging from the model unchecks the button",
          !apfBtn->isChecked());
    check("2.5  ...and re-disables the level row", !apfSlider->isEnabled());

    // ── §3  Re-binding does not stack handlers ──────────────────────────────
    std::printf("\nSection 3 — re-binding (the #4879 adjacent defect)\n");
    check("3.1  one modeChanged receiver after the first bind",
          sliceA.modeReceivers() == 1,
          QString::number(sliceA.modeReceivers()));

    applet.setSlice(&sliceB);
    check("3.2  binding elsewhere drops the old slice's handler",
          sliceA.modeReceivers() == 0,
          QString::number(sliceA.modeReceivers()));

    applet.setSlice(&sliceA);
    check("3.3  RETURNING to a slice still leaves exactly one handler",
          sliceA.modeReceivers() == 1,
          QString::number(sliceA.modeReceivers()));

    // The shape that used to fail: setSlice on every active-slice change, so an
    // operator cycling A -> B -> A -> B -> A stacked a handler per visit.
    for (int i = 0; i < 4; ++i) {
        applet.setSlice(&sliceB);
        applet.setSlice(&sliceA);
    }
    check("3.4  eight more slice changes still leave exactly one",
          sliceA.modeReceivers() == 1,
          QString::number(sliceA.modeReceivers()));
    check("3.5  ...and none stranded on the other slice",
          sliceB.modeReceivers() == 0,
          QString::number(sliceB.modeReceivers()));

    // Unbinding entirely must not leave the row live against a dead slice.
    applet.setSlice(nullptr);
    check("3.6  unbinding disables the row", !apfBtn->isEnabled());
    check("3.7  ...and leaves no handler behind", sliceA.modeReceivers() == 0,
          QString::number(sliceA.modeReceivers()));

    // ── §4  Capability gate ─────────────────────────────────────────────────
    std::printf("\nSection 4 — hasRadioSideDsp gate\n");
    applet.setSlice(&sliceA);
    // Nothing is realised in an offscreen test that never show()s the applet,
    // and the CW face is the non-current page of a QStackedWidget besides — so
    // isVisible() is false for everything here regardless of the gate.
    // isHidden() is the flag the gate actually sets, and the one that decides
    // whether the row appears once the operator switches to CW.
    check("4.1  row shows by default (permissive while disconnected)",
          !apfRow->isHidden());

    applet.setHasRadioSideDsp(false);
    check("4.2  a radio with no radio-side DSP hides the row entirely",
          apfRow->isHidden());
    check("4.3  ...and the hidden control cannot be driven", !apfBtn->isEnabled());

    // A gated row must not keep SENDING, which is the half that makes this a
    // capability gate rather than a cosmetic hide. Hiding and disabling stops a
    // person, but setChecked() still emits toggled — so drive it the way the
    // automation bridge would and assert nothing reaches the radio.
    const int before = cmds.count();
    apfBtn->setChecked(true);
    apfSlider->setValue(33);
    applet.setSlice(&sliceB);
    applet.setSlice(&sliceA);
    check("4.4  a programmatic toggle under the gate issues no APF command",
          cmds.count() == before, QString::number(cmds.count() - before));
    apfBtn->setChecked(false);

    applet.setHasRadioSideDsp(true);
    check("4.5  the row comes back when a capable radio attaches",
          !apfRow->isHidden() && apfBtn->isEnabled());

    std::printf("\n%s — %d failure(s)\n", g_failures ? "FAILED" : "PASSED",
                g_failures);
    return g_failures ? 1 : 0;
}
