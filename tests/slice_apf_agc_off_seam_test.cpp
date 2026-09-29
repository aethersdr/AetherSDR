// APF and AGC-off level reach the backend seam; Flex keeps its wire text.
//
// SliceModel::setApf/setApfLevel/setAgcOffLevel used to emit FlexRadio wire
// text and nothing else. A non-Flex slice never has commandReady connected,
// so on every host-DSP radio the APF button, its level slider, the
// ToggleApf/WheelApf controller actions, the AGC-T slider with AGC Off and the
// AGC-T calibrator's AGC-off strategy moved the model and reached nothing.
//
// Socket-free: a recording IRadioBackend installed through the production
// receiver bindings (RadioModel::setBackendForTest), no DSP, no wire.
//
// Pinned:
//   1. The Flex wire text is byte-for-byte what it was.
//   2. Operator setters emit the typed intents, enable and level together.
//   3. RadioModel routes them to setSliceApf / setSliceAgcOffLevel for the
//      right slice, including through the AGC-T knob and the calibrator's
//      own setter.
//   4. Status application (a backend's echo) never comes back as a command
//      (Principle II), and an echo at an unchanged off level emits nothing.
//   5. The base IRadioBackend verbs are no-ops, so every other family is
//      unaffected.

#include "TestSettingsProfile.h"
#include "core/AgcTCalibrator.h"
#include "core/backends/IRadioBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

using namespace AetherSDR;

namespace {

int failures = 0;
void check(bool condition, const char* description)
{
    std::printf("  [%s] %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition)
        ++failures;
}

// Implements only what the seam requires; APF and the off level fall through
// to IRadioBackend's defaults unless Recording overrides them.
class MinimalBackend : public IRadioBackend
{
public:
    RadioCapabilities caps;
    RadioCapabilities capabilities() const override { return caps; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    bool isConnected() const override { return true; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}   // cannot transmit
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

class RecordingBackend : public MinimalBackend
{
public:
    struct Apf { int slice; bool on; int level; };
    struct OffLevel { int slice; int level; };
    std::vector<Apf> apf;
    std::vector<OffLevel> offLevel;
    void setSliceApf(int sliceId, bool on, int level) override
    {
        apf.push_back({sliceId, on, level});
    }
    void setSliceAgcOffLevel(int sliceId, int level) override
    {
        offLevel.push_back({sliceId, level});
    }
};

SliceDelta fullDelta(const QString& pan)
{
    SliceDelta d;
    d.panId = pan;
    d.frequency = 7.030;
    d.mode = QStringLiteral("CW");
    d.filterLow = -250;
    d.filterHigh = 250;
    d.inUse = true;
    return d;
}

void testFlexWireTextUnchanged()
{
    std::printf("\n  1. Flex wire text\n");
    SliceModel s(3);
    QStringList sent;
    QObject::connect(&s, &SliceModel::commandReady, &s,
                     [&sent](const QString& cmd) { sent << cmd; });
    s.setApf(true);
    s.setApfLevel(42);
    s.setAgcOffLevel(31);
    s.setApf(false);
    check(sent == QStringList{QStringLiteral("slice set 3 apf=1"),
                              QStringLiteral("slice set 3 apf_level=42"),
                              QStringLiteral("slice set 3 agc_off_level=31"),
                              QStringLiteral("slice set 3 apf=0")},
          "apf= / apf_level= / agc_off_level= are exactly the Flex strings they were");
}

void testIntentsCarryBothHalves()
{
    std::printf("\n  2. Typed intents\n");
    SliceModel s(0);
    QSignalSpy apf(&s, &SliceModel::apfCommandIssued);
    QSignalSpy off(&s, &SliceModel::agcOffLevelCommandIssued);
    s.setApfLevel(70);
    check(apf.size() == 1 && apf.last().at(0).toBool() == false
              && apf.last().at(1).toInt() == 70,
          "a level change carries the current enable with it");
    s.setApf(true);
    check(apf.size() == 2 && apf.last().at(0).toBool() && apf.last().at(1).toInt() == 70,
          "an enable carries the current level with it");
    s.setApfLevel(70);
    check(apf.size() == 2, "an unchanged level is not re-issued");
    s.setAgcOffLevel(25);
    check(off.size() == 1 && off.last().at(0).toInt() == 25, "off level is issued");
    s.setAgcOffLevel(25);
    check(off.size() == 1, "an unchanged off level is not re-issued");
    s.setAgcOffLevel(250);
    check(off.size() == 2 && off.last().at(0).toInt() == 100,
          "the off level is clamped to 0..100 before it is issued");
}

void testRoutedThroughTheSeam()
{
    std::printf("\n  3. RadioModel routes them to the backend\n");
    RadioModel radio;
    auto owned = std::make_unique<RecordingBackend>();
    RecordingBackend* backend = owned.get();
    backend->caps.maxSlices = 2;
    backend->caps.hasAudioPeakingFilter = true;
    radio.setBackendForTest(std::move(owned), QStringLiteral("hl2"));
    const QString opaquePan = QStringLiteral("hl2:ddc0");
    emit backend->panCenterBandwidthChanged(opaquePan, 7.030, 0.048);
    emit backend->sliceChanged(1, fullDelta(opaquePan));
    SliceModel* s = radio.slice(1);
    check(s != nullptr, "the backend's slice exists in the model");
    if (!s)
        return;

    s->setApf(true);
    check(backend->apf.size() == 1 && backend->apf.back().slice == 1
              && backend->apf.back().on && backend->apf.back().level == 50,
          "the APF button reaches setSliceApf(slice, on, level)");
    s->setApfLevel(80);
    check(backend->apf.size() == 2 && backend->apf.back().on
              && backend->apf.back().level == 80,
          "the APF level slider reaches setSliceApf with the enable held");
    // ToggleApf is `s->setApf(!s->apfOn())` in MainWindow_Controllers.cpp;
    // WheelApf ends in setApfLevel. Both are the setters exercised here.
    s->setApf(!s->apfOn());
    check(backend->apf.size() == 3 && !backend->apf.back().on
              && backend->apf.back().level == 80,
          "a ToggleApf-shaped toggle reaches the backend");

    s->setAgcOffLevel(33);
    check(backend->offLevel.size() == 1 && backend->offLevel.back().slice == 1
              && backend->offLevel.back().level == 33,
          "the AGC-off level reaches setSliceAgcOffLevel(slice, level)");

    // The AGC-T knob with AGC Off, the controller surfaces' one entry point.
    s->setAgcMode(QStringLiteral("off"));
    check(s->agcTKnobUsesOffLevel(), "AGC Off puts the AGC-T knob on the off level");
    s->setAgcTKnobLevel(47);
    check(backend->offLevel.size() == 2 && backend->offLevel.back().level == 47,
          "the AGC-T knob with AGC Off reaches the backend");

    // The calibrator's AGC-off strategy writes through the same setter.
    AgcTCalibrator cal;
    cal.setSlice(s);
    check(cal.strategy() == AgcTCalibrator::Strategy::TargetLevel,
          "with AGC Off the calibrator runs its target-level strategy");
    cal.startAutoSweep();   // first sweep point is written synchronously
    check(backend->offLevel.size() == 3 && backend->offLevel.back().level == 100,
          "the calibrator's AGC-off sweep reaches the backend");
    cal.stop();             // restores what it found
    check(backend->offLevel.size() == 4 && backend->offLevel.back().level == 47,
          "and so does its restore on abort");

    std::printf("\n  4. Echoes are not commands\n");
    const std::size_t apfBefore = backend->apf.size();
    const std::size_t offBefore = backend->offLevel.size();
    QSignalSpy offChanged(s, &SliceModel::agcOffLevelChanged);
    SliceDelta echo;
    echo.apf = true;
    echo.apfLevel = 20;
    echo.agcOffLevel = 64;
    emit backend->sliceChanged(1, echo);
    check(s->apfOn() && s->apfLevel() == 20 && s->agcOffLevel() == 64,
          "a backend echo updates the model");
    check(backend->apf.size() == apfBefore && backend->offLevel.size() == offBefore,
          "and never comes back as a command (Principle II)");
    check(offChanged.size() == 1, "the changed off level notifies once");
    emit backend->sliceChanged(1, echo);
    check(offChanged.size() == 1,
          "an echo at the same off level notifies nothing — the calibrator is "
          "wired to this signal and would re-record a point on every VFO step");
}

void testBaseVerbsAreNoOps()
{
    std::printf("\n  5. Other families\n");
    // A backend that does not override the verbs must accept them silently —
    // the base defaults are what every family except HL2 now inherits.
    MinimalBackend b;
    b.setSliceApf(0, true, 50);
    b.setSliceAgcOffLevel(0, 40);
    check(true, "IRadioBackend's default setSliceApf / setSliceAgcOffLevel are no-ops");
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("slice-apf-agc-off-seam"));
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "settings are isolated");
    testFlexWireTextUnchanged();
    testIntentsCarryBothHalves();
    testRoutedThroughTheSeam();
    testBaseVerbsAreNoOps();
    std::printf("\n  %s — %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
