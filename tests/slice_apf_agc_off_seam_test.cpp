// APF and AGC-off level reach the backend seam. Socket-free: a recording
// IRadioBackend installed through RadioModel's production receiver bindings.
//   1. Flex's APF wire text from its typed adapter is unchanged.
//   2. setApf/setApfLevel emit receiveDspRequested with enable and level together.
//   3. RadioModel routes the APF to requestSliceDsp and the off level (also via the
//      AGC-T knob and the calibrator) to requestSliceAgc(Field::OffLevel).
//   4. A backend echo never comes back as a command (Principle II).
//   5. The base verbs emit nothing and the base drops OffLevel.
//   6. The calibrator's AGC-off sweep climbs from 0 and stops at its target.

#include "TestSettingsProfile.h"
#include "core/AgcTCalibrator.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/flex/FlexBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QMetaMethod>
#include <QMetaObject>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>
#include <cmath>
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

// Implements only what the seam requires; the APF and AGC requests fall
// through to IRadioBackend's defaults unless Recording overrides them.
class MinimalBackend : public IRadioBackend
{
public:
    RadioCapabilities caps;
    int pairedAgcCalls = 0;
    RadioCapabilities capabilities() const override { return caps; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    bool isConnected() const override { return true; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override { ++pairedAgcCalls; }
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
    ReceiveDispatch requestSliceDsp(int sliceId, const SliceDspRequest& request) override
    {
        if (!request.valid() || request.feature != SliceDspRequest::Feature::Apf) {
            return ReceiveDispatch::Unsupported;
        }
        apf.push_back({sliceId, request.enabled, request.level});
        return ReceiveDispatch::Dispatched;
    }
    void requestSliceAgc(int sliceId, const SliceAgcRequest& request) override
    {
        if (request.field == SliceAgcRequest::Field::OffLevel)
            offLevel.push_back({sliceId, request.offLevel});
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
    FlexBackend backend;
    QStringList sent;
    backend.setSliceCommandSink([&sent](const QString& cmd) { sent << cmd; });
    QObject::connect(&s, &SliceModel::receiveDspRequested, &s,
                     [&](const SliceDspRequest& request) { backend.requestSliceDsp(3, request); });
    QObject::connect(&s, &SliceModel::commandReady, &s,
                     [&sent](const QString& cmd) { sent << cmd; });
    s.setApf(true);
    s.setApfLevel(42);
    s.setApf(false);
    check(sent == QStringList{QStringLiteral("slice set 3 apf=1"),
                              QStringLiteral("slice set 3 apf_level=42"),
                              QStringLiteral("slice set 3 apf=0")},
          "apf= / apf_level= are exactly the Flex strings they were");
}

void testIntentsCarryBothHalves()
{
    std::printf("\n  2. Typed intents\n");
    SliceModel s(0);
    QSignalSpy apf(&s, &SliceModel::receiveDspRequested);
    s.setApfLevel(70);
    check(apf.size() == 1
              && qvariant_cast<SliceDspRequest>(apf.last().at(0)).feature == SliceDspRequest::Feature::Apf
              && qvariant_cast<SliceDspRequest>(apf.last().at(0)).field == SliceDspRequest::Field::Level
              && !qvariant_cast<SliceDspRequest>(apf.last().at(0)).enabled
              && qvariant_cast<SliceDspRequest>(apf.last().at(0)).level == 70,
          "a level change carries the current enable with it");
    s.setApf(true);
    check(apf.size() == 2
              && qvariant_cast<SliceDspRequest>(apf.last().at(0)).field == SliceDspRequest::Field::Enabled
              && qvariant_cast<SliceDspRequest>(apf.last().at(0)).enabled
              && qvariant_cast<SliceDspRequest>(apf.last().at(0)).level == 70,
          "an enable carries the current level with it");
    s.setApfLevel(70);
    check(apf.size() == 2, "an unchanged level is not re-issued");
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
          "the AGC-off level reaches requestSliceAgc(slice, OffLevel)");

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
    check(backend->offLevel.size() == 3 && backend->offLevel.back().level == 0,
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
    // A backend that overrides neither verb: the base must emit nothing (every
    // signal is spied) and must not turn an OffLevel request into a paired
    // mode/threshold write.
    MinimalBackend b;
    std::vector<std::unique_ptr<QSignalSpy>> spies;
    const QMetaObject* mo = b.metaObject();
    for (int i = 0; i < mo->methodCount(); ++i) {
        const QMetaMethod m = mo->method(i);
        if (m.methodType() == QMetaMethod::Signal)
            spies.push_back(std::make_unique<QSignalSpy>(&b, m));
    }
    b.setSliceApf(0, true, 50);
    b.requestSliceAgc(0, {SliceAgcRequest::Field::OffLevel, QStringLiteral("off"), 65, 40});
    int emitted = 0;
    for (const auto& spy : spies)
        emitted += static_cast<int>(spy->count());
    check(spies.size() > 1, "the backend's signals are all being watched");
    check(emitted == 0,
          "IRadioBackend's default setSliceApf / requestSliceAgc(OffLevel) emit nothing");
    check(b.pairedAgcCalls == 0, "the base drops OffLevel rather than calling setSliceAgc");
}

void testCalibratorClimbsToTheTarget()
{
    std::printf("\n  6. The calibrator's AGC-off sweep\n");
    // The off level is a fixed gain, and ANAN's 100 is 120 dB, past clipping on
    // band noise alone. Here the audio is -60 dB at 0 and rises 0.6 dB per
    // step, so the default -28 dB target is crossed at 53.
    SliceModel s(0);
    s.setAgcMode(QStringLiteral("off"));
    s.setAgcOffLevel(47);
    AgcTCalibrator cal;
    cal.setSlice(&s);
    cal.setSettleMs(1);
    std::vector<int> applied;
    QObject::connect(&s, &SliceModel::agcOffLevelChanged, &cal, [&](int level) {
        applied.push_back(level);
        const float rms = std::pow(10.0f, (-60.0f + 0.6f * static_cast<float>(level)) / 20.0f);
        for (int i = 0; i < 40; ++i)   // past the detector's smoothing
            cal.onAudioLevel(rms);
    });
    QSignalSpy finished(&cal, &AgcTCalibrator::finished);
    cal.startAutoSweep();
    check(finished.wait(5000), "the sweep finishes");
    check(!applied.empty() && applied.front() == 0,
          "with AGC Off the sweep starts at 0, not at 100");
    check(!applied.empty() && *std::max_element(applied.begin(), applied.end()) == 56,
          "and stops at the first step past the target");
    check(cal.recommendedValue() == 53, "the recommendation is where the audio crosses the target");
    cal.stop();

    // With AGC on the AGC holds the level, so the knee sweep still starts at 100.
    s.setAgcMode(QStringLiteral("med"));
    s.setAgcThreshold(47);
    cal.startAutoSweep();
    check(s.agcThreshold() == 100, "with AGC on the sweep starts at 100");
    cal.stop();
}

void testSweepStopsWithoutAudio()
{
    std::printf("\n  7. A sweep with no audio stops\n");
    // PC Audio off stops AudioEngine, so levelChanged never fires. The curve
    // was then flat at the last reading, the AGC-off pass climbed to 100
    // (120 dB on ANAN's line out) and the target solve applied 0.
    SliceModel s(0);
    s.setAgcMode(QStringLiteral("off"));
    s.setAgcOffLevel(47);
    AgcTCalibrator cal;
    cal.setSlice(&s);
    cal.setSettleMs(1);
    std::vector<int> applied;
    int audibleBelow = -1;   // levels arrive only for off levels under this
    QObject::connect(&s, &SliceModel::agcOffLevelChanged, &cal, [&](int level) {
        applied.push_back(level);
        if (level < audibleBelow) {
            for (int i = 0; i < 40; ++i)
                cal.onAudioLevel(0.001f);
        }
    });
    // As the dialog wires them: a value that settles outside a sweep is
    // recorded as a manual point, 120 ms later.
    QObject::connect(&s, &SliceModel::agcOffLevelChanged, &cal, &AgcTCalibrator::onValueChanged);
    QObject::connect(&s, &SliceModel::agcThresholdChanged, &cal, &AgcTCalibrator::onValueChanged);
    const auto nothingRecordedOnceSettled = [&] {
        QTest::qWait(400);
        return cal.curve().isEmpty() && !cal.hasRecommendation();
    };
    QSignalSpy finished(&cal, &AgcTCalibrator::finished);
    QSignalSpy noAudio(&cal, &AgcTCalibrator::noAudio);

    cal.startAutoSweep();
    check(finished.wait(5000), "with no audio at all the sweep ends");
    check(noAudio.size() == 1 && !finished.constFirst().constFirst().toBool(),
          "and says it stopped for no audio");
    check(applied == std::vector<int>{0, 47},
          "at its first step, before climbing, putting the off level back");
    check(!cal.isRunning() && !cal.hasRecommendation(), "with nothing to recommend");
    check(nothingRecordedOnceSettled(), "and the put-back value is not recorded as a point");

    // Audio that stops part-way: levels for 0, 4 and 8, none from 12.
    applied.clear();
    finished.clear();
    noAudio.clear();
    audibleBelow = 12;
    cal.startAutoSweep();
    check(finished.wait(5000) && noAudio.size() == 1, "audio that stops mid-sweep stops it");
    check(applied == std::vector<int>{0, 4, 8, 12, 47},
          "at the first silent step, putting the off level back");
    check(!cal.hasRecommendation(), "with no recommendation from a part curve");
    check(nothingRecordedOnceSettled(),
          "nor once the put-back value settles: the part curve is dropped");

    // Audio from before the sweep, none during it: the smoothed reading is
    // stale, not absent. The knee sweep (AGC on) stops the same way.
    s.setAgcMode(QStringLiteral("med"));
    s.setAgcThreshold(47);
    std::vector<int> thresholds;
    QObject::connect(&s, &SliceModel::agcThresholdChanged, &cal,
                     [&](int value) { thresholds.push_back(value); });
    cal.onAudioLevel(0.2f);
    finished.clear();
    noAudio.clear();
    cal.startAutoSweep();
    check(finished.wait(5000) && noAudio.size() == 1,
          "a level from before the sweep does not count for its first step");
    check(thresholds == std::vector<int>{100, 47},
          "so the knee sweep stops at 100 and puts AGC-T back");
    check(nothingRecordedOnceSettled(), "recording nothing once AGC-T settles");

    // The stop has already said finished. A session change that leaves the
    // calibrator unable to write cancels it, and must not say so again: the
    // dialog would replace the no-audio line with "Calibration stopped."
    finished.clear();
    s.invalidateFrequencyObservation();
    check(finished.isEmpty(), "and a later session change does not end it a second time");
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
    testCalibratorClimbsToTheTarget();
    testSweepStopsWithoutAudio();
    std::printf("\n  %s — %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
