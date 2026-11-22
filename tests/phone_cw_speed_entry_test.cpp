#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/TransmitDelta.h"
#include "gui/CenterMarkSlider.h"
#include "gui/PhoneCwApplet.h"
#include "models/TransmitModel.h"

#include <QApplication>
#include <QCoreApplication>
#include <QLineEdit>
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
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
