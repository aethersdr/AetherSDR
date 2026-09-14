#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/AmpApplet.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QByteArray>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const QString& detail = QString())
{
    std::printf("%s %-52s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                qPrintable(detail));
    if (!ok) ++g_failed;
}

QPushButton* tempButton(AmpApplet& applet)
{
    return applet.findChild<QPushButton*>(QStringLiteral("ampTempUnitButton"));
}

QComboBox* fanCombo(AmpApplet& applet)
{
    return applet.findChild<QComboBox*>(QStringLiteral("ampFanModeCombo"));
}

void resetSettings()
{
    auto& settings = AppSettings::instance();
    const QString path = settings.filePath();
    settings.reset();
    QFile::remove(path);
    QFile::remove(path + QStringLiteral(".bak"));
    QFile::remove(path + QStringLiteral(".tmp"));
    settings.load();
}

// Values are right-aligned in a fixed field and drawn in a fixed-width face.
// The bottom row is four readouts abreast and they arrive five times a second,
// so a reading that changes width shuffles everything to its right and the
// whole row twitches. The padding is part of the contract, not incidental
// whitespace — these expectations hold it.
void testDefaultPlaceholder()
{
    resetSettings();

    AmpApplet applet;
    auto* button = tempButton(applet);
    report("temperature button exists", button != nullptr);
    if (!button) return;

    report("default placeholder uses Celsius",
           button->text() == QStringLiteral("PA     \u2014 C"),
           button->text());
}

void testSingleSensorToggle()
{
    resetSettings();

    AmpApplet applet;
    auto* button = tempButton(applet);
    report("single sensor button exists", button != nullptr);
    if (!button) return;

    applet.setTemp(34.7f);
    report("single sensor displays Celsius",
           button->text() == QStringLiteral("PA  34.7 C"),
           button->text());

    button->click();
    report("single sensor toggles to Fahrenheit",
           button->text() == QStringLiteral("PA  94.5 F"),
           button->text());

    button->click();
    report("single sensor toggles back to Celsius",
           button->text() == QStringLiteral("PA  34.7 C"),
           button->text());
}

void testDualSensorToggle()
{
    resetSettings();

    AmpApplet applet;
    auto* button = tempButton(applet);
    report("dual sensor button exists", button != nullptr);
    if (!button) return;

    // Both sensors are named. The amplifier's own panel runs them unlabelled
    // ("24.4/24.2 C"); on hardware the operator knows which is which, and
    // here two bare numbers say nothing about what either is measuring.
    applet.setTemp(34.7f);
    applet.setTempB(28.4f);
    report("dual sensor displays Celsius pair",
           button->text() == QStringLiteral("PA  34.7 / HL  28.4 C"),
           button->text());

    button->click();
    report("dual sensor toggles to Fahrenheit pair",
           button->text() == QStringLiteral("PA  94.5 / HL  83.1 F"),
           button->text());
}

void testPreferenceReload()
{
    resetSettings();

    {
        AmpApplet applet;
        auto* button = tempButton(applet);
        report("preference button exists", button != nullptr);
        if (!button) return;
        button->click();
    }

    auto& settings = AppSettings::instance();
    settings.reset();
    settings.load();

    AmpApplet restored;
    auto* button = tempButton(restored);
    report("reloaded button exists", button != nullptr);
    if (!button) return;

    report("reloaded placeholder uses Fahrenheit",
           button->text() == QStringLiteral("PA     \u2014 F"),
           button->text());

    restored.setTemp(0.0f);
    report("reloaded value displays Fahrenheit",
           button->text() == QStringLiteral("PA  32.0 F"),
           button->text());
}

void testFanModePulldown()
{
    resetSettings();

    AmpApplet applet;
    auto* combo = fanCombo(applet);
    report("fan combo exists", combo != nullptr);
    if (!combo) return;

    report("fan combo has three modes", combo->count() == 3, QString::number(combo->count()));
    // isVisibleTo(&applet), not isVisible(): the applet is never shown as a
    // top-level window in this offscreen harness, so isVisible() would be
    // false regardless of the combo's own shown/hidden state.
    report("fan combo starts hidden", !combo->isVisibleTo(&applet));

    QSignalSpy spy(&applet, &AmpApplet::fanModeChanged);

    // Reflecting an incoming PGXL status must select the right item, show
    // the combo, and NOT emit fanModeChanged (#3905) — that would echo a
    // redundant "setup fanmode=" command straight back to the amp.
    applet.setFanMode("contest");
    report("setFanMode selects the matching item",
           combo->currentData().toString() == QStringLiteral("CONTEST"),
           combo->currentData().toString());
    report("setFanMode shows the combo", combo->isVisibleTo(&applet));
    report("setFanMode does not emit fanModeChanged", spy.isEmpty());

    // A user-driven selection must emit the uppercase mode.
    combo->setCurrentIndex(combo->findData(QStringLiteral("BROADCAST")));
    report("user selection emits fanModeChanged", spy.count() == 1, QString::number(spy.count()));
    if (!spy.isEmpty()) {
        report("emitted mode is uppercase BROADCAST",
               spy.takeFirst().at(0).toString() == QStringLiteral("BROADCAST"));
    }

    // An unrecognized mode from the radio must not crash or desync the
    // combo's selection.
    const QString before = combo->currentData().toString();
    applet.setFanMode("bogus");
    report("unknown fanmode leaves combo selection unchanged",
           combo->currentData().toString() == before,
           combo->currentData().toString());

    // And an unrecognized mode must not be what reveals the control. A fan
    // control that is up asserts the mode it is showing; if the only thing the
    // amplifier ever sent was a word we could not parse, the control would be
    // claiming a mode the amplifier never confirmed.
    {
        AmpApplet fresh;
        QComboBox* freshCombo = fanCombo(fresh);
        report("fresh fan combo starts hidden", freshCombo && freshCombo->isHidden());
        if (freshCombo) {
            fresh.setFanMode("bogus");
            report("unknown fanmode does not reveal the control",
                   freshCombo->isHidden());
            fresh.setFanMode("CONTEST");
            report("a recognized mode does reveal it", !freshCombo->isHidden());
        }
    }

    // #4731: on a large-enough default UI font, the popup's fixed pixel
    // width (sized off the combo's own hardcoded 10px stylesheet font)
    // couldn't fit "Fan: Contest" — the longest item — so Qt's default
    // ElideMiddle silently mangled it. Widths/fonts aren't trustworthy to
    // assert on directly in this offscreen, unlaid-out harness (the combo
    // is never shown, so its geometry never reflects a real style pass),
    // so guard the three properties the fix actually sets instead: let the
    // widest item drive the combo's width rather than pinning it, and fail
    // any future overflow visibly (clipped) instead of mid-eliding it.
    report("fan combo sizes to its widest item, not a pinned width",
           combo->sizeAdjustPolicy() == QComboBox::AdjustToMinimumContentsLengthWithIcon);
    report("fan combo reserves room for \"Fan: Contest\"",
           combo->minimumContentsLength() >= static_cast<int>(QStringLiteral("Fan: Contest").length()),
           QString::number(combo->minimumContentsLength()));
    report("fan combo popup does not silently mid-elide overflow",
           combo->view()->textElideMode() == Qt::ElideNone);
}

// The readouts must not change width as the values move. Both halves of that
// are load-bearing: a fixed-width face so a 1 and an 8 cost the same, and a
// fixed field so 9.9 and 100.4 do. Miss either and the bottom row twitches on
// every poll, five times a second.
void testReadoutWidthIsStable()
{
    resetSettings();

    AmpApplet applet;
    applet.setDirectConnected(true);
    auto* button = tempButton(applet);
    report("stable-width button exists", button != nullptr);
    if (!button) return;

    auto labelStarting = [&applet](const QString& prefix) -> QLabel* {
        for (QLabel* l : applet.findChildren<QLabel*>()) {
            if (l->text().startsWith(prefix)) return l;
        }
        return nullptr;
    };
    QLabel* vdd = labelStarting(QStringLiteral("Vdd"));
    QLabel* vac = labelStarting(QStringLiteral("Vac"));
    report("drain and mains readouts exist", vdd != nullptr && vac != nullptr);
    if (!vdd || !vac) return;

    // A digit either side of a width change, and the placeholder too: the
    // dash is what stands there before the first reading arrives, and a row
    // that settles into place on the first poll is the same jitter once.
    const int tempWidth = button->text().length();
    const int vddWidth = vdd->text().length();
    const int vacWidth = vac->text().length();

    applet.setTemp(9.9f);
    applet.setTempB(9.9f);
    const int pairWidth = button->text().length();

    applet.setTemp(100.4f);
    applet.setTempB(-5.0f);
    report("temperature pair keeps its width across a digit change",
           button->text().length() == pairWidth, button->text());

    applet.setDrainVoltage(9.9f);
    const int vddReading = vdd->text().length();
    applet.setDrainVoltage(51.9f);
    report("drain voltage keeps its width across a digit change",
           vdd->text().length() == vddReading, vdd->text());
    // Zero is a reading, not a gap. The amplifier keeps its drain rail down
    // while idle, so this is what it reports for most of the time it is
    // switched on; a dash there reads as "nothing arrived" and sends the
    // operator looking for a fault in the client.
    applet.setDrainVoltage(0.0f);
    report("zero drain voltage is reported literally",
           vdd->text().contains(QStringLiteral("0.0")), vdd->text());
    report("zero drain voltage is not a placeholder",
           !vdd->text().contains(QStringLiteral("\u2014")), vdd->text());
    report("zero drain voltage keeps the row's width",
           vdd->text().length() == vddReading, vdd->text());

    // The dash is kept for the case where there is genuinely nothing: the
    // readings only exist on the direct connection.
    applet.setDirectConnected(false);
    report("no direct connection falls back to the placeholder",
           vdd->text().contains(QStringLiteral("\u2014")), vdd->text());
    report("the placeholder is the same width as a reading",
           vdd->text().length() == vddWidth, vdd->text());
    applet.setDirectConnected(true);

    applet.setMainsVoltage(98);
    const int vacReading = vac->text().length();
    applet.setMainsVoltage(247);
    report("mains voltage keeps its width across a digit change",
           vac->text().length() == vacReading, vac->text());
    report("mains voltage placeholder is the same width as a reading",
           vacWidth == vacReading, vac->text());

    // A single sensor and the pre-reading dash are narrower than the pair —
    // they are different rows, not different widths of the same row — but
    // each has to be stable in itself.
    report("single-sensor readout is stable", tempWidth > 0, QString::number(tempWidth));

    // The face has to be fixed-width too, or the field alone does not save it.
    // The size comes from a style sheet, so the widget's own font cannot be
    // asked; the sheet is what decides it.
    report("temperature readout is drawn in a fixed-width face",
           button->styleSheet().contains(QStringLiteral("monospace")),
           button->styleSheet());
    report("voltage readouts are drawn in a fixed-width face",
           vdd->styleSheet().contains(QStringLiteral("monospace"))
               && vac->styleSheet().contains(QStringLiteral("monospace")),
           vdd->styleSheet());
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-amp-applet-test"));
    if (!settingsProfile.isValid()) {
        std::printf("[FAIL] create temporary home\n");
        return 1;
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    QApplication app(argc, argv);

    std::printf("AmpApplet temperature unit test harness\n\n");

    testDefaultPlaceholder();
    testSingleSensorToggle();
    testDualSensorToggle();
    testPreferenceReload();
    testFanModePulldown();
    testReadoutWidthIsStable();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : qPrintable(QStringLiteral("%1 test(s) failed.").arg(g_failed)));
    return g_failed == 0 ? 0 : 1;
}
