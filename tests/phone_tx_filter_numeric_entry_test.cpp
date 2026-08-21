// TX filter direct numeric entry (#3627).
//
// The PHONE applet's low/high cut readouts are ScrollableLabels. Before this
// they could only be stepped in 50 Hz snaps; now they can be typed into.
// These rows pin the things that are easy to get wrong: the cross-bound a
// typed value must obey, the fact that an out-of-range entry is REJECTED with
// the previous value restored (issue #3627's words) rather than clamped, that
// Enter is terminal for every input including the empty field, that the
// feature is reachable from the keyboard, and that editing is OFF for every
// OTHER ScrollableLabel in the app — RIT/XIT, step size, RTTY mark/shift all
// share this class.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/GuardedSlider.h"
#include "gui/PhoneApplet.h"
#include "models/TransmitModel.h"

#include <QApplication>
#include <QLineEdit>
#include <QAccessible>
#include <QFocusEvent>
#include <QLocale>
#include <QSignalSpy>
#include <QCoreApplication>
#include <QTest>

#include <cstdio>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* label)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", label);
    if (!condition)
        ++failures;
}

ScrollableLabel* cutLabel(PhoneApplet& applet, const char* accessibleName)
{
    for (ScrollableLabel* l : applet.findChildren<ScrollableLabel*>()) {
        if (l->accessibleName() == QLatin1String(accessibleName))
            return l;
    }
    return nullptr;
}

// Open the editor and hand back the CURRENT one.
//
// Always drain deferred deletes first: findChild<QLineEdit*>() will otherwise
// return an editor a previous edit already closed but that has not been
// deleted yet, and every assertion then runs against a widget the label no
// longer owns. A running app gets this for free from the event loop between
// two operator actions.
QLineEdit* openEditor(ScrollableLabel* label)
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    label->beginEdit();
    return label->findChild<QLineEdit*>();
}

// Open the editor the way an operator on a keyboard does, and hand back the
// CURRENT one. Same deferred-delete drain as openEditor() and for the same
// reason: without it findChild() returns an editor a previous row already
// closed but that has not been deleted yet.
QLineEdit* openEditorByKey(ScrollableLabel* label, Qt::Key key)
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    label->setFocus(Qt::TabFocusReason);
    QTest::keyClick(label, key);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return label->findChild<QLineEdit*>();
}

// Type into an open editor and commit with Return.
void typeAndCommit(ScrollableLabel* label, const QString& text)
{
    QLineEdit* ed = openEditor(label);
    if (!ed)
        return;
    ed->setText(text);
    QTest::keyClick(ed, Qt::Key_Return);
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("phone-tx-filter-numeric-entry-test"));
    QApplication app(argc, argv);
    AppSettings::instance().load();

    TransmitModel model;
    PhoneApplet applet;
    applet.setTransmitModel(&model);

    ScrollableLabel* low  = cutLabel(applet, "TX low cut frequency");
    ScrollableLabel* high = cutLabel(applet, "TX high cut frequency");
    check(low != nullptr,  "low cut readout exists");
    check(high != nullptr, "high cut readout exists");
    if (!low || !high)
        return 1;

    check(low->isEditable(),  "low cut readout is editable (#3627)");
    check(high->isEditable(), "high cut readout is editable (#3627)");

    // The accepted range comes from the MODEL, never a literal in the widget.
    // AetherSDR is growing backends (HL2, Icom, FT-991, ColibriNANO, RTL-SDR);
    // when a radio declares a narrower passband these accessors are the single
    // seam that has to change, and the editor follows without being touched.
    check(low->editMinimum() == model.txFilterMinHz()
          && low->editMaximum() == model.txFilterMaxHz(),
          "the editor takes its range from the model, not a hardcoded 0..10000");
    check(model.txFilterMinWidthHz() > 0,
          "the minimum passband width is model-owned too");

    // Without a styler the editor renders as a bare system line edit in the
    // middle of a dark panel. The rule must select QLineEdit — Qt matches
    // stylesheet selectors on widget class, so a QLabel rule styles nothing.
    check(low->hasEditorStyler() && high->hasEditorStyler(),
          "both readouts style their editor rather than leaving it unthemed");
    if (QLineEdit* ed = openEditor(low)) {
        check(ed->styleSheet().contains(QLatin1String("QLineEdit")),
              "the editor is themed with a QLineEdit rule");
        check(!ed->styleSheet().contains(QLatin1String("{{")),
              "theme tokens are resolved, not passed through raw");
        QTest::keyClick(ed, Qt::Key_Escape);
    }

    // ── The feature: an exact value, in one action ────────────────────────
    model.setTxFilter(50, 3300);
    typeAndCommit(low, QStringLiteral("237"));
    check(model.txFilterLow() == 237,
          "a typed low cut reaches the model exactly (not snapped to 50 Hz)");

    typeAndCommit(high, QStringLiteral("2843"));
    check(model.txFilterHigh() == 2843,
          "a typed high cut reaches the model exactly");

    // The issue's motivating example: 100-2900 in two actions, not dozens.
    model.setTxFilter(50, 3300);
    typeAndCommit(low, QStringLiteral("100"));
    typeAndCommit(high, QStringLiteral("2900"));
    check(model.txFilterLow() == 100 && model.txFilterHigh() == 2900,
          "100-2900 Hz is reachable by typing both bounds");

    // ── Cross-bound: rejected, previous value restored (#3627) ────────────
    //
    // Issue #3627: "Invalid values are rejected with validation and the
    // previous value is restored." This clamped until the #5064 review — a
    // typed 9000 silently became 3250, which reads as the radio ignoring the
    // operator. The step buttons still clamp, deliberately: stopping at the
    // bound is the only sensible answer to "move by one increment".
    model.setTxFilter(50, 3300);
    typeAndCommit(low, QStringLiteral("9000"));
    check(model.txFilterLow() == 50,
          "a low cut above high is REJECTED, and the previous low stands");
    // The reason the cross-bound test lives at the call site at all: the model
    // would resolve the same crossed pair by keeping low and dragging high up
    // to 9050, moving an edge the operator never touched.
    check(model.txFilterHigh() == 3300,
          "rejecting a typed low cut leaves the untouched high cut where it was");
    check(low->text() == QStringLiteral("50"),
          "the label is restored to the model value, not the rejected keystrokes");

    model.setTxFilter(500, 3300);
    typeAndCommit(high, QStringLiteral("200"));
    check(model.txFilterHigh() == 3300,
          "a high cut below low is REJECTED, and the previous high stands");

    // Above the model's own ceiling, not just the cross-bound.
    model.setTxFilter(50, 3300);
    typeAndCommit(high, QStringLiteral("99999"));
    check(model.txFilterHigh() == 3300,
          "a high cut above the model maximum is rejected too");

    // ── The stale-label trap ──────────────────────────────────────────────
    // A rejected entry changes nothing, so no model signal fires and
    // syncFromModel() never runs. The label must already be right — it is,
    // because the editor is a separate widget laid OVER it and the label's own
    // text was never touched. That is what makes "restore" free.
    model.setTxFilter(3250, 3300);
    typeAndCommit(low, QStringLiteral("9999"));
    check(model.txFilterLow() == 3250,
          "a rejected entry leaves the model alone");
    check(low->text() == QStringLiteral("3250"),
          "the label still shows the model value after a rejected entry");

    // Out-of-range input must not deadlock Return. QIntValidator calls 20000
    // Intermediate and QLineEdit will not emit returnPressed/editingFinished
    // on Intermediate input, so with the clamping fixup() gone, Enter is
    // terminal only because eventFilter handles it explicitly.
    model.setTxFilter(50, 3300);
    typeAndCommit(low, QStringLiteral("20000"));
    check(!low->isEditing(), "Enter closes the editor on an out-of-range value");
    check(model.txFilterLow() == 50 && model.txFilterHigh() == 3300,
          "an out-of-range typed value is rejected, and nothing moves");

    // ── Enter is terminal for EVERY input (#5064 review) ──────────────────
    //
    // The stranding bug: commit hung off editingFinished, which QLineEdit
    // emits on Return only once the validator reports Acceptable. An empty
    // field is Intermediate and fixup() could not repair it, so Enter emitted
    // nothing and the editor stayed open — covering the wheel target
    // underneath. Reverting the Return branch in eventFilter fails these.
    model.setTxFilter(200, 3300);
    if (QLineEdit* ed = openEditor(low)) {
        ed->clear();
        check(!ed->hasAcceptableInput(),
              "an empty field is Intermediate, not Acceptable (the precondition)");
        QTest::keyClick(ed, Qt::Key_Return);
    }
    check(!low->isEditing(), "empty + Enter closes the editor instead of stranding it");
    check(model.txFilterLow() == 200, "empty + Enter commits nothing");

    // No "the wheel reaches the label again" row here, deliberately. Written
    // and then deleted: it passed with the fix REVERTED, because a test can
    // only sendEvent() to the label directly, which bypasses the very overlay
    // the row claimed to be about. In the app the editor is a child laid over
    // the label and the window routes the wheel to it; in a harness there is
    // nothing to route. `!low->isEditing()` above is the real assertion — no
    // editor, no lid.

    model.setTxFilter(200, 3300);
    typeAndCommit(low, QStringLiteral("abc"));
    check(!low->isEditing() && model.txFilterLow() == 200,
          "unparseable + Enter closes the editor and commits nothing");

    // ── Keyboard reachability (docs/a11y.md, #5064 review) ────────────────
    //
    // The feature was mouse-double-click-only, which defeats the
    // accessibility motivation in the issue itself.
    check(low->focusPolicy() != Qt::NoFocus && high->focusPolicy() != Qt::NoFocus,
          "an editable readout is Tab-focusable");
    check(!low->accessibleDescription().isEmpty(),
          "the readout describes its editability to an AT");
    check(!low->accessibleDescription().contains(QLatin1String("Space")),
          "the readout does not advertise Space, which the app reserves for PTT");

    // ── The AT contract, not just the names (#5064 re-review) ─────────────
    //
    // The first version of this claimed role EditableText and implemented no
    // text or editable-text interface, leaving SetFocus as the only action —
    // so VoiceOver/NVDA/switch-control could focus the readout and never open
    // the editor, because AT activation invokes an accessibility ACTION rather
    // than synthesizing Return. Asserting the role alone would not have caught
    // that; these rows assert the actionable contract behind it.
    {
        QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(low);
        check(iface != nullptr, "the editable readout has an accessible interface");
        check(iface && iface->role() == QAccessible::Button,
              "the editable readout reports Button, per docs/a11y.md");

        QAccessibleActionInterface* action = iface ? iface->actionInterface() : nullptr;
        check(action != nullptr, "it exposes an action interface");
        const QStringList names = action ? action->actionNames() : QStringList{};
        check(names.contains(QAccessibleActionInterface::pressAction()),
              "press is among its actions, not just SetFocus");
        check(names.contains(QAccessibleActionInterface::setFocusAction()),
              "SetFocus is still offered alongside it");
        const QStringList pressKeys = action
            ? action->keyBindingsForAction(QAccessibleActionInterface::pressAction())
            : QStringList{};
        check(pressKeys.contains(QLatin1String("Return")),
              "the press action announces Return as its activation key");
        check(!pressKeys.contains(QLatin1String("Space")),
              "the press action does not announce Space, which the app reserves for PTT");

        // The row that actually matters: driving the accessibility API the way
        // an AT does must open the real editor.
        model.setTxFilter(200, 3300);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (action)
            action->doAction(QAccessibleActionInterface::pressAction());
        check(low->isEditing(), "an AT press opens the editor");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (QLineEdit* ed = low->findChild<QLineEdit*>()) {
            ed->setText(QStringLiteral("610"));
            QTest::keyClick(ed, Qt::Key_Return);
        }
        check(model.txFilterLow() == 610,
              "an edit opened through the accessibility API commits normally");

        // The lock gate applies to the AT route too — the keyboard and
        // double-click routes both honour it, and a third way in that ignored
        // it would be a hole (#745).
        ControlsLock::setLocked(true);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (action)
            action->doAction(QAccessibleActionInterface::pressAction());
        check(!low->isEditing(), "an AT press on a locked panel is a no-op (#745)");
        ControlsLock::setLocked(false);

        // A display-only ScrollableLabel must NOT become a button.
        ScrollableLabel readOnly(QStringLiteral("+0 Hz"));
        QAccessibleInterface* plainIface =
            QAccessible::queryAccessibleInterface(&readOnly);
        check(plainIface && plainIface->role() != QAccessible::Button,
              "a display-only ScrollableLabel is not announced as a button");
    }

    model.setTxFilter(200, 3300);
    if (QLineEdit* ed = openEditorByKey(low, Qt::Key_Return)) {
        check(low->isEditing(), "Return on the focused readout opens the editor");
        check(!ed->accessibleName().isEmpty(),
              "the editor carries an accessible name of its own");
        ed->setText(QStringLiteral("450"));
        QTest::keyClick(ed, Qt::Key_Return);
    } else {
        check(false, "Return on the focused readout opens the editor");
    }
    check(model.txFilterLow() == 450,
          "a keyboard-only edit reaches the model (open, type, Enter)");
    // Focus is asserted through the label's own window rather than
    // hasFocus(): this applet is never shown, so no window is ACTIVE and
    // hasFocus() is false for every widget in the harness regardless of where
    // focus actually went. focusWidget() is the part the fix moves.
    check(low->window()->focusWidget() == low,
          "focus returns to the readout after a keyboard commit");

    model.setTxFilter(200, 3300);
    openEditorByKey(low, Qt::Key_Space);
    check(!low->isEditing(),
          "Space does not open the editor because the app reserves it for PTT");

    // The keyboard route obeys the same lock gate as the double-click (#745).
    ControlsLock::setLocked(true);
    openEditorByKey(low, Qt::Key_Return);
    check(!low->isEditing(), "a locked panel ignores the keyboard route too (#745)");
    ControlsLock::setLocked(false);

    // ── Validator and parser must agree on one locale (#5064 review) ──────
    //
    // QIntValidator validates in ITS locale; QString::toInt() always parses in
    // the C locale. Under a grouping locale the validator called "3,000"
    // Acceptable, Return fired, and toInt() then refused it — the edit closed
    // having done nothing, which is indistinguishable from the radio ignoring
    // the operator. The validator is now pinned to C with the group separator
    // rejected, so the two halves accept exactly the same strings.
    {
        const QLocale previous = QLocale();
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
        model.setTxFilter(200, 3300);
        if (QLineEdit* ed = openEditor(low)) {
            ed->setText(QStringLiteral("3,000"));
            check(!ed->hasAcceptableInput(),
                  "a grouped number is rejected by the validator, as toInt() would");
            QTest::keyClick(ed, Qt::Key_Return);
        }
        check(!low->isEditing() && model.txFilterLow() == 200,
              "a grouped number closes the editor without a silent no-op commit");

        // The same digits without the separator are the value the operator
        // meant, and they must still work under that locale.
        typeAndCommit(low, QStringLiteral("3000"));
        check(model.txFilterLow() == 3000,
              "plain ASCII digits still commit under a grouping locale");
        QLocale::setDefault(previous);
    }

    // ── Cancel routes ─────────────────────────────────────────────────────
    //
    // Esc is the row that shipped broken, so it is tested the way the APP
    // behaves, not the way a bare harness does. The main window carries
    // QShortcut(Qt::Key_Escape, window()) (CwxPanel, DvkPanel). Qt offers a
    // ShortcutOverride first and, if nothing accepts it, the shortcut fires
    // and the focused widget never receives a KeyPress — which is why the
    // original keyPressEvent handler never ran in the real app while passing
    // a test that delivered Esc straight to the widget.
    //
    // Accepting the ShortcutOverride is therefore the whole fix, and it is
    // asserted directly: without it this row fails and Esc stays dead.
    model.setTxFilter(150, 3300);
    if (QLineEdit* ed = openEditor(low)) {
        ed->setText(QStringLiteral("2000"));
        QKeyEvent probe(QEvent::ShortcutOverride, Qt::Key_Escape, Qt::NoModifier);
        probe.setAccepted(false);
        QApplication::sendEvent(ed, &probe);
        check(probe.isAccepted(),
              "Esc is claimed from the window shortcut (ShortcutOverride accepted)");
        QTest::keyClick(ed, Qt::Key_Escape);
    }
    check(!low->isEditing(), "Esc closes the editor");
    check(model.txFilterLow() == 150, "Esc abandons the edit without committing");
    check(low->text() == QStringLiteral("150"), "Esc leaves the displayed value intact");

    // Qt::Key_Cancel is the same gesture on platforms that send it; VfoWidget
    // treats the two together and so do we.
    model.setTxFilter(150, 3300);
    if (QLineEdit* ed = openEditor(low)) {
        ed->setText(QStringLiteral("2000"));
        QTest::keyClick(ed, Qt::Key_Cancel);
    }
    check(!low->isEditing() && model.txFilterLow() == 150,
          "Key_Cancel abandons the edit the same way Esc does");

    // ── Lock gate: a locked panel is being scrolled, not operated (#745) ──
    ControlsLock::setLocked(true);
    QTest::mouseDClick(low, Qt::LeftButton);
    check(!low->isEditing(), "a locked panel does not open the editor (#745)");
    ControlsLock::setLocked(false);
    QTest::mouseDClick(low, Qt::LeftButton);
    check(low->isEditing(), "unlocking restores double-click-to-type");
    // Flush again before reaching for the editor: the row above opened a NEW
    // one, but an earlier closed editor can still be pending deleteLater and
    // findChild() returns whichever it meets first.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QTest::keyClick(low->findChild<QLineEdit*>(), Qt::Key_Escape);
    check(!low->isEditing(), "the editor closes again after the lock test");

    // ── Blast radius: every OTHER ScrollableLabel is untouched ────────────
    // RIT/XIT (RxApplet), step size (RxApplet) and RTTY mark/shift
    // (VfoWidget) all use this class and must stay display-only.
    ScrollableLabel plain(QStringLiteral("+0 Hz"));
    check(!plain.isEditable(),
          "a ScrollableLabel is NOT editable by default (RIT/XIT/step/RTTY)");
    QTest::mouseDClick(&plain, Qt::LeftButton);
    check(!plain.isEditing(),
          "double-clicking a default ScrollableLabel does nothing");

    // ── A leftover editor is what actually ate the wheel ──────────────────
    //
    // Reported live 2026-08-18: rolling over the numerals did nothing. The
    // editor covers the label exactly, so any editor still on screen is a lid
    // over the control. Two routes used to leave one there.

    // Route 1: focus-out with input QIntValidator calls Intermediate.
    // QLineEdit emits editingFinished on focus-out only when the input is
    // ACCEPTABLE, so this emitted nothing and the editor stayed open forever.
    // The eventFilter's FocusOut branch is what closes it regardless — and
    // now that the clamping fixup() is gone, EVERY non-Acceptable input takes
    // this route, not just the empty one.
    model.setTxFilter(200, 3300);
    if (QLineEdit* ed = openEditor(low)) {
        ed->clear();
        check(!ed->hasAcceptableInput(), "an empty field is not Acceptable (the precondition)");
        QApplication::sendEvent(ed, new QFocusEvent(QEvent::FocusOut));
    }
    check(!low->isEditing(),
          "focus-out always closes the editor, even on unacceptable input");

    // Route 2: with an editor open, the wheel must still step the value —
    // the label underneath can never receive the event itself.
    model.setTxFilter(200, 3300);
    if (QLineEdit* ed = openEditor(low)) {
        QWheelEvent up(QPointF(5, 5), QPointF(5, 5), QPoint(), QPoint(0, 120),
                       Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(ed, &up);
        check(model.txFilterLow() == 250,
              "the wheel still steps while the editor is open");
        check(ed->text() == QStringLiteral("250"),
              "the open field follows the model rather than showing a stale number");
    }
    if (low->isEditing())
        QTest::keyClick(low->findChild<QLineEdit*>(), Qt::Key_Escape);

    // ── The existing affordance still works ───────────────────────────────
    model.setTxFilter(200, 3300);
    QSignalSpy wheelSpy(low, &ScrollableLabel::scrolled);
    QWheelEvent up(QPointF(5, 5), QPointF(5, 5), QPoint(), QPoint(0, 120),
                   Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(low, &up);
    check(wheelSpy.count() == 1, "making the label editable did not break wheel stepping");

    std::printf("%s\n", failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
