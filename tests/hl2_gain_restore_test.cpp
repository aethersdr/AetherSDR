#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/RadioStateMemory.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "gui/RfGainRestore.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <cstdio>

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool condition, const char* label)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", label);
    if (!condition) {
        ++failures;
    }
}

int bandGain(const RestoredRadioState& state, const QString& band)
{
    return state.extension.value(QStringLiteral("rfGain")).toObject()
        .value(QStringLiteral("lnaDbByBand")).toObject().value(band).toInt(999);
}

RestoredRadioState rememberedGain()
{
    RestoredRadioState state;
    state.rfFrequencyHz = 14'074'000.0;
    state.sampleRateHz = 48'000;
    state.extensionSchemaVersion = 1;
    state.extension = QJsonObject{
        {QStringLiteral("rfGain"), QJsonObject{
            {QStringLiteral("defaultDb"), 20},
            {QStringLiteral("lnaDbByBand"), QJsonObject{
                {QStringLiteral("20m"), -12}, {QStringLiteral("40m"), -6}}}}}};
    return state;
}

// Exercise synchronous connect seeding and capture without starting transport.
// boardMaxRx skips the unicast discovery socket. No event loop is pumped:
// finishDspSetup cannot run, and disconnect cancels it before destruction.
// TEST-NET-1 alone would NOT make this socket-free.
class GainSession {
public:
    QString panId;
    int echoedGain = 999;
    hl2::Hl2Backend backend;

    GainSession(const RestoredRadioState& state, std::optional<int> pin = std::nullopt)
    {
        QObject::connect(&backend, &IRadioBackend::panCenterBandwidthChanged,
                         &backend, [this](const QString& id, double, double) {
            panId = id;
        });
        QObject::connect(&backend, &IRadioBackend::panRfGainChanged,
                         &backend, [this](const QString&, int gain) {
            echoedGain = gain;
        });
        backend.applyRestoredState(state);
        RadioConnectRequest request;
        request.host = QStringLiteral("192.0.2.1");
        request.serial = QStringLiteral("AA:BB:CC:DD:EE:01");
        request.params.insert(QStringLiteral("boardMaxRx"), 4);
        if (pin.has_value()) {
            request.params.insert(QStringLiteral("lnaGainDb"), *pin);
        }
        backend.connectRadio(request);
        backend.setSliceFrequency(0, state.rfFrequencyHz); // publish the pan identity
        check(!panId.isEmpty(), "connect seeding creates a usable pan identity");
    }
    ~GainSession() { backend.disconnectRadio(); }

    int liveGain() const
    {
        return backend.healthSnapshot().values.value(QStringLiteral("lnaGainDb"), 999).toInt();
    }
    int restoreDisplay(std::optional<int> savedGain, int& writes)
    {
        const RadioCapabilities caps = backend.capabilities();
        return restoreLegacyRfGain(caps.family,
            caps.clientSettingsDomains.testFlag(RadioCapabilities::ClientSettingsDomain::RfGain),
            savedGain, liveGain(), [this, &writes](int gain) {
                ++writes;
                backend.setPanRfGain(panId, gain);
            });
    }
};
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-hl2-gain-restore"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    const RadioSettingsScope scope(QStringLiteral("hl2"), QStringLiteral("AA:BB:CC:DD:EE:01"));
    RadioCapabilities caps;
    {
        GainSession session(rememberedGain());
        caps = session.backend.capabilities();
        check(caps.clientSettingsDomains.testFlag(RadioCapabilities::ClientSettingsDomain::RfGain),
              "HL2 retains the RF-gain domain required by per-band storage");
        int writes = 0;
        check(session.restoreDisplay(20, writes) == -12 && writes == 0,
              "startup displays the restored band gain without replaying the legacy +20");
        check(session.liveGain() == -12, "legacy display restore leaves live 20m gain at -12");
        session.backend.setSliceFrequency(0, 14'080'000.0);
        check(bandGain(session.backend.currentOperatingState(), QStringLiteral("20m")) == -12,
              "same-band capture preserves the saved 20m gain");
        session.backend.setSliceFrequency(0, 7'074'000.0);
        check(session.liveGain() == -6 && session.echoedGain == -6,
              "band hop applies and publishes 40m gain");
        session.backend.setSliceFrequency(0, 14'074'000.0);
        check(session.liveGain() == -12 && session.echoedGain == -12,
              "return to 20m applies and publishes its own gain");
        session.backend.setPanRfGain(session.panId, 5);
        check(session.liveGain() == 5 && session.echoedGain == 5,
              "operator gain change still applies and publishes");
        check(RadioStateMemory::store(scope, caps, session.backend.currentOperatingState()),
              "updated gain persists through the production OperatingState store");
    }
    {
        GainSession session(RadioStateMemory::load(scope, caps));
        int writes = 0;
        check(session.restoreDisplay(20, writes) == 5 && writes == 0 && session.liveGain() == 5,
              "a recreated session restores the operator's +5 despite stale global +20");
        check(bandGain(session.backend.currentOperatingState(), QStringLiteral("40m")) == -6,
              "saving 20m leaves 40m unchanged");
    }
    {
        GainSession session(rememberedGain(), 20);
        check(session.liveGain() == 20, "explicit connect override really sets live gain to +20");
        check(bandGain(session.backend.currentOperatingState(), QStringLiteral("20m")) == -12,
              "production capture preserves -12 while the connect override is active");
        session.backend.setSliceFrequency(0, 14'080'000.0);
        check(bandGain(session.backend.currentOperatingState(), QStringLiteral("20m")) == -12,
              "same-band tune cannot persist the temporary override");
        session.backend.setSliceFrequency(0, 7'074'000.0);
        session.backend.setSliceFrequency(0, 14'074'000.0);
        check(session.liveGain() == -12, "band writeback preserves the overridden start band");
        session.backend.setPanRfGain(session.panId, 5);
        check(bandGain(session.backend.currentOperatingState(), QStringLiteral("20m")) == 5,
              "operator changes still reach the production snapshot after a pin");
    }
    // The same write, but of the PINNED VALUE ITSELF, on the start band. A
    // write that does not MOVE the gain is still the operator choosing that
    // value for this band, so it has to end the pin and record the band exactly
    // as a moving write does. setPanRfGain's equality early return used to sit
    // above both, so this operator got neither. (#5402 review nit 3.)
    {
        GainSession session(rememberedGain(), 20);
        check(session.liveGain() == 20 && bandGain(session.backend.currentOperatingState(),
                                                   QStringLiteral("20m")) == -12,
              "same-value case starts pinned at +20 with 20m still stored as -12");
        session.backend.setPanRfGain(session.panId, 20);
        check(bandGain(session.backend.currentOperatingState(), QStringLiteral("20m")) == 20,
              "an operator write of the pinned value itself records the band");
        session.backend.setSliceFrequency(0, 7'074'000.0);
        session.backend.setSliceFrequency(0, 14'074'000.0);
        check(session.liveGain() == 20,
              "the confirmed value survives a band round trip instead of reverting to -12");
    }
    // Cross-family compatibility at the exact display-restore seam. No Flex or
    // Icom backend is instantiated or changed; their current domain is empty.
    for (const QString& family : {QStringLiteral("flex"), QStringLiteral("icom"),
                                  QStringLiteral("sim"), QStringLiteral("anan")}) {
        int writes = 0;
        const int result = restoreLegacyRfGain(family, false, 20, 7,
            [&writes](int) { ++writes; });
        check(result == 7 && writes == 0, "radio-owned gain retains the existing no-replay behavior");
    }
    int writes = 0;
    check(restoreLegacyRfGain(u"other", true, 20, 7,
              [&writes](int gain) { writes += gain == 20; }) == 20 && writes == 1,
          "a non-HL2 client-owned family retains its existing saved replay");
    check(restoreLegacyRfGain(u"other", true, std::nullopt, 7,
              [&writes](int) { ++writes; }) == 7 && writes == 1,
          "an absent saved gain never writes a default");
    return failures ? 1 : 0;
}
