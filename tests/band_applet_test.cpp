#include "TestSettingsProfile.h"
#include "gui/BandApplet.h"
#include "models/BandPlanManager.h"
#include "models/SliceModel.h"
#include "models/ModelCapabilities.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QPushButton>
#include <QSignalSpy>

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

QPushButton* findButton(const BandApplet& applet, const QString& text)
{
    const auto buttons = applet.findChildren<QPushButton*>();
    for (auto* btn : buttons) {
        if (btn->text() == text) {
            return btn;
        }
    }
    return nullptr;
}

// rebuildGrid() deleteLater()s the buttons of the layout it replaces, and a
// deferred delete only runs when the event loop gets control back — which it
// never does inside a test. Purge them so findChildren() cannot see buttons
// from a superseded layout.
void settleRebuild()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

BandPlanManager::Segment segment(double lowMhz, double highMhz)
{
    BandPlanManager::Segment seg;
    seg.lowMhz = lowMhz;
    seg.highMhz = highMhz;
    return seg;
}

// A US-style plan: 40 m runs to 7.300 MHz, 20 m to 14.350 MHz.
QVector<BandPlanManager::Segment> usStylePlan()
{
    return {segment(7.000, 7.300), segment(14.000, 14.350)};
}

void testDefaultButtons()
{
    BandApplet applet;

    QStringList expectedButtons = {
        "160", "80", "60", "40", "30", "20", "17", "15", "12", "10", "6",
        "WWV", "GEN", "2200", "630", "XVTR"
    };

    bool allFound = true;
    for (const auto& name : expectedButtons) {
        if (!findButton(applet, name)) {
            allFound = false;
            report("default button found", false, QString("Missing button: %1").arg(name));
        }
    }
    if (allFound) {
        report("all default band buttons present", true);
    }
}

void testBandSelectionSignals()
{
    BandApplet applet;

    QSignalSpy bandSpy(&applet, &BandApplet::bandSelected);
    QSignalSpy xvtrSpy(&applet, &BandApplet::xvtrSetupRequested);

    auto* btn20 = findButton(applet, "20");
    report("20m button exists", btn20 != nullptr);
    if (btn20) {
        btn20->click();
        report("20m bandSelected emitted", bandSpy.count() == 1);
        if (bandSpy.count() == 1) {
            const auto args = bandSpy.takeFirst();
            report("20m bandName correct", args.at(0).toString() == "20m");
            report("20m freq correct", qFuzzyCompare(args.at(1).toDouble(), 14.225));
            report("20m mode correct", args.at(2).toString() == "USB");
        }
    }

    auto* btnXvtr = findButton(applet, "XVTR");
    report("XVTR button exists", btnXvtr != nullptr);
    if (btnXvtr) {
        btnXvtr->click();
        report("xvtrSetupRequested emitted", xvtrSpy.count() == 1);
    }
}

void testActiveBandHighlighting()
{
    BandApplet applet;
    SliceModel slice(0);
    BandPlanManager plan;
    plan.setSegmentsForTest(usStylePlan());

    applet.setBandPlanManager(&plan);
    applet.setSlice(&slice);

    // Set frequency to 20m (14.074 MHz)
    slice.setFrequency(14.074);
    auto* btn20 = findButton(applet, "20");
    auto* btn40 = findButton(applet, "40");
    report("20m button active on 14.074 MHz", btn20 && btn20->isChecked());
    report("40m button inactive on 14.074 MHz", btn40 && !btn40->isChecked());

    // Switch frequency to 40m (7.074 MHz)
    slice.setFrequency(7.074);
    report("40m button active on 7.074 MHz", btn40 && btn40->isChecked());
    report("20m button inactive on 7.074 MHz", btn20 && !btn20->isChecked());

    // Switch frequency to general coverage (15.000 MHz)
    slice.setFrequency(15.000);
    auto* btnGen = findButton(applet, "GEN");
    report("GEN button active on 15.000 MHz", btnGen && btnGen->isChecked());

    // Disconnect slice
    applet.setSlice(nullptr);
    report("all buttons inactive when slice is null", btnGen && !btnGen->isChecked());
}

// The highlight follows the active band plan, exactly as the overlay's Band
// panel does: a frequency outside every segment of the plan is GEN, even when
// it sits inside the band's broad grouping window (#5234 review nit 1).
void testHighlightFollowsBandPlan()
{
    BandApplet applet;
    SliceModel slice(0);
    BandPlanManager plan;
    plan.setSegmentsForTest(usStylePlan());
    applet.setBandPlanManager(&plan);
    applet.setSlice(&slice);

    auto* btn40 = findButton(applet, "40");
    auto* btnGen = findButton(applet, "GEN");

    slice.setFrequency(7.250);
    report("40 lit at 7.250 MHz under a US-style plan", btn40 && btn40->isChecked());

    // Region 1 style: 40 m ends at 7.200 MHz. planChanged re-runs the
    // highlight without the slice moving.
    plan.setSegmentsForTest({segment(7.000, 7.200), segment(14.000, 14.350)});
    emit plan.planChanged();
    report("GEN lit at 7.250 MHz once the plan ends 40 m at 7.200",
           btnGen && btnGen->isChecked());
    report("40 not lit at 7.250 MHz outside the plan", btn40 && !btn40->isChecked());

    slice.setFrequency(7.100);
    report("40 lit at 7.100 MHz inside the plan", btn40 && btn40->isChecked());

    // No plan at all reads as GEN, as the overlay's lookup does.
    applet.setBandPlanManager(nullptr);
    report("GEN lit with no band plan", btnGen && btnGen->isChecked());
}

// Every grid button carries a screen-reader name (docs/a11y.md, #5234 review
// nit 3).
void testAccessibleNames()
{
    BandApplet applet;
    auto* btn20 = findButton(applet, "20");
    auto* btnGen = findButton(applet, "GEN");
    auto* btnXvtr = findButton(applet, "XVTR");
    report("20 button is named \"20m band\"",
           btn20 && btn20->accessibleName() == QStringLiteral("20m band"),
           btn20 ? btn20->accessibleName() : QString());
    report("GEN button is named \"GEN band\"",
           btnGen && btnGen->accessibleName() == QStringLiteral("GEN band"));
    report("XVTR entry is named \"Transverter setup\"",
           btnXvtr && btnXvtr->accessibleName() == QStringLiteral("Transverter setup"));

    bool allNamed = true;
    for (auto* btn : applet.findChildren<QPushButton*>())
        allNamed = allNamed && !btn->accessibleName().isEmpty();
    report("every band button has an accessible name", allNamed);
}

void testRadioCapabilitiesAndXvtr()
{
    BandApplet applet;

    ModelCapabilities caps;
    caps.has2Meters = true;
    caps.has4Meters = true;
    applet.setRadioCapabilities(caps);

    auto* btn2 = findButton(applet, "2");
    auto* btn4 = findButton(applet, "4");
    report("2m button present when capability enabled", btn2 != nullptr);
    report("4m button present when capability enabled", btn4 != nullptr);

    // Custom XVTR
    QVector<BandApplet::XvtrBand> xvtrs;
    xvtrs.append({"222", 222.1, "X0"});
    applet.setXvtrBands(xvtrs);

    auto* btn222 = findButton(applet, "222");
    report("custom XVTR 222 button present", btn222 != nullptr);
    if (btn222) {
        QSignalSpy bandSpy(&applet, &BandApplet::bandSelected);
        btn222->click();
        report("custom XVTR bandSelected emitted", bandSpy.count() == 1);
        if (bandSpy.count() == 1) {
            const auto args = bandSpy.takeFirst();
            report("custom XVTR stackKey correct", args.at(3).toString() == "X0");
        }
    }
}

void testTuningRange()
{
    BandApplet applet;
    // Restrict tuning to 1.8 - 30.0 MHz
    applet.setTuningRange(1.8, 30.0);

    auto* btn20 = findButton(applet, "20");
    auto* btn6 = findButton(applet, "6");
    auto* btnXvtr = findButton(applet, "XVTR");

    report("20m button enabled within tuning range", btn20 && btn20->isEnabled());
    report("6m button disabled outside tuning range", btn6 && !btn6->isEnabled());
    report("XVTR setup button remains enabled", btnXvtr && btnXvtr->isEnabled());

    // Disabled buttons say why, in the overlay's words (#5234 review nit 2).
    const QString expectedTip =
        QStringLiteral("Outside this radio's tuning range (1.800–30.000 MHz)");
    report("out-of-range button explains itself",
           btn6 && btn6->toolTip() == expectedTip,
           btn6 ? btn6->toolTip() : QString());
    report("out-of-range reason reaches screen readers",
           btn6 && btn6->accessibleDescription() == expectedTip);
    report("in-range button has no tooltip", btn20 && btn20->toolTip().isEmpty());

    // Clearing the range re-enables everything and drops the explanation.
    applet.setTuningRange(0.0, 0.0);
    report("6m re-enabled once no range is reported", btn6 && btn6->isEnabled());
    report("6m tooltip cleared once no range is reported",
           btn6 && btn6->toolTip().isEmpty());
}

void testDeclaredBands()
{
    BandApplet applet;

    const QVector<DeclaredBandRange> ranges = {
        {QStringLiteral("20m"), 14.0e6, 14.35e6},
        {QStringLiteral("2m"), 144.0e6, 148.0e6},
    };
    // Ranges give the buttons their hardware-coverage labels ("14", "144").
    applet.setDeclaredBands({"20m", "2m"}, ranges);

    // A configured XVTR reusing a declared name must not add a row: a declared
    // set owns its band surface (DeclaredBandMenuPolicy, as the overlay does).
    QVector<BandApplet::XvtrBand> xvtrs;
    xvtrs.append({"2m", 144.1, "X0"});
    applet.setXvtrBands(xvtrs);
    settleRebuild();

    report("declared 20m button present", findButton(applet, "14") != nullptr);
    report("declared 2m button present", findButton(applet, "144") != nullptr);
    report("declared set hides configured XVTR rows", findButton(applet, "2m") == nullptr);
    report("declared set hides the XVTR setup entry", findButton(applet, "XVTR") == nullptr);
    report("declared set retains reachable utilities",
           findButton(applet, "WWV") != nullptr);

    // No stack-key hint: MainWindow::selectBand() must run
    // XvtrPolicy::resolveBandStackKey() first (#4191).
    auto* btn20 = findButton(applet, "14");
    if (btn20) {
        QSignalSpy bandSpy(&applet, &BandApplet::bandSelected);
        btn20->click();
        report("declared bandSelected emitted", bandSpy.count() == 1);
        if (bandSpy.count() == 1) {
            const auto args = bandSpy.takeFirst();
            report("declared band name passed through", args.at(0).toString() == "20m");
            report("declared band carries no stack-key hint", args.at(3).toString().isEmpty());
        }
    }

    // Utility rows follow the reported tuning range for a declared set.
    BandApplet ranged;
    ranged.setTuningRange(1.8, 148.0);
    ranged.setDeclaredBands({"20m", "2m"}, ranges);
    settleRebuild();
    report("declared utility inside tuning range kept",
           findButton(ranged, "WWV") != nullptr);
    report("declared utility outside tuning range dropped",
           findButton(ranged, "2200") == nullptr);

    // MainWindow pushes the declared set first and the range after it, so a
    // range arriving later must rebuild the presence-gated utility rows.
    BandApplet late;
    late.setDeclaredBands({"20m", "2m"}, ranges);
    settleRebuild();
    report("declared utility shown before any range is reported",
           findButton(late, "2200") != nullptr);
    late.setTuningRange(1.8, 148.0);
    settleRebuild();
    report("declared utility dropped by a range reported afterwards",
           findButton(late, "2200") == nullptr);
    report("declared utility in range kept after a late range",
           findButton(late, "WWV") != nullptr);
}

void testXvtrHighlightPrecedence()
{
    BandApplet applet;
    SliceModel slice(0);

    // A declared range and a configured XVTR overlap at 50.1 MHz, with
    // different names: the XVTR wins, matching MainWindow::selectBand()'s
    // resolution order (#5236) and the overlay's highlight.
    const QVector<DeclaredBandRange> ranges = {
        {QStringLiteral("6m"), 50.0e6, 54.0e6},
    };
    applet.setDeclaredBands({}, ranges);

    QVector<BandApplet::XvtrBand> xvtrs;
    xvtrs.append({"222", 50.1, "X0"});
    applet.setXvtrBands(xvtrs);
    settleRebuild();

    applet.setSlice(&slice);
    slice.setFrequency(50.1);

    auto* btn222 = findButton(applet, "222");
    auto* btn6 = findButton(applet, "6");
    report("XVTR wins the overlap over a declared range", btn222 && btn222->isChecked());
    report("declared range does not win the overlap", btn6 && !btn6->isChecked());
}

} // namespace

int main(int argc, char* argv[])
{
    TestSettingsProfile profile(QStringLiteral("band_applet_test"));
    QApplication app(argc, argv);

    testDefaultButtons();
    testBandSelectionSignals();
    testActiveBandHighlighting();
    testHighlightFollowsBandPlan();
    testAccessibleNames();
    testRadioCapabilitiesAndXvtr();
    testTuningRange();
    testDeclaredBands();
    testXvtrHighlightPrecedence();

    return g_failed == 0 ? 0 : 1;
}
