#include "TestSettingsProfile.h"
#include "gui/BandApplet.h"
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
    QApplication app(argc, argv);

    testDefaultButtons();
    testBandSelectionSignals();
    testActiveBandHighlighting();
    testRadioCapabilitiesAndXvtr();
    testTuningRange();
    testDeclaredBands();
    testXvtrHighlightPrecedence();

    return g_failed == 0 ? 0 : 1;
}
