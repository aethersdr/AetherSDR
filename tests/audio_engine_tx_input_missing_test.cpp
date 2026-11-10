// #7078: AudioEngine::txInputDeviceMissing() is refreshed only by
// startTxStream(). A disconnect, or a withdrawn seam route, must clear it so a
// reconnect does not report NoInputDevice before capture is retried. Real
// engine through the friend seam; no capture device, socket or radio is opened
// (pcAudioEnabled stays false, so startTxStream() never runs).
#include "TestSettingsProfile.h"
#include "core/AudioEngine.h"
#include "core/backends/RadioCapabilities.h"

#include <QCoreApplication>
#include <QHostAddress>

#include <cstdio>

namespace AetherSDR {

// Same friend seam as audio_engine_rates_test, in a separate executable.
class AudioEngineRatesTestAccess {
public:
    static void markInputMissing(AudioEngine& engine)
    {
        engine.m_txInputDeviceMissing.store(true);
    }
};

} // namespace AetherSDR

using namespace AetherSDR;

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const char* message)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    } else {
        std::printf("PASS: %s\n", message);
    }
}

RadioCapabilities seamCaps()
{
    RadioCapabilities caps;
    caps.family = QStringLiteral("hl2");
    caps.takesTxAudioOverSeam = true;
    caps.canTransmit = true;
    caps.hostModulates = true;
    return caps;
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("audio-engine-tx-input-missing"));
    QCoreApplication app(argc, argv);
    check(settings.isValid(), "isolated settings profile");

    AudioEngine audio;
    QList<bool> emitted;
    QObject::connect(&audio, &AudioEngine::txInputDeviceMissingChanged,
                     [&emitted](bool missing) { emitted << missing; });

    // Seam route up, capture not attempted (PC Audio off): flag is kept.
    audio.applyBackendAudioCapabilities(true, seamCaps(), false, {});
    AudioEngineRatesTestAccess::markInputMissing(audio);
    audio.applyBackendAudioCapabilities(true, seamCaps(), false, {});
    check(audio.hostModulation() && !audio.isTxStreaming(),
          "seam route without PC Audio opens no capture");
    check(audio.txInputDeviceMissing() && emitted.isEmpty(),
          "a repeated seam publication keeps the last capture result");

    // Disconnect: the flag no longer describes any capture.
    audio.applyBackendAudioCapabilities(false, seamCaps(), false, {});
    check(!audio.txInputDeviceMissing(), "disconnect clears the missing-input flag");
    check(emitted == QList<bool>{false}, "disconnect announces the cleared flag once");
    audio.applyBackendAudioCapabilities(false, seamCaps(), false, {});
    check(emitted.size() == 1, "a second disconnect publication emits nothing");

    // Seam route withdrawn while connected (identity revoked, canTransmit off).
    emitted.clear();
    audio.applyBackendAudioCapabilities(true, seamCaps(), false, {});
    AudioEngineRatesTestAccess::markInputMissing(audio);
    RadioCapabilities rxOnly = seamCaps();
    rxOnly.canTransmit = false;
    audio.applyBackendAudioCapabilities(true, rxOnly, false, {});
    check(!audio.hostModulation() && !audio.txInputDeviceMissing(),
          "seam withdrawal clears the missing-input flag");
    check(emitted == QList<bool>{false}, "seam withdrawal announces the cleared flag");

    // A connected radio that never used the seam (Flex) keeps its own
    // capture result: its PC-audio capture is started outside this method.
    emitted.clear();
    AudioEngineRatesTestAccess::markInputMissing(audio);
    RadioCapabilities flex;
    flex.family = QStringLiteral("flex");
    flex.canTransmit = true;
    flex.takesTxAudioOverSeam = false;
    audio.applyBackendAudioCapabilities(true, flex, true, {});
    check(audio.txInputDeviceMissing() && emitted.isEmpty(),
          "a connected non-seam radio keeps the missing-input flag");
    audio.applyBackendAudioCapabilities(false, flex, true, {});
    check(!audio.txInputDeviceMissing() && emitted == QList<bool>{false},
          "a non-seam disconnect clears it too");

    std::printf("AudioEngine TX input missing: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
