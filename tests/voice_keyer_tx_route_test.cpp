// Ownership and cancellation for the REAL VoiceKeyerTxRoute — the class
// FakeTxAudioRoute stands in for in generated_audio_transmitter_test, so the
// producer/request/context handling was previously untested.
// Socket-free: no connection is opened; the route is driven directly.
// Run: ./build/voice_keyer_tx_route_test

#include "core/VoiceKeyerTxRoute.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>

#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace AetherSDR {
class VoiceKeyerTxRouteTestAccess {
public:
    static unsigned activeTxActivities(const RadioModel& radio)
    {
        return radio.activeTxActivities();
    }
};
}  // namespace AetherSDR

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-56s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) ++g_failed;
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // A disconnected radio refuses before anything is taken from the
    // coordinator: startRefusal() is the first gate the transmitter consults.
    {
        RadioModel radio;
        VoiceKeyerTxRoute route(radio, nullptr);
        const QString refusal = route.startRefusal();
        report("disconnected_radio_refuses_start",
               !refusal.isEmpty() && refusal.contains(QLatin1String("Connect")),
               refusal.toStdString());
        report("refusal_leaves_no_tx_activity",
               VoiceKeyerTxRouteTestAccess::activeTxActivities(radio) == 0);
    }

    // keyOn() asks for PTT against a request admitted at the input boundary.
    // Without that request there is nothing to key against, and keying anyway
    // would put a carrier up carrying no admitted operation (#5659).
    {
        RadioModel radio;
        VoiceKeyerTxRoute route(radio, nullptr);
        report("key_without_admit_is_refused", !route.keyOn());
        report("refused_key_leaves_no_tx_activity",
               VoiceKeyerTxRouteTestAccess::activeTxActivities(radio) == 0);
    }

    // The release path blocker 3(b) was about: an admitted request is handed
    // back by keyOff() even though the radio never showed keyed, so a second
    // keyOn() has nothing to key against until it is admitted again.
    {
        RadioModel radio;
        VoiceKeyerTxRoute route(radio, nullptr);
        const bool admitted = route.admitOperation();
        route.keyOff();
        report("keyoff_hands_back_an_admitted_request",
               admitted && !route.keyOn(),
               admitted ? "admitted" : "admitOperation() refused");
        report("handed_back_request_leaves_no_tx_activity",
               VoiceKeyerTxRouteTestAccess::activeTxActivities(radio) == 0);
    }

    // keyOff() with nothing held is harmless: finish() calls it on paths where
    // the key was never asked for.
    {
        RadioModel radio;
        VoiceKeyerTxRoute route(radio, nullptr);
        route.keyOff();
        route.keyOff();
        report("keyoff_is_idempotent",
               VoiceKeyerTxRouteTestAccess::activeTxActivities(radio) == 0);
    }

    // The operator's TX audio state is left as it was found. #5524: DAX left
    // on after a dax_tx producer finishes means the mic is dead on the next
    // PTT, so release must restore the value claim saw — not a fixed one.
    {
        RadioModel radio;
        VoiceKeyerTxRoute route(radio, nullptr);
        auto& tx = radio.transmitModel();
        tx.setDax(false);
        route.claimAudioPath();
        const bool onForUs = tx.daxOn();
        route.releaseAudioPath();
        report("release_restores_dax_off_as_found",
               onForUs && !tx.daxOn(),
               QString("claimed=%1 after=%2").arg(onForUs).arg(tx.daxOn()).toStdString());
    }

    // The same route on a radio where the operator already had DAX on: release
    // must leave it on, so a fixed restore-to-false is not good enough.
    {
        RadioModel radio;
        VoiceKeyerTxRoute route(radio, nullptr);
        auto& tx = radio.transmitModel();
        tx.setDax(true);
        route.claimAudioPath();
        route.releaseAudioPath();
        report("release_restores_dax_on_as_found", tx.daxOn(),
               QString("after=%1").arg(tx.daxOn()).toStdString());
    }

    std::printf("\n%s (%d failed)\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
