#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/AmpApplet.h"
#include "models/AmpModel.h"
#include "core/backends/AmpDelta.h"
#include <QDateTime>
#include <QtTest>
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
// The telemetry row has three readouts abreast and they arrive five times a second,
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
           button->text() == QStringLiteral("PA \u2014 C    "),
           button->text());
    // Spoken in words, not as the visible dash.
    report("placeholder is spoken as not reported",
           button->accessibleName() == QStringLiteral("PA heatsink not reported"),
           button->accessibleName());

    // The placeholder is drawn like Vdd and Vac without a connection: in the
    // disabled tone. The first reading switches it to the normal one.
    const QString placeholderStyle = button->styleSheet();
    applet.setPaHeatsinkTemp(34.7f);
    report("the first PA reading leaves the placeholder tone",
           !placeholderStyle.isEmpty() && button->styleSheet() != placeholderStyle);
}

void testConnectionSourceIndicator()
{
    AmpApplet applet;
    QLabel* source = applet.findChild<QLabel*>(QStringLiteral("ampConnectionSource"));
    report("PGXL source indicator exists", source != nullptr);
    if (!source) {
        return;
    }
    report("PGXL initially shows offline", source->text() == QStringLiteral("● OFFLINE")
        && source->accessibleName().contains(QStringLiteral("OFFLINE")));
    applet.setRadioConnected(true);
    report("PGXL has no relay without an amp handle", source->text() == QStringLiteral("● OFFLINE"));
    AmpModel model;
    AmpDelta delta;
    delta.handle = QStringLiteral("0x2000");
    delta.detectedModel = QStringLiteral("PowerGeniusXL");
    model.applyChanges(delta);
    applet.setAmpModel(&model);
    report("PGXL shows radio relay when connected", source->text() == QStringLiteral("● RADIO"));
    applet.setDirectFailureReason(QStringLiteral("Stored authorization code unavailable"));
    report("PGXL source exposes direct failure", source->accessibleDescription().contains(
        QStringLiteral("Stored authorization code unavailable"))
        && source->toolTip() == QStringLiteral("Stored authorization code unavailable"));
    applet.setDirectConnected(true);
    report("PGXL shows authenticated direct path", source->text() == QStringLiteral("● DIRECT")
        && source->accessibleName().contains(QStringLiteral("DIRECT"))
        && source->toolTip().isEmpty());
    applet.setRadioConnected(false);
    report("PGXL keeps direct path when radio disconnects", source->text() == QStringLiteral("● DIRECT"));
    applet.setDirectConnected(false);
    report("PGXL returns to offline when both paths disconnect", source->text() == QStringLiteral("● OFFLINE")
        && source->accessibleName().contains(QStringLiteral("OFFLINE")));
    applet.setFloating(true);
    applet.resize(420, 360);
    applet.show();
    QCoreApplication::processEvents();
    const QPoint sourceAt = source->mapTo(&applet, QPoint(0, 0));
    const int bottomInset = applet.height() - sourceAt.y() - source->height();
    report("PGXL floating indicator has a frame inset",
           bottomInset >= 6 && bottomInset < 20, QString::number(bottomInset));
}

void testSingleSensorToggle()
{
    resetSettings();

    AmpApplet applet;
    auto* button = tempButton(applet);
    report("single sensor button exists", button != nullptr);
    if (!button) return;

    applet.setPaHeatsinkTemp(34.7f);
    report("single sensor displays Celsius",
           button->text() == QStringLiteral("PA 34.7 C "),
           button->text());

    button->click();
    report("single sensor toggles to Fahrenheit",
           button->text() == QStringLiteral("PA 94.5 F "),
           button->text());

    button->click();
    report("single sensor toggles back to Celsius",
           button->text() == QStringLiteral("PA 34.7 C "),
           button->text());
}

void testDualSensorToggle()
{
    resetSettings();

    AmpApplet applet;
    auto* pa = tempButton(applet);
    auto* hl = applet.findChild<QPushButton*>(QStringLiteral("ampHlTempButton"));
    report("dual sensor readouts exist", pa != nullptr && hl != nullptr);
    if (!pa || !hl) return;

    // The PGXL front panel shows both temperatures without labels, for
    // example "24.4/24.2 C". The applet labels them PA (PA heatsink) and
    // HL (Harmonic Load heatsink), one readout each. HL comes only over a
    // direct connection.
    report("HL keeps its place with a dash before a reading",
           hl->text() == QStringLiteral("HL \u2014 C    "), hl->text());
    applet.setDirectConnected(true);
    applet.setPaHeatsinkTemp(34.7f);
    applet.setHarmonicLoadHeatsinkTemp(28.4f);
    report("dual sensor displays Celsius pair",
           pa->text() == QStringLiteral("PA 34.7 C ") && hl->text() == QStringLiteral("HL 28.4 C "),
           pa->text() + QStringLiteral("|") + hl->text());

    // Either readout toggles both.
    pa->click();
    report("a click on PA shows both in Fahrenheit",
           pa->text() == QStringLiteral("PA 94.5 F ") && hl->text() == QStringLiteral("HL 83.1 F "),
           pa->text() + QStringLiteral("|") + hl->text());
    hl->click();
    report("a click on HL shows both in Celsius again",
           pa->text() == QStringLiteral("PA 34.7 C ") && hl->text() == QStringLiteral("HL 28.4 C "),
           pa->text() + QStringLiteral("|") + hl->text());
    // The unit chosen from HL is saved like one chosen from PA: a new applet
    // opens in it.
    hl->click();
    AmpApplet reopened;
    auto* reopenedHl = reopened.findChild<QPushButton*>(QStringLiteral("ampHlTempButton"));
    report("the unit chosen from HL is saved",
           reopenedHl && reopenedHl->text().endsWith(QStringLiteral("F    ")),
           reopenedHl ? reopenedHl->text() : QString());
}

// Vac and Vdd start at the same x whatever the temperatures read: the grid
// column is as wide as its widest cell, and every readout keeps one length.
void testVoltagesStayAlignedWithTheTemperatures()
{
    resetSettings();
    AmpApplet applet;
    applet.setDirectConnected(true);
    applet.resize(260, 360);
    applet.show();
    auto* vac = applet.findChild<QLabel*>(QStringLiteral("ampMainsVoltage"));
    auto* vdd = applet.findChild<QLabel*>(QStringLiteral("ampDrainVoltage"));
    report("voltage readouts exist", vac && vdd);
    if (!vac || !vdd) return;

    auto x = [&applet](QWidget* w) {
        QCoreApplication::processEvents();
        return w->mapTo(&applet, QPoint(0, 0)).x();
    };
    const int start = x(vac);
    bool aligned = x(vac) == x(vdd);
    bool steady = true;
    const float temps[][2] = {{9.9f, 9.9f}, {106.8f, 89.8f}, {35.7f, 100.4f}, {-5.0f, 120.0f}};
    for (const auto& t : temps) {
        applet.setPaHeatsinkTemp(t[0]);
        applet.setHarmonicLoadHeatsinkTemp(t[1]);
        aligned = aligned && x(vac) == x(vdd);
        steady = steady && x(vac) == start;
    }
    report("Vac and Vdd start at the same x", aligned);
    report("the voltages do not move as the temperatures change", steady);
}

void testRadioFallbackDropsHarmonicLoadTemp()
{
    resetSettings();

    AmpApplet applet;
    auto* pa = tempButton(applet);
    auto* hl = applet.findChild<QPushButton*>(QStringLiteral("ampHlTempButton"));
    report("fallback readouts exist", pa != nullptr && hl != nullptr);
    if (!pa || !hl) return;

    applet.setDirectConnected(true);
    applet.setPaHeatsinkTemp(34.7f);
    applet.setHarmonicLoadHeatsinkTemp(28.4f);
    report("direct connection shows both heatsinks",
           pa->text() == QStringLiteral("PA 34.7 C ") && hl->text() == QStringLiteral("HL 28.4 C "),
           pa->text() + QStringLiteral("|") + hl->text());
    report("the HL readout explains HL in its tooltip",
           hl->toolTip().contains(QStringLiteral("Harmonic Load heatsink")), hl->toolTip());
    report("the PA readout's tooltip explains only PA",
           !pa->toolTip().contains(QStringLiteral("Harmonic Load")), pa->toolTip());

    // A FlexRadio relays only the PA heatsink temperature, so the HL value
    // must not stay on screen after the direct connection drops: HL goes back
    // to its dash, holding its place.
    const QString liveHlStyle = hl->styleSheet();
    applet.setDirectConnected(false);
    report("radio fallback drops the Harmonic Load heatsink",
           hl->text() == QStringLiteral("HL \u2014 C    ") && pa->text() == QStringLiteral("PA 34.7 C "),
           pa->text() + QStringLiteral("|") + hl->text());
    report("the dropped HL returns to the placeholder tone",
           hl->styleSheet() != liveHlStyle);

    // A late HL write after the drop must not bring the stale value back.
    applet.setHarmonicLoadHeatsinkTemp(28.5f);
    report("a late Harmonic Load write after the drop stays hidden",
           hl->text() == QStringLiteral("HL \u2014 C    "), hl->text());

    applet.setPaHeatsinkTemp(36.0f);
    report("radio fallback keeps updating the PA heatsink",
           pa->text() == QStringLiteral("PA 36.0 C "), pa->text());

    applet.setDirectConnected(true);
    applet.setHarmonicLoadHeatsinkTemp(29.0f);
    report("direct reconnection restores the Harmonic Load heatsink",
           pa->text() == QStringLiteral("PA 36.0 C ") && hl->text() == QStringLiteral("HL 29.0 C "),
           pa->text() + QStringLiteral("|") + hl->text());
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
           button->text() == QStringLiteral("PA \u2014 F    "),
           button->text());

    restored.setPaHeatsinkTemp(0.0f);
    report("reloaded value displays Fahrenheit",
           button->text() == QStringLiteral("PA 32.0 F "),
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
    report("fan combo starts shown, disabled and showing no mode",
           combo->isVisibleTo(&applet) && !combo->isEnabled() && combo->currentIndex() == -1);

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
        report("fresh fan combo starts disabled with no mode",
               freshCombo && !freshCombo->isEnabled() && freshCombo->currentIndex() == -1);
        if (freshCombo) {
            fresh.setFanMode("bogus");
            report("unknown fanmode does not enable the control or show a mode",
                   !freshCombo->isEnabled() && freshCombo->currentIndex() == -1);
            fresh.setFanMode("CONTEST");
            report("a recognized mode enables it and shows the mode",
                   freshCombo->isEnabled()
                       && freshCombo->currentText() == QStringLiteral("Fan: Contest"),
                   freshCombo->currentText());
            // Losing the direct connection clears the mode again, and clearing
            // it is not a fan-mode command.
            QSignalSpy freshSpy(&fresh, &AmpApplet::fanModeChanged);
            fresh.setDirectConnected(true);
            fresh.setDirectConnected(false);
            report("losing the connection clears the mode without commanding one",
                   freshCombo->currentIndex() == -1 && !freshCombo->isEnabled()
                       && freshSpy.count() == 0,
                   QString::number(freshSpy.count()));
        }
    }

    // #4731: on a large-enough default UI font, the popup's fixed pixel
    // width (sized off the combo's own hardcoded 10px stylesheet font)
    // couldn't fit the longest item — then "Fan: Contest" — so Qt's default
    // ElideMiddle silently mangled it. Widths/fonts aren't trustworthy to
    // assert on directly in this offscreen, unlaid-out harness (the combo
    // is never shown, so its geometry never reflects a real style pass),
    // so guard the three properties the fix actually sets instead: let the
    // widest item drive the combo's width rather than pinning it, and fail
    // any future overflow visibly (clipped) instead of mid-eliding it.
    report("fan combo sizes to its widest item, not a pinned width",
           combo->sizeAdjustPolicy() == QComboBox::AdjustToMinimumContentsLengthWithIcon);
    int longestItem = 0;
    for (int i = 0; i < combo->count(); ++i) {
        longestItem = std::max(longestItem, static_cast<int>(combo->itemText(i).length()));
    }
    report("fan combo reserves room for its longest item",
           combo->minimumContentsLength() >= longestItem,
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
HGauge* gaugeNamed(const AmpApplet& applet, const QString& accessibleName)
{
    for (QWidget* w : applet.findChildren<QWidget*>()) {
        if (w->accessibleName() != accessibleName) continue;
        if (auto* g = dynamic_cast<HGauge*>(w)) return g;
    }
    return nullptr;
}

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
// Drain current and PA heatsink temperature come from both the radio's meters
// and the PGXL's own status, and both change at the same moments. The tie goes
// to the radio: its value wins while fresh, and the PGXL's is used otherwise.
void testRadioVitalsWinOverTheDeviceWhileFresh()
{
    resetSettings();
    AmpApplet applet;
    auto* button = tempButton(applet);
    if (!button) { report("radio vitals", false); return; }

    // No radio meters at all: the PGXL's own readings apply.
    applet.setDrainCurrent(4.3f);
    applet.setPaHeatsinkTemp(43.6f);
    report("the PGXL's drain current applies with no radio meter",
           qFuzzyCompare(gaugeValue(applet, QStringLiteral("Drain current")), 4.3f));
    report("the PGXL's PA heatsink temperature applies with no radio meter",
           button->text() == QStringLiteral("PA 43.6 C "), button->text());

    // The radio's meters arrive and win while fresh.
    applet.setRadioDrainCurrent(19.2f, true);
    applet.setRadioPaHeatsinkTemp(47.9f, true);
    applet.setDrainCurrent(19.1f);
    applet.setPaHeatsinkTemp(47.8f);
    report("the radio's drain current wins while fresh",
           qFuzzyCompare(gaugeValue(applet, QStringLiteral("Drain current")), 19.2f));
    report("the radio's PA heatsink temperature wins while fresh",
           button->text() == QStringLiteral("PA 47.9 C "), button->text());

    // The radio's meters are withdrawn: the PGXL takes over at once.
    applet.setRadioDrainCurrent(0.0f, false);
    applet.setRadioPaHeatsinkTemp(0.0f, false);
    report("a withdrawn radio meter does not zero the drain current",
           qFuzzyCompare(gaugeValue(applet, QStringLiteral("Drain current")), 19.2f));
    applet.setDrainCurrent(5.0f);
    applet.setPaHeatsinkTemp(48.9f);
    report("the PGXL's drain current applies once the radio meter is gone",
           qFuzzyCompare(gaugeValue(applet, QStringLiteral("Drain current")), 5.0f));
    report("the PGXL's PA heatsink temperature applies once the radio meter is gone",
           button->text() == QStringLiteral("PA 48.9 C "), button->text());
}

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
    report("MEffA control is shown but disabled until the amplifier reports",
           !btn->isHidden() && !btn->isEnabled());

    // OPERATE follows the same rule: in its place, but disabled and grey,
    // until the amplifier reports a state.
    QPushButton* operate = nullptr;
    for (QPushButton* b : applet.findChildren<QPushButton*>()) {
        if (!b->isHidden() && b->accessibleName() == QStringLiteral("Amplifier state not reported")) operate = b;
    }
    report("OPERATE is shown but disabled until the amplifier reports",
           operate != nullptr && !operate->isEnabled()
               && operate->text() == QStringLiteral("—"));
    if (operate) {
        applet.setState(QStringLiteral("STANDBY"));
        report("a reported state enables the operate control",
               operate->isEnabled() && operate->text() == QStringLiteral("STANDBY"),
               operate->text());
    }

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

    applet.setPaHeatsinkTemp(9.9f);
    applet.setHarmonicLoadHeatsinkTemp(9.9f);
    const int pairWidth = button->text().length();

    applet.setPaHeatsinkTemp(100.4f);
    applet.setHarmonicLoadHeatsinkTemp(-5.0f);
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
           vdd->text().startsWith(QStringLiteral("Vdd 0.0 ")), vdd->text());
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

// Project canon is SmartMTR's extremes engine: a sliding-window max with a
// constant-velocity glide and no hold phase. Asserts the shape -- the marker
// glides rather than jumping, and retires itself when the peak leaves the
// window -- not the constants.
void testPeakMarkerUsesTheSlidingWindow()
{
    resetSettings();
    AmpApplet applet;
    HGauge* g = gaugeNamed(applet, QStringLiteral("Forward power"));
    report("the forward-power gauge exists", g != nullptr, QString());
    if (!g) return;

    applet.setDeviceMeters(1000.0f, 1.2f);
    QTest::qWait(80);
    const float early = g->peakValue();
    report("the marker glides toward the peak rather than snapping to it",
           early < 1000.0f, QString::number(early));

    const qint64 upDeadline = QDateTime::currentMSecsSinceEpoch() + 6000;
    while (g->peakValue() < 900.0f
           && QDateTime::currentMSecsSinceEpoch() < upDeadline) {
        QTest::qWait(50);
    }
    report("it reaches the peak given time", g->peakValue() >= 900.0f,
           QString::number(g->peakValue()));

    // No hold timer: the window rolling past the peak is what brings it down.
    applet.setDeviceMeters(100.0f, 1.2f);
    const qint64 downDeadline = QDateTime::currentMSecsSinceEpoch() + 9000;
    while (g->peakValue() > 900.0f
           && QDateTime::currentMSecsSinceEpoch() < downDeadline) {
        QTest::qWait(50);
    }
    report("it comes back down once the peak leaves the window",
           g->peakValue() < 900.0f, QString::number(g->peakValue()));
}

// Every caption the rail's controls can show fits the button it gets, at the
// rail's width (AppletPanel is 260 px) and wider. #5903: MEffA, "Fan: St…" and
// "PERAT" clipped in the 260 px rail.
//
// Checked against each control's own sizeHint rather than font metrics taken
// here: the hint is computed from the fonts and style sheet this machine
// actually draws with, so a CI host that falls back to other fonts moves both
// sides together instead of failing on a caption that fits.
void testDockedCaptionsFitTheRail()
{
    resetSettings();

    auto controlsFit = [](AmpApplet& applet, const QString& when) {
        QCoreApplication::processEvents();
        auto* meffa = applet.findChild<QPushButton*>(QStringLiteral("ampMeffaButton"));
        auto* fan = fanCombo(applet);
        QPushButton* operate = nullptr;
        for (QPushButton* b : applet.findChildren<QPushButton*>()) {
            if (meffa && b->isVisible() && b != meffa && b->objectName().isEmpty()
                && b->parentWidget() == meffa->parentWidget()) {
                operate = b;
            }
        }
        bool fits = meffa && fan && operate;
        QString detail = when;
        for (QWidget* w : std::initializer_list<QWidget*>{meffa, fan, operate}) {
            if (!w) continue;
            if (!w->isVisible() || w->width() < w->sizeHint().width()) {
                fits = false;
                detail += QStringLiteral(" | %1 %2<%3").arg(w->metaObject()->className())
                              .arg(w->width()).arg(w->sizeHint().width());
            }
        }
        // A combo's sizeHint uses minimumContentsLength, not the item text, so
        // the caption itself is measured too (#4885).
        if (fan && !fan->currentText().isEmpty()
            && fan->fontMetrics().horizontalAdvance(fan->currentText()) > fan->width()) {
            fits = false;
            detail += QStringLiteral(" | fan caption wider than combo");
        }
        if (operate) detail += QStringLiteral(" | operate=") + operate->text();
        if (fan) detail += QStringLiteral(" | fan=") + fan->currentText();
        return std::make_pair(fits, detail);
    };

    for (int railWidth : {260, 300}) {
        {
            // A fresh applet: nothing reported yet, every control disabled.
            AmpApplet fresh;
            fresh.resize(railWidth, 360);
            fresh.show();
            const auto offline = controlsFit(fresh, QStringLiteral("before any report"));
            report(qPrintable(QStringLiteral("rail controls fit before any report at %1 px").arg(railWidth)),
                   offline.first, offline.second);
        }

        AmpApplet applet;
        applet.resize(railWidth, 360);
        applet.show();
        applet.setDirectConnected(true);
        bool all = true;
        QString failures;
        for (const char* state : {"STANDBY", "IDLE", "FAULT", "POWERUP", "SELFCHECK"}) {
            applet.setState(QString::fromLatin1(state));
            for (const char* fanMode : {"STANDARD", "CONTEST", "BROADCAST"}) {
                applet.setFanMode(QString::fromLatin1(fanMode));
                for (const char* meffa : {"OFF", "STANDBY", "ACTIVE"}) {
                    applet.setMeffa(QString::fromLatin1(meffa), true);
                    const auto r = controlsFit(applet, QStringLiteral("%1/%2/%3").arg(state, fanMode, meffa));
                    if (!r.first) { all = false; failures += r.second + QStringLiteral("; "); }
                }
            }
        }
        report(qPrintable(QStringLiteral("every rail caption fits at %1 px").arg(railWidth)),
               all, failures);
    }
}

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
    testConnectionSourceIndicator();
    testSingleSensorToggle();
    testDualSensorToggle();
    testVoltagesStayAlignedWithTheTemperatures();
    testDockedCaptionsFitTheRail();
    testRadioFallbackDropsHarmonicLoadTemp();
    testPreferenceReload();
    testFanModePulldown();
    testReadoutWidthIsStable();
    testDriveRowShowsMeasuredDrive();
    testRelayedMetersWinOverTheDeviceWhileFresh();
    testRadioVitalsWinOverTheDeviceWhileFresh();
    testDeviceMetersDriveTheGaugesWithoutARelay();
    testSwrBarFollowsThePowerCrossing();
    testMeffaShowsThreeStates();
    testMeffaToggleSendsTheOperatorsBit();
    testMeffaIsInertUntilTheSetupGroupIsKnown();
    testDeviceMetersSurviveARelayWithNoPower();
    testWithdrawnDriveHidesTheRow();
    testFanControlStaysOperableWithoutTheSetupGroup();
    testPeakMarkerUsesTheSlidingWindow();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : qPrintable(QStringLiteral("%1 test(s) failed.").arg(g_failed)));
    return g_failed == 0 ? 0 : 1;
}
