// Socket-free: the client-owned state RadioModel itself keeps for a radio with
// no command plane -- the output level the master controls read back, and the
// tuning step it restores into new slices and captures from them (#6129).
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/RadioStateMemory.h"
#include "core/backends/anan/AnanBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>

#include <cstdio>
#include <memory>

namespace AetherSDR {
class RadioModelSliceLifecycleTestAccess {
public:
    static void setSerial(RadioModel& radio, const QString& serial)
    {
        radio.m_lastInfo.serial = serial;
    }
    static void restore(RadioModel& radio) { radio.handRestoredStateToBackend(); }
    static void persist(RadioModel& radio) { radio.persistOperatingState(true); }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) {
        ++failures;
    }
}

SliceDelta newSlice()
{
    SliceDelta delta;
    delta.frequency = 14.175;
    delta.mode = QStringLiteral("USB");
    return delta;
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("radio_model_client_state_test"));
    if (!profile.isValid()) {
        std::printf("[FAIL] create temporary home\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    using Access = RadioModelSliceLifecycleTestAccess;
    auto& settings = AppSettings::instance();
    settings.load();

    // ---- the level the operator hears ----
    // No backend, so nothing declares ReceiveOutputLevel: the case of a Flex,
    // the demo, an HL2, an Icom and an RTL.
    {
        RadioModel radio;
        settings.setValue(QStringLiteral("MasterVolume"), QStringLiteral("60"));
        settings.setValue(QStringLiteral("PcAudioEnabled"), QStringLiteral("False"));
        radio.setLineoutGain(30);
        check(radio.activeOutputVolumePercent() == 30,
              "PC Audio off: the radio's line-out level, with no capability declared");
        settings.setValue(QStringLiteral("PcAudioEnabled"), QStringLiteral("True"));
        check(radio.activeOutputVolumePercent() == 60,
              "PC Audio on: the PC sink's MasterVolume");
    }

    // ---- an ANAN's restored level and step ----
    const RadioCapabilities caps = anan::AnanBackend().capabilities();
    const RadioSettingsScope radioA(QStringLiteral("anan"), QStringLiteral("ANAN-A"));
    {
        RestoredRadioState stored;
        stored.receiveOutputLevelPct = 35;
        stored.tuningStepHz = 2'500;
        check(RadioStateMemory::store(radioA, caps, stored), "radio A's document is seeded");
    }

    RadioModel radio;
    radio.setBackendForTest(std::make_unique<anan::AnanBackend>(), QStringLiteral("anan"));
    Access::setSerial(radio, QStringLiteral("ANAN-A"));
    Access::restore(radio);
    check(radio.lineoutGain() == 35,
          "the restored level reaches the model the slider reads with PC Audio off");

    radio.emitBackendSliceChangedForTest(0, newSlice());
    const SliceModel* first = radio.slice(0);
    check(first && first->stepHz() == 2'500,
          "a new slice starts at the restored step, not the 100 Hz default");

    check(radio.applyClientOwnedSliceStep(0, 5'000),
          "with no command plane the step is the client's to apply");
    Access::persist(radio);
    check(RadioStateMemory::load(radioA, caps).tuningStepHz == 5'000,
          "a changed step is captured into radio A's document");

    // A same-model swap to a radio with no document must not inherit A's step.
    Access::setSerial(radio, QStringLiteral("ANAN-B"));
    Access::restore(radio);
    radio.emitBackendSliceChangedForTest(1, newSlice());
    const SliceModel* second = radio.slice(1);
    check(second && second->stepHz() == 100,
          "radio B's new slice keeps the default step, not radio A's");

    return failures == 0 ? 0 : 1;
}
