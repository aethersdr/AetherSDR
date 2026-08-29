#include "gui/SpectrumOverlayMenu.h"
#include "models/BandPlanManager.h"
#include "models/SliceModel.h"

#include <QApplication>
#include <QPushButton>
#include <QWidget>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

QPushButton* findBandButton(QWidget& parent, const QString& text)
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    const auto buttons = parent.findChildren<QPushButton*>();
    for (auto* btn : buttons) {
        if (btn->text() == text) {
            return btn;
        }
    }
    return nullptr;
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    QWidget parent;
    SpectrumOverlayMenu menu(&parent);
    SliceModel slice(0);
    BandPlanManager bandPlan;
    bandPlan.setSegmentsForTest({
        {3.500, 3.800, "80m", "", QColor()},
        {7.000, 7.200, "40m", "", QColor()},
        {14.000, 14.350, "20m", "", QColor()},
        {144.000, 148.000, "2m", "", QColor()},
    });
    menu.setBandPlanManager(&bandPlan);

    auto* btn20 = findBandButton(parent, "20");
    auto* btn40 = findBandButton(parent, "40");
    auto* btn80 = findBandButton(parent, "80");
    auto* btnWwv = findBandButton(parent, "WWV");
    auto* btnGen = findBandButton(parent, "GEN");

    report("Band buttons present in overlay menu",
           btn20 && btn40 && btn80 && btnWwv && btnGen);

    // Initial state without slice
    menu.setSlice(nullptr);
    report("No button checked when slice is null",
           btn20 && !btn20->isChecked() && btn40 && !btn40->isChecked() &&
           btnGen && !btnGen->isChecked() && btnWwv && !btnWwv->isChecked());

    // Tune slice to 20m (14.225 MHz) and attach
    slice.setFrequency(14.225);
    menu.setSlice(&slice);
    report("20m button highlighted when slice frequency is 14.225 MHz",
           btn20 && btn20->isChecked() && btn40 && !btn40->isChecked() &&
           btnGen && !btnGen->isChecked() && btnWwv && !btnWwv->isChecked());

    // Frequency changes to 40m (7.150 MHz)
    slice.setFrequency(7.150);
    report("40m button highlighted when frequency changes to 7.150 MHz",
           btn40 && btn40->isChecked() && btn20 && !btn20->isChecked() &&
           btnGen && !btnGen->isChecked());

    // 7.250 MHz is inside the legacy US BandDefs range but outside the active
    // Region 1 plan supplied above, so the overlay must not highlight 40m.
    slice.setFrequency(7.250);
    report("GEN highlighted outside active regional 40m allocation",
           btnGen && btnGen->isChecked() && btn40 && !btn40->isChecked());

    // Frequency changes to general coverage (15.000 MHz)
    slice.setFrequency(15.000);
    report("GEN button highlighted on 15.000 MHz",
           btnGen && btnGen->isChecked() && btn20 && !btn20->isChecked() &&
           btn40 && !btn40->isChecked());

    // Frequency changes to 80m (3.800 MHz)
    slice.setFrequency(3.800);
    report("80m button highlighted on 3.800 MHz",
           btn80 && btn80->isChecked() && btnGen && !btnGen->isChecked());

    // Test with XVTR bands
    QVector<SpectrumOverlayMenu::XvtrBand> xvtrs = {
        {"222", 222.100, "X1"},
        {"1296", 1296.100, "X2"}
    };
    menu.setXvtrBands(xvtrs);
    app.processEvents();

    auto* btn222 = findBandButton(parent, "222");
    auto* btn1296 = findBandButton(parent, "1296");
    btn20 = findBandButton(parent, "20");
    btnGen = findBandButton(parent, "GEN");

    report("XVTR buttons created after setXvtrBands",
           btn222 != nullptr && btn1296 != nullptr);

    slice.setFrequency(222.100);
    report("XVTR 222 button highlighted when tuned to 222.100 MHz",
           btn222 && btn222->isChecked() && btn1296 && !btn1296->isChecked());

    slice.setFrequency(14.074);
    report("20m button highlighted after XVTR rebuild",
           btn20 && btn20->isChecked() && btn222 && !btn222->isChecked());

    // Test with radio capability for 2m (native 2m button labelled "2") and XVTR overlap (finding 3 / finding 4)
    ModelCapabilities caps;
    caps.has2Meters = true;
    menu.setRadioCapabilities(caps);
    QVector<SpectrumOverlayMenu::XvtrBand> xvtrsWithOverlap = {
        {"2m-Sat", 144.200, "X1"}
    };
    menu.setXvtrBands(xvtrsWithOverlap);
    app.processEvents();

    auto* btn2 = findBandButton(parent, "2");
    auto* btn2mSat = findBandButton(parent, "2m-Sat");
    report("Native 2m button '2' and XVTR button '2m-Sat' created",
           btn2 != nullptr && btn2mSat != nullptr);

    // Tune within the XVTR window: XVTR must win over native/declared 2m band (finding 3)
    slice.setFrequency(144.200);
    report("XVTR button takes precedence over 2m button when overlapping",
           btn2mSat && btn2mSat->isChecked() && btn2 && !btn2->isChecked());

    // Tune within 2m band but outside XVTR ±500 kHz window: 2m button highlighted
    slice.setFrequency(146.000);
    report("2m button highlighted when outside XVTR window",
           btn2 && btn2->isChecked() && btn2mSat && !btn2mSat->isChecked());

    // setRadioCapabilities()/setXvtrBands() rebuild the panel, so reacquire
    // every raw button pointer used below after deferred deletes are drained.
    btn20 = findBandButton(parent, "20");
    btnGen = findBandButton(parent, "GEN");
    btn2 = findBandButton(parent, "2");
    btn2mSat = findBandButton(parent, "2m-Sat");

    // Detach slice
    menu.setSlice(nullptr);
    report("All buttons unhighlighted after detaching slice",
           btn20 && !btn20->isChecked() && btn2 && !btn2->isChecked() &&
           btn2mSat && !btn2mSat->isChecked() && btnGen && !btnGen->isChecked());

    return g_failed == 0 ? 0 : 1;
}
