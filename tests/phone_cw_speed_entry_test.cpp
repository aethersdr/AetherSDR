#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/TransmitDelta.h"
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
    QSlider* slider = nullptr;
    for (QLineEdit* candidate : applet.findChildren<QLineEdit*>()) {
        if (candidate->accessibleName() == QStringLiteral("CW speed value")) {
            edit = candidate;
        }
    }
    for (QSlider* candidate : applet.findChildren<QSlider*>()) {
        if (candidate->accessibleName() == QStringLiteral("CW speed")) {
            slider = candidate;
        }
    }
    if (!edit || !slider) {
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
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
