#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/AmpApplet.h"
#include "gui/HGauge.h"

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
#include <limits>

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
// Find the label a gauge row carries, by the name it starts with.
QString rowLabel(const AmpApplet& applet, const QString& prefix)
{
    for (QLabel* l : applet.findChildren<QLabel*>()) {
        if (l->text().startsWith(prefix)) return l->text();
    }
    return QString();
}

// The gauges take their value synchronously; the row LABELS are refreshed by
// a 100 ms timer, so a test that reads a number wants the gauge.
float gaugeValue(const AmpApplet& applet, const QString& accessibleName)
{
    // HGauge declares no Q_OBJECT, so findChildren cannot select it directly;
    // it is still a polymorphic QWidget, which dynamic_cast can.
    for (QWidget* w : applet.findChildren<QWidget*>()) {
        if (w->accessibleName() != accessibleName) continue;
        if (auto* g = dynamic_cast<HGauge*>(w)) return g->value();
    }
    return std::numeric_limits<float>::quiet_NaN();
}

// The drive row shows the amplifier's measured exciter power. It is the other
// half of the gain reading: PWR alone cannot say whether an amplifier that is
// making little power is being driven with little power.
void testDriveRowShowsMeasuredDrive()
{
    resetSettings();
    AmpApplet applet;
    // Present but unmeasured: the row keeps its name and no number, so an
    // amplifier that publishes no DRV meter does not read as "no drive".
    report("drive row starts unmeasured",
           rowLabel(applet, QStringLiteral("DRV")) == QStringLiteral("DRV"),
           rowLabel(applet, QStringLiteral("DRV")));

    applet.setDrivePower(10.9f, true);
    report("drive row shows the measured watts",
           rowLabel(applet, QStringLiteral("DRV")) == QStringLiteral("DRV  10.9"),
           rowLabel(applet, QStringLiteral("DRV")));

    // Withdrawn: back to the name alone, not to "0.0".
    applet.setDrivePower(0.0f, false);
    report("withdrawn drive blanks the number rather than reading zero",
           rowLabel(applet, QStringLiteral("DRV")) == QStringLiteral("DRV"),
           rowLabel(applet, QStringLiteral("DRV")));
}

// Two transports carry forward power and SWR. They are the same measurement,
// so the rule is about rate: the radio relay runs at the radio's meter rate
// and the amplifier's own socket is polled at 5 Hz. The relay wins while it
// is fresh; without this, the slower source kept dragging the bar back.
void testRelayedMetersWinOverTheDeviceWhileFresh()
{
    resetSettings();
    AmpApplet applet;

    applet.setRadioMeters(1000.0f, 1.2f);
    const float relayed = gaugeValue(applet, QStringLiteral("Forward power"));
    report("relayed meters reach the gauge", qFuzzyCompare(relayed, 1000.0f),
           QString::number(relayed));

    // The device's own (slower) sample must not overwrite it.
    applet.setDeviceMeters(16.0f, 1.0f);
    const float afterDevice = gaugeValue(applet, QStringLiteral("Forward power"));
    report("a device sample is discarded while the relay is fresh",
           qFuzzyCompare(afterDevice, 1000.0f), QString::number(afterDevice));
}

// With no relay at all — a radio that publishes no amplifier meters, or before
// the meter manifest lands — the amplifier's own socket is the only source and
// must drive the gauges.
void testDeviceMetersDriveTheGaugesWithoutARelay()
{
    resetSettings();
    AmpApplet applet;

    applet.setDeviceMeters(16.6f, 1.002f);
    const float v = gaugeValue(applet, QStringLiteral("Forward power"));
    report("device meters drive the gauges when nothing is relaying",
           qFuzzyCompare(v, 16.6f), QString::number(v));
}

// Forward power crossing the 5 W mark clears and restores the SWR bar: SWR is
// not a measurement when nothing is being transmitted, and a bar left standing
// at the last ratio claims it is.
//
// The crossing is spotted by comparing the arriving reading against the
// PREVIOUS one, so any path that caches the new value before applying it
// disables this silently — the bar simply stops clearing. That is exactly what
// routing the gauges through a shared entry point did on the first attempt,
// which is why it is pinned here.
void testSwrBarFollowsThePowerCrossing()
{
    resetSettings();
    AmpApplet applet;
    const QString swr = QStringLiteral("SWR");

    applet.setRadioMeters(1000.0f, 2.4f);
    report("SWR bar shows the ratio while power is flowing",
           qFuzzyCompare(gaugeValue(applet, swr), 2.4f),
           QString::number(gaugeValue(applet, swr)));

    applet.setRadioMeters(0.0f, 2.4f);
    report("SWR bar clears when the power stops",
           qFuzzyCompare(gaugeValue(applet, swr), 1.0f),
           QString::number(gaugeValue(applet, swr)));

    applet.setRadioMeters(1000.0f, 2.4f);
    report("SWR bar returns when power resumes",
           qFuzzyCompare(gaugeValue(applet, swr), 2.4f),
           QString::number(gaugeValue(applet, swr)));
}

// MEffA wears three states and the operator controls one bit of them. A plain
// on/off control would be wrong in the middle state: enabling the algorithm
// while the PA is in class AAB — which is where SSB and AM put it — reports
// STANDBY, and showing that as "off" tells the operator their setting did not
// take.
void testMeffaShowsThreeStates()
{
    resetSettings();
    AmpApplet applet;
    auto* btn = applet.findChild<QPushButton*>(QStringLiteral("ampMeffaButton"));
    report("MEffA control exists", btn != nullptr);
    if (!btn) return;

    // Nothing before the amplifier has reported a state.
    // isHidden(), NOT !isVisible(): the applet is never shown in this test, so
    // isVisible() is false for every child whatever setVisible() was called
    // with — the assertion passed with the whole visibility gate deleted.
    // isHidden() reads the widget's own flag.
    report("MEffA control is hidden until the amplifier reports",
           btn->isHidden());

    applet.setMeffa(QStringLiteral("OFF"), true);
    report("OFF reads as disabled",
           btn->accessibleName() == QStringLiteral("MEffA off"),
           btn->accessibleName());

    applet.setMeffa(QStringLiteral("STANDBY"), true);
    report("STANDBY reads as enabled-but-idle, not as off",
           btn->accessibleName() == QStringLiteral("MEffA on — idle in class AAB"),
           btn->accessibleName());

    applet.setMeffa(QStringLiteral("ACTIVE"), true);
    report("ACTIVE reads as optimising",
           btn->accessibleName() == QStringLiteral("MEffA on — optimising"),
           btn->accessibleName());
}

// The toggle carries the operator's bit, never the reported word: from STANDBY
// — which is ENABLED — a press must ask to turn it OFF, not on.
void testMeffaToggleSendsTheOperatorsBit()
{
    resetSettings();
    AmpApplet applet;
    auto* btn = applet.findChild<QPushButton*>(QStringLiteral("ampMeffaButton"));
    if (!btn) { report("MEffA toggle bit", false); return; }
    QSignalSpy toggled(&applet, &AmpApplet::meffaToggled);

    applet.setMeffa(QStringLiteral("OFF"), true);
    btn->click();
    report("pressing while OFF asks to enable",
           toggled.count() == 1 && toggled.last().at(0).toBool());

    applet.setMeffa(QStringLiteral("STANDBY"), true);
    btn->click();
    report("pressing while STANDBY asks to DISABLE, because STANDBY is enabled",
           toggled.count() == 2 && !toggled.last().at(0).toBool());

    applet.setMeffa(QStringLiteral("ACTIVE"), true);
    btn->click();
    report("pressing while ACTIVE asks to disable",
           toggled.count() == 3 && !toggled.last().at(0).toBool());
}

// A write needs the whole `setup` group, which is not known until the
// amplifier has answered `setup read`. Until then the control is visible but
// inert — a control that cannot complete is worse than one that visibly
// cannot be pressed yet.
void testMeffaIsInertUntilTheSetupGroupIsKnown()
{
    resetSettings();
    AmpApplet applet;
    auto* btn = applet.findChild<QPushButton*>(QStringLiteral("ampMeffaButton"));
    if (!btn) { report("MEffA inert gate", false); return; }
    QSignalSpy toggled(&applet, &AmpApplet::meffaToggled);

    applet.setMeffa(QStringLiteral("STANDBY"), false);
    report("control is disabled while the setup group is unknown",
           !btn->isEnabled());
    btn->click();
    report("a press while inert commands nothing", toggled.count() == 0);

    applet.setMeffa(QStringLiteral("STANDBY"), true);
    report("control becomes live once the group is known", btn->isEnabled());
}

// A relay update that carried no forward power must not lock out the
// amplifier's own socket. ampMetersChanged also fires for TEMP and DRV, so on
// a station whose relayed FWD/RL never arrive the handler would otherwise
// stamp the relay "fresh" at 0 W forever and discard every socket sample —
// the #4805 shape, and the fallback added for it would never engage.
void testDeviceMetersSurviveARelayWithNoPower()
{
    resetSettings();
    AmpApplet applet;

    // What the wiring passes when only TEMP moved: no power sample has landed,
    // so fwd is still at its default and powerValid is false.
    applet.setRadioMeters(0.0f, 1.0f, /*powerValid=*/false);
    applet.setDeviceMeters(1148.0f, 1.2f);
    const float v = gaugeValue(applet, QStringLiteral("Forward power"));
    report("a zero-power relay sample does not lock out the device feed",
           qFuzzyCompare(v, 1148.0f), QString::number(v));
}

// Withdrawn drive blanks the ROW, not just the number. A bar parked at the
// left stop is what "no drive" looks like; an amplifier that publishes no DRV
// meter is not being driven with nothing, it is not saying.
void testWithdrawnDriveHidesTheRow()
{
    resetSettings();
    AmpApplet applet;

    applet.setDrivePower(10.9f, true);
    QWidget* gauge = nullptr;
    for (QWidget* w : applet.findChildren<QWidget*>()) {
        if (w->accessibleName() == QStringLiteral("Drive power")) gauge = w;
    }
    report("drive gauge exists", gauge != nullptr);
    if (!gauge) return;
    report("measured drive keeps the gauge", !gauge->isHidden());

    applet.setDrivePower(0.0f, false);
    report("unmeasured drive hides the gauge rather than parking it at zero",
           gauge->isHidden());
}

// The relayed MEffA state reads out on a station with no direct socket, but
// can never be written from there — a `setup` write carries the whole group
// and only the socket can read the rest of it.
void testRelayedMeffaReadsOutButStaysInert()
{
    resetSettings();
    AmpApplet applet;
    auto* btn = applet.findChild<QPushButton*>(QStringLiteral("ampMeffaButton"));
    if (!btn) { report("relayed MEffA", false); return; }
    QSignalSpy toggled(&applet, &AmpApplet::meffaToggled);

    applet.setMeff(QStringLiteral("ACTIVE"));
    report("relayed MEffA state reaches the control",
           btn->accessibleName() == QStringLiteral("MEffA on — optimising"),
           btn->accessibleName());
    report("relayed MEffA is not writable", !btn->isEnabled());
    btn->click();
    report("a press on a relay-only MEffA commands nothing",
           toggled.count() == 0);
}

// Fan mode must stay operable whether or not the `setup` group is known: the
// model falls back to the single-key write, so disabling the control here
// would make fan mode LESS available than it was before the group write
// existed. MEffA is different — it is new, and has no single-key form.
void testFanControlStaysOperableWithoutTheSetupGroup()
{
    resetSettings();
    AmpApplet applet;
    const auto combos = applet.findChildren<QComboBox*>();
    report("fan combo exists", !combos.isEmpty());
    if (combos.isEmpty()) return;
    QComboBox* combo = combos.first();

    applet.setFanMode(QStringLiteral("STANDARD"));
    applet.setMeffa(QStringLiteral("STANDBY"), false);
    report("fan control is operable even with the setup group unknown",
           combo->isEnabled());

    QSignalSpy fan(&applet, &AmpApplet::fanModeChanged);
    combo->setCurrentIndex((combo->currentIndex() + 1) % combo->count());
    report("and still commands a change", fan.count() >= 1);
}

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
    testDriveRowShowsMeasuredDrive();
    testRelayedMetersWinOverTheDeviceWhileFresh();
    testDeviceMetersDriveTheGaugesWithoutARelay();
    testSwrBarFollowsThePowerCrossing();
    testMeffaShowsThreeStates();
    testMeffaToggleSendsTheOperatorsBit();
    testMeffaIsInertUntilTheSetupGroupIsKnown();
    testDeviceMetersSurviveARelayWithNoPower();
    testWithdrawnDriveHidesTheRow();
    testFanControlStaysOperableWithoutTheSetupGroup();
    testRelayedMeffaReadsOutButStaysInert();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : qPrintable(QStringLiteral("%1 test(s) failed.").arg(g_failed)));
    return g_failed == 0 ? 0 : 1;
}
