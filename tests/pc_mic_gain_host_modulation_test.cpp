// The saved PcMicGain must not attenuate a host-modulating radio, and must come
// back on a family that uses it. The shipping binding (gui/PcMicGain.h) on a
// real RadioModel and AudioEngine with injected backends. Socket-free; the
// engine is never started and, with no TxController, every key is refused.

#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/RadioCertification.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/TransmitDelta.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "gui/PcMicGain.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"
#include "TestSettingsProfile.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>

#include <cstdio>
#include <memory>
#include <utility>

namespace AetherSDR {

class RadioCertificationTestAccess {
public:
    static QJsonArray stages(const RadioCertification& cert) { return cert.m_stages; }
    static void controlEffect(RadioCertification& cert, const RadioCertification::Options& o)
    {
        cert.stageControlEffect(o);
    }
    static void carrierSuppression(RadioCertification& cert, const RadioCertification::Options& o)
    {
        cert.stageCarrierSuppression(o);
    }
};

} // namespace AetherSDR

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* message, int value)
{
    std::printf("[%s] %s (%d)\n", condition ? "PASS" : "FAIL", message, value);
    std::fflush(stdout);
    failures += !condition;
}

// Declares whichever family the test needs; never opens a transport.
class FakeBackend final : public hl2::Hl2Backend {
public:
    explicit FakeBackend(RadioCapabilities caps) : m_caps(std::move(caps)) {}
    RadioCapabilities capabilities() const override { return m_caps; }
    bool isConnected() const override { return linked; }
    bool linked{true};

private:
    RadioCapabilities m_caps;
};

RadioCapabilities hostModulatingCaps()
{
    RadioCapabilities caps;
    caps.family = QStringLiteral("hl2");
    caps.takesTxAudioOverSeam = true;
    caps.canTransmit = true;
    caps.hostModulates = true;
    return caps;
}

RadioCapabilities radioModulatingCaps()
{
    RadioCapabilities caps;
    caps.family = QStringLiteral("flex");
    caps.canTransmit = true;
    return caps;
}

// Receive-only: the modulator is never reached, so this is not the HL2 rule.
RadioCapabilities receiveOnlyHostCaps()
{
    RadioCapabilities caps = hostModulatingCaps();
    caps.canTransmit = false;
    return caps;
}

FakeBackend* install(RadioModel& radio, const RadioCapabilities& caps)
{
    auto* fake = new FakeBackend(caps);
    std::unique_ptr<IRadioBackend> owned(static_cast<IRadioBackend*>(fake));
    radio.setBackendForTest(std::move(owned), caps.family);
    return fake;
}

void connectEdge(RadioModel& radio, FakeBackend* fake, bool connected)
{
    fake->linked = connected;
    emit radio.connectionStateChanged(connected);
}

void reportMic(TransmitModel& tx, const char* selection, int level)
{
    TransmitDelta d;
    d.micSelection = QString::fromLatin1(selection);
    d.micLevel = level;
    tx.applyChanges(d);
}

constexpr int kSaved = 40;

int savedGain()
{
    return AppSettings::instance().value("PcMicGain", 100).toInt();
}

void testFamilySwap()
{
    RadioModel radio;
    AudioEngine audio;
    TransmitModel& tx = radio.transmitModel();
    bindPcMicGain(tx, &audio, [&audio](int percent) { audio.setPcMicGain(percent); });

    // Flex, PC mic: the saved gain is the operator's, and applies.
    FakeBackend* flex = install(radio, radioModulatingCaps());
    connectEdge(radio, flex, true);
    reportMic(tx, "PC", 50);
    check(audio.pcMicGain() == kSaved, "Flex PC mic applies the saved gain", audio.pcMicGain());
    reportMic(tx, "MIC", 50);
    check(audio.pcMicGain() == 100, "Flex hardware mic is unity", audio.pcMicGain());
    reportMic(tx, "PC", 50);
    check(audio.pcMicGain() == kSaved, "back on PC, the saved gain again", audio.pcMicGain());
    connectEdge(radio, flex, false);

    // Connecting the host-modulating radio clears it, with no mic event.
    FakeBackend* hl2 = install(radio, hostModulatingCaps());
    connectEdge(radio, hl2, true);
    check(tx.hostModulation() && tx.micSelection() == QLatin1String("PC"),
          "fixture: host modulation is on and the source is PC", tx.hostModulation());
    check(audio.pcMicGain() == 100, "host-modulating connect: unity", audio.pcMicGain());

    // The operator's slider move and a backend report are both mic state changes.
    tx.setMicLevel(tx.micLevel() == 70 ? 30 : 70);
    check(audio.pcMicGain() == 100, "host-modulating slider move: unity", audio.pcMicGain());
    reportMic(tx, "PC", 12);
    check(audio.pcMicGain() == 100, "host-modulating mic report: unity", audio.pcMicGain());
    check(savedGain() == kSaved, "the saved gain is untouched", savedGain());

    // radiocert's probe drives the engine directly: still a real attenuation.
    audio.setPcMicGain(50);
    check(audio.pcMicGain() == 50, "a direct setPcMicGain is not overridden", audio.pcMicGain());

    // And the next family has its gain back, including with no mic event.
    connectEdge(radio, hl2, false);
    flex = install(radio, radioModulatingCaps());
    connectEdge(radio, flex, true);
    check(!tx.hostModulation(), "fixture: host modulation is off", tx.hostModulation());
    check(audio.pcMicGain() == kSaved, "swap to a Flex restores the saved gain",
          audio.pcMicGain());
    connectEdge(radio, flex, false);

    // A receive-only session is not host modulation (hostModulates && canTransmit).
    FakeBackend* rxOnly = install(radio, receiveOnlyHostCaps());
    connectEdge(radio, rxOnly, true);
    check(!tx.hostModulation() && audio.pcMicGain() == kSaved,
          "a receive-only host radio does not take the rule", audio.pcMicGain());
}

// A grant that arrives mid-session revises the capability after connect.
void testMidSessionGrant()
{
    RadioModel radio;
    AudioEngine audio;
    TransmitModel& tx = radio.transmitModel();
    bindPcMicGain(tx, &audio, [&audio](int percent) { audio.setPcMicGain(percent); });
    tx.applyMicSelectionState(QStringLiteral("PC"));
    check(audio.pcMicGain() == kSaved, "before the grant: the saved gain", audio.pcMicGain());
    tx.setHostModulation(true);
    check(audio.pcMicGain() == 100, "host modulation begins: unity", audio.pcMicGain());
    tx.setHostModulation(false);
    check(audio.pcMicGain() == kSaved, "host modulation ends: the saved gain", audio.pcMicGain());
}

// The stages that move the gain leave it as they found it, which on a
// host-modulating radio is unity and not the saved value.
void testRadiocertRestore()
{
    RadioModel radio;
    AudioEngine audio;
    FakeBackend* hl2 = install(radio, hostModulatingCaps());
    bindPcMicGain(radio.transmitModel(), &audio,
                  [&audio](int percent) { audio.setPcMicGain(percent); });
    connectEdge(radio, hl2, true);

    RadioCertification cert(&radio, &audio);
    RadioCertification::Options o;
    o.settleMs = 0;
    RadioCertificationTestAccess::carrierSuppression(cert, o);
    check(audio.pcMicGain() == 100, "carrier-suppression restores unity", audio.pcMicGain());
    const QJsonObject measured = RadioCertificationTestAccess::stages(cert)
        .first().toObject().value(QStringLiteral("measured")).toObject();
    const int reported = measured.value(QStringLiteral("micGainRestoredTo")).toInt(-1);
    check(reported == 100, "and reports the value it restored", reported);
    RadioCertificationTestAccess::controlEffect(cert, o);
    check(audio.pcMicGain() == 100, "control-effect restores unity", audio.pcMicGain());
}

// MainWindow is not a test target: its session wiring must use the binding and
// must not read the saved gain for itself. A tripwire on today's spelling, not
// a behavior test: a reformat or a correct refactor of those lines fails it,
// and then this check is updated to match.
void testMainWindowUsesBinding()
{
    QFile source(QStringLiteral(AETHER_SOURCE_DIR "/src/gui/MainWindow_Session.cpp"));
    const bool opened = source.open(QIODevice::ReadOnly);
    const QByteArray text = opened ? source.readAll() : QByteArray();
    const auto bindings = static_cast<int>(
        text.count("bindPcMicGain(m_radioModel.transmitModel(), this,"));
    check(bindings == 1, "MainWindow binds the gain once", bindings);
    const auto reads = static_cast<int>(text.count("value(\"PcMicGain\""));
    check(opened && reads == 0, "and never applies the saved gain itself", reads);
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("pc-mic-gain-host-modulation-test"));
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "settings are isolated", 0);
    AppSettings::instance().load();
    AppSettings::instance().setValue(QStringLiteral("PcMicGain"), kSaved);

    testFamilySwap();
    testMidSessionGrant();
    testRadiocertRestore();
    testMainWindowUsesBinding();

    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
