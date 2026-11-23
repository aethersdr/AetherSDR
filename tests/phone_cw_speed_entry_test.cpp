#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/TransmitDelta.h"
#include "gui/CenterMarkSlider.h"
#include "gui/PhoneCwApplet.h"
#include "models/TransmitModel.h"

#include <QAccessible>
#include <QApplication>
#include <QCoreApplication>
#include <QLineEdit>
#include <QStringList>
#include <QSignalSpy>
#include <QSlider>
#include <QTest>

#include <cstdio>

using namespace AetherSDR;

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("phone-cw-speed-entry-test"));
    QApplication app(argc, argv);
    AppSettings::instance().load();
    TransmitModel model;
    PhoneCwApplet applet;
    applet.setTransmitModel(&model);
    applet.setMode(QStringLiteral("CW"));
    applet.show();
    applet.activateWindow();
    QTest::qWait(10);

    QLineEdit* edit = nullptr;
    QLineEdit* pitchEdit = nullptr;
    QSlider* slider = nullptr;
    for (QLineEdit* candidate : applet.findChildren<QLineEdit*>()) {
        if (candidate->accessibleName() == QStringLiteral("CW speed value")) {
            edit = candidate;
        } else if (candidate->accessibleName()
                   == QStringLiteral("CW pitch frequency")) {
            pitchEdit = candidate;
        }
    }
    for (QSlider* candidate : applet.findChildren<QSlider*>()) {
        if (candidate->accessibleName() == QStringLiteral("CW speed")) {
            slider = candidate;
        }
    }
    if (!edit || !pitchEdit || !slider) {
        return 1;
    }

    int failures = 0;
    const auto check = [&failures](bool ok, const char* label) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) {
            ++failures;
        }
    };
    const auto refreshLimits = [&applet] {
        applet.setCwControlLimits(5, 100, 100, 6000, 10);
    };
    const auto type = [edit](const QString& text) {
        edit->setFocus();
        edit->selectAll();
        QTest::keyClicks(edit, text);
    };
    QSignalSpy commands(&model, &TransmitModel::cwSpeedCommandIssued);
    refreshLimits();
    edit->setFocus();
    QCoreApplication::processEvents();
    check(edit->hasFocus(), "the editor really owns keyboard focus");

    // A GPS/oscillator update re-applies the same capability limits while
    // the first digit of 25 is still Intermediate to QIntValidator.
    type(QStringLiteral("2"));
    check(!edit->hasAcceptableInput(), "the first digit is an incomplete WPM");
    refreshLimits();
    check(edit->text() == QStringLiteral("2"), "unchanged limits preserve the first digit");
    TransmitDelta report;
    report.cwSpeed = 22;
    model.applyChanges(report);
    check(edit->text() == QStringLiteral("2") && slider->value() == 22,
          "radio state updates the slider while preserving an unfinished edit");
    edit->selectAll();
    QTest::keyClick(edit, Qt::Key_Backspace);
    refreshLimits();
    check(edit->text().isEmpty(), "unchanged limits preserve an empty active edit");
    check(commands.isEmpty(), "refreshing limits never issues a speed command");

    type(QStringLiteral("25"));
    refreshLimits();
    QTest::keyClick(edit, Qt::Key_Return);
    check(model.cwSpeed() == 25 && slider->value() == 25
              && edit->text() == QStringLiteral("25"),
          "Return commits typed WPM to both model and slider");
    check(commands.count() == 1 && commands.last().at(0).toInt() == 25,
          "Return issues exactly the requested speed");

    type(QStringLiteral("30"));
    slider->setFocus();
    QCoreApplication::processEvents();
    check(model.cwSpeed() == 30 && edit->text() == QStringLiteral("30"),
          "focus-out also commits typed WPM");
    slider->setValue(35);
    check(model.cwSpeed() == 35 && edit->text() == QStringLiteral("35"),
          "slider adjustment still synchronizes the value field");

    type(QStringLiteral("2"));
    commands.clear();
    applet.setCwControlLimits(10, 60, 100, 6000, 10);
    check(edit->text() == QStringLiteral("35") && commands.isEmpty(),
          "a changed range reconciles an unacceptable edit without a command");
    type(QStringLiteral("25"));
    applet.setCwControlLimits(15, 50, 100, 6000, 10);
    check(edit->text() == QStringLiteral("25") && commands.isEmpty(),
          "a changed range preserves an edit that is still acceptable");
    QTest::keyClick(edit, Qt::Key_Return);
    check(model.cwSpeed() == 25, "an edit valid in the new range still commits");

    slider->setFocus();
    applet.setCwControlLimits(40, 60, 100, 6000, 10);
    check(slider->value() == 40 && edit->text() == QStringLiteral("40"),
          "unfocused readout follows a genuine range clamp");

    // Pitch shares the refresh and the rule: its 100 Hz minimum makes the
    // first digit of 650 Intermediate, exactly as 2 is for 25 WPM.
    QSignalSpy pitchCommands(&model, &TransmitModel::cwPitchCommandIssued);
    pitchEdit->setFocus();
    pitchEdit->selectAll();
    QTest::keyClicks(pitchEdit, QStringLiteral("6"));
    check(pitchEdit->hasFocus() && !pitchEdit->hasAcceptableInput(),
          "the first pitch digit is an incomplete Hz value");
    applet.setCwControlLimits(40, 60, 100, 6000, 10);
    check(pitchEdit->text() == QStringLiteral("6") && pitchCommands.isEmpty(),
          "unchanged limits preserve the first pitch digit");
    QTest::keyClicks(pitchEdit, QStringLiteral("50"));
    QTest::keyClick(pitchEdit, Qt::Key_Return);
    check(model.cwPitch() == 650 && pitchEdit->text() == QStringLiteral("650"),
          "Return commits typed pitch");
    pitchEdit->selectAll();
    QTest::keyClicks(pitchEdit, QStringLiteral("5"));
    applet.setCwControlLimits(40, 60, 200, 6000, 10);
    check(pitchEdit->text() == QStringLiteral("650"),
          "a changed pitch range reconciles an unacceptable edit");
    // #7325: QLineEdit sends no editingFinished for Intermediate text, so
    // leaving the field must put the current value back, and putting it
    // back must not send anything.
    commands.clear();
    type(QStringLiteral("2"));
    slider->setFocus();
    QCoreApplication::processEvents();
    check(edit->text() == QString::number(slider->value()) && commands.isEmpty(),
          "focus-out restores an incomplete WPM without a speed command");
    edit->setFocus();
    edit->selectAll();
    QTest::keyClick(edit, Qt::Key_Backspace);
    slider->setFocus();
    QCoreApplication::processEvents();
    check(edit->text() == QString::number(slider->value()) && commands.isEmpty(),
          "focus-out restores an empty WPM field without a speed command");
    pitchCommands.clear();
    pitchEdit->setFocus();
    pitchEdit->selectAll();
    QTest::keyClicks(pitchEdit, QStringLiteral("6"));
    slider->setFocus();
    QCoreApplication::processEvents();
    check(pitchEdit->text() == QString::number(model.cwPitch()),
          "focus-out restores an incomplete pitch");
    check(pitchCommands.isEmpty(),
          "restoring the pitch sends no cw pitch command");
    // The operator's way out of the field: a mouse click on another one.
    commands.clear();
    pitchCommands.clear();
    type(QStringLiteral("2"));
    QTest::mouseClick(pitchEdit, Qt::LeftButton);
    QCoreApplication::processEvents();
    check(pitchEdit->hasFocus(), "a mouse click moves focus to the pitch field");
    check(edit->text() == QString::number(slider->value()) && commands.isEmpty()
              && pitchCommands.isEmpty(),
          "clicking away restores an incomplete WPM without a command");
    type(QStringLiteral("2"));
    QTest::keyClick(edit, Qt::Key_Return);
    QCoreApplication::processEvents();
    check(edit->hasFocus() && edit->text() == QStringLiteral("2"),
          "Return on an incomplete WPM keeps the edit as typed");
    slider->setFocus();
    QCoreApplication::processEvents();

    // #7260: the CW sidetone pan is the shared centre-out pan slider.
    QSlider* pan = nullptr;
    for (QSlider* candidate : applet.findChildren<QSlider*>()) {
        if (candidate->accessibleName() == QStringLiteral("CW audio pan")) {
            pan = candidate;
        }
    }
    check(pan && dynamic_cast<CenterMarkSlider*>(pan) != nullptr,
          "the CW pan is a centre-mark pan slider");
    if (pan) {
        pan->setValue(20);
        check(model.monPanCw() == 20, "moving the CW pan sets mon_pan_cw");
        QTest::mouseDClick(pan, Qt::LeftButton);
        check(pan->value() == 50 && model.monPanCw() == 50,
              "double-click recentres the CW pan");
    }
    check(panText(50) == QStringLiteral("C") && panText(20) == QStringLiteral("L30")
              && panText(80) == QStringLiteral("R30"),
          "the pan readout is C, L30 or R30");
    // Leaving Delay or Sidetone with Intermediate text (an empty field, or an
    // over-range number such as "3000" in the 0-2000 delay) puts the current
    // value back too. (APF shares the same filter but is disabled with no
    // slice bound.)
    const auto findEdit = [&applet](const char* name) -> QLineEdit* {
        for (QLineEdit* candidate : applet.findChildren<QLineEdit*>()) {
            if (candidate->accessibleName() == QLatin1String(name)) {
                return candidate;
            }
        }
        return nullptr;
    };
    const auto typeThenLeave = [&slider, &check](QLineEdit* field, const char* text,
                                                 const char* what) {
        field->setFocus();
        field->selectAll();
        QTest::keyClick(field, Qt::Key_Backspace);
        QTest::keyClicks(field, QString::fromLatin1(text));
        check(field->hasFocus() && field->text() == QLatin1String(text)
                  && !field->hasAcceptableInput(),
              what);
        slider->setFocus();
        QCoreApplication::processEvents();
    };
    QLineEdit* delayEdit = findEdit("CW delay value");
    QLineEdit* sidetoneEdit = findEdit("Sidetone volume value");
    check(delayEdit && sidetoneEdit, "the delay and sidetone value fields exist");
    if (delayEdit && sidetoneEdit) {
        const int delayBefore = model.cwDelay();
        typeThenLeave(delayEdit, "", "the delay field has focus and is empty before leaving");
        check(delayEdit->text() == QString::number(delayBefore)
                  && model.cwDelay() == delayBefore,
              "focus-out restores an empty delay field without changing the delay");
        typeThenLeave(delayEdit, "3000",
                      "the delay field has focus and holds an over-range 3000 before leaving");
        check(delayEdit->text() == QString::number(delayBefore)
                  && model.cwDelay() == delayBefore,
              "focus-out restores an over-range delay without changing the delay");
        const int sidetoneBefore = model.monGainCw();
        typeThenLeave(sidetoneEdit, "",
                      "the sidetone field has focus and is empty before leaving");
        check(sidetoneEdit->text() == QString::number(sidetoneBefore)
                  && model.monGainCw() == sidetoneBefore,
              "focus-out restores an empty sidetone field without changing the level");
    }

    // The accessible Value reads the pan the way the operator sees it. That is
    // what Windows UI Automation's ValuePattern reports; macOS VoiceOver reads
    // the number instead.
    if (pan) {
        pan->setValue(20);
        QAccessibleInterface* a11y = QAccessible::queryAccessibleInterface(pan);
        check(a11y && a11y->role() == QAccessible::Slider,
              "the CW pan is announced as a slider");
        check(a11y && a11y->text(QAccessible::Value) == QStringLiteral("L30"),
              "the CW pan's accessible value is L30, not 20");
        check(a11y && a11y->valueInterface()
                  && a11y->valueInterface()->currentValue().toInt() == 20,
              "the numeric value is still there for assistive tech that wants it");
        pan->setValue(50);
        check(a11y && a11y->text(QAccessible::Value) == QStringLiteral("C"),
              "the centred pan reads C");

        // A value change also posts the text, which on Windows raises UIA's
        // Value property change rather than only its RangeValue one.
        static QObject* s_watched = nullptr;
        static QStringList s_valueTexts;
        static QAccessible::UpdateHandler s_previous = nullptr;
        s_watched = pan;
        s_valueTexts.clear();
        QAccessible::setActive(true);
        s_previous = QAccessible::installUpdateHandler([](QAccessibleEvent* event) {
            if (event->object() == s_watched && event->type() == QAccessible::ValueChanged) {
                const QVariant value = static_cast<QAccessibleValueChangeEvent*>(event)->value();
                if (value.typeId() == QMetaType::QString) {
                    s_valueTexts << value.toString();
                }
            }
            if (s_previous != nullptr) {
                s_previous(event);
            }
        });
        check(QAccessible::isActive(), "accessibility is active for the event check");
        pan->setValue(20);
        check(s_valueTexts == QStringList{QStringLiteral("L30")},
              "a pan value change posts the text L30 to assistive tech");
        QAccessible::installUpdateHandler(s_previous);
        QAccessible::setActive(false);
        s_watched = nullptr;
        pan->setValue(50);
    }
    // Every CenterMarkSlider gets this, including the VFO pan, which never
    // set a readout of its own.
    CenterMarkSlider bare(50, Qt::Horizontal);
    bare.setRange(0, 100);
    bare.setValue(80);
    QAccessibleInterface* bareA11y = QAccessible::queryAccessibleInterface(&bare);
    check(bareA11y && bareA11y->text(QAccessible::Value) == QStringLiteral("R30"),
          "a CenterMarkSlider with no formatter set still reads R30");
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
