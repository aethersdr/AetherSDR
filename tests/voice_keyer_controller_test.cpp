// The client-side voice keyer's controller: which keyer the panel is bound to,
// and what happens when the operator changes source mid-operation.
//
// The guard that matters here is RFC #4214's condition that a routing change
// never retargets a running operation, and that the outcome is visible: the
// swap waits for the operation in flight to end, and the operator is told once
// rather than left with a menu that shows one source and a panel driving the
// other.
//
// Socket-free: no connection, no audio device — the controller is built against
// a bare RadioModel with no AudioEngine.
//
// Run: ./build/voice_keyer_controller_test

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/VoiceKeyerSettings.h"
#include "models/DvkModel.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"
#include "models/VoiceKeyer.h"
#include "models/VoiceKeyerController.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-56s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) ++g_failed;
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("voice_keyer_controller_test"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    // The panel always has a keyer to drive, from construction, before any
    // availability pass has run.
    {
        VoiceKeyerSettings::setSource(VoiceKeyerSourceSetting::Local);
        RadioModel radio;
        VoiceKeyerController ctl(radio, nullptr, nullptr);
        report("a_keyer_is_bound_from_construction",
               ctl.bound() != nullptr
                   && ctl.bound() != static_cast<VoiceKeyer*>(&radio.dvkModel()));
    }

    // Choosing the radio keyer while idle binds it immediately.
    {
        VoiceKeyerSettings::setSource(VoiceKeyerSourceSetting::Local);
        RadioModel radio;
        VoiceKeyerController ctl(radio, nullptr, nullptr);
        QSignalSpy bound(&ctl, &VoiceKeyerController::boundKeyerChanged);
        QSignalSpy deferred(&ctl, &VoiceKeyerController::sourceChangeDeferred);
        ctl.setSourceSetting(VoiceKeyerSourceSetting::Radio);
        report("idle_source_change_binds_at_once",
               bound.count() == 1 && deferred.count() == 0
                   && ctl.bound() == static_cast<VoiceKeyer*>(&radio.dvkModel()),
               QString("bound=%1 deferred=%2").arg(bound.count()).arg(deferred.count())
                   .toStdString());
    }

    // The condition: a source change while the bound keyer is busy does not
    // retarget the running operation. It is announced once, however many status
    // ticks the busy keyer emits, and applied when the operation ends.
    {
        VoiceKeyerSettings::setSource(VoiceKeyerSourceSetting::Local);
        RadioModel radio;
        VoiceKeyerController ctl(radio, nullptr, nullptr);
        // The default backend reports selectable mic inputs with MIC chosen, and
        // recording taps the PC mic, so REC is refused until the source is PC.
        radio.transmitModel().setMicSelection(QStringLiteral("PC"));
        VoiceKeyer* local = ctl.bound();
        const bool startedLocal = local && local != static_cast<VoiceKeyer*>(&radio.dvkModel());

        QSignalSpy bound(&ctl, &VoiceKeyerController::boundKeyerChanged);
        QSignalSpy deferred(&ctl, &VoiceKeyerController::sourceChangeDeferred);

        local->recStart(1);
        const bool recording = !local->canStartOperation();

        ctl.setSourceSetting(VoiceKeyerSourceSetting::Radio);
        const bool heldBack = ctl.bound() == local && bound.count() == 0
                              && deferred.count() == 1;
        const bool saidWhich = deferred.count() == 1
                               && deferred.at(0).at(0).value<VoiceKeyerSource>()
                                      == VoiceKeyerSource::Radio;

        // More ticks from the busy keyer must not re-announce the same change.
        ctl.refreshBinding();
        ctl.refreshBinding();
        const bool announcedOnce = deferred.count() == 1;

        // The operation ends: the deferred swap lands on its own.
        local->recStop(1);
        const bool applied = ctl.bound() == static_cast<VoiceKeyer*>(&radio.dvkModel())
                             && bound.count() == 1;

        report("busy_source_change_is_announced_once_then_applied",
               startedLocal && recording && heldBack && saidWhich && announcedOnce && applied,
               QString("rec=%1 held=%2 which=%3 once=%4 applied=%5 bound=%6 deferred=%7")
                   .arg(recording).arg(heldBack).arg(saidWhich).arg(announcedOnce)
                   .arg(applied).arg(bound.count()).arg(deferred.count()).toStdString());
    }

    std::printf("\n%s (%d failed)\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
