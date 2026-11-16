// Client transverters at the RadioModel seam (#6472).
//
// Socket-free: an injected backend records every intent the model dispatches,
// and radio reports are fed back on the backend's own signals. The backend only
// ever sees the IF; the slice, the pan and the TX-side readers see RF. A client
// transverter is receive-only, so MOX, TUNE and an ATU start are refused with a
// named interlock and nothing reaches the backend's keying verbs.

#include "TestSettingsProfile.h"
#include "core/backends/PanDisplayRequest.h"
#include "models/ClientTransverter.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"
#include "core/AppSettings.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QStringList>

#include <cmath>
#include <cstdio>
#include <memory>

namespace AetherSDR {
// Reuses an existing friend rather than adding a production test hook.
struct RadioModelWakeTestAccess {
    static void identity(RadioModel& radio, const QString& serial) { radio.m_lastInfo.serial = serial; }
    static void reload(RadioModel& radio) { radio.loadClientTransverters(); }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool ok, const char* message)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}

bool near(double a, double b) { return std::fabs(a - b) < 1.0e-6; }

class RecordingBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    bool connected{false};
    int tunes{0};
    double lastTuneHz{0.0};
    int panCenters{0};
    double lastPanCenterHz{0.0};
    int keys{0};
    int atuStarts{0};
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override { connected = true; }
    void disconnectRadio() override { connected = false; }
    void setSliceFrequency(int, double hz) override { ++tunes; lastTuneHz = hz; }
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double hz, PanCenterIntent) override
    {
        ++panCenters;
        lastPanCenterHz = hz;
    }
    void setKeying(bool, const AetherSDR::TxCoordinator::Operation&,
                   const AetherSDR::TxCoordinator::Completion&) override { ++keys; }
    void setAtu(bool start, const AetherSDR::TxCoordinator::Operation&,
                const AetherSDR::TxCoordinator::Completion&) override { atuStarts += start; }
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}

    void reportFrequency(double mhz)
    {
        SliceDelta delta;
        delta.frequency = mhz;
        emit sliceChanged(0, delta);
    }
};

// A QO-100 station: one 28.5-29.0 MHz IF, 10461 MHz down and 2371.5 MHz up.
ClientTransverter qo100()
{
    ClientTransverter t;
    t.name = QStringLiteral("QO-100");
    t.ifLowMhz = 28.5;
    t.ifHighMhz = 29.0;
    t.rxLoMhz = 10461.0;
    t.txLoMhz = 2371.5;
    return t;
}

// A single-LO 2 m transverter on the same 28 MHz IF.
ClientTransverter twoMetres()
{
    ClientTransverter t;
    t.name = QStringLiteral("2m");
    t.ifLowMhz = 28.0;
    t.ifHighMhz = 30.0;
    t.rxLoMhz = 116.0;
    return t;
}

struct Fixture {
    RadioModel radio;
    RecordingBackend* backend{nullptr};
    SliceModel* slice{nullptr};
    QStringList interlocks;

    explicit Fixture(bool qualified = true, const QString& serial = QStringLiteral("xvtr-a"))
    {
        auto owned = std::make_unique<RecordingBackend>();
        backend = owned.get();
        backend->caps.family = QStringLiteral("icom");
        backend->caps.canTransmit = true;
        backend->caps.hasTuner = true;
        if (qualified) {
            backend->caps.clientTransverters = ClientTransverterSupport{};
        } else {
            backend->caps.clientTransverters.reset();
        }
        backend->caps.tuningMinHz = 30.0e3;
        backend->caps.tuningMaxHz = 470.0e6;
        backend->caps.sliceFrequencyControl = {SliceFrequencyControl::Authority::Radio,
                                               30'000, 470'000'000};
        radio.setBackendForTest(std::move(owned), QStringLiteral("icom"));
        RadioModelWakeTestAccess::identity(radio, serial);
        check(radio.automationApplySliceFixture(0, QStringLiteral("A")),
              "socket-free slice fixture installed");
        slice = radio.slice(0);
        // The slice fixture installs only on a disconnected model.
        backend->connected = true;
        SliceDelta delta;
        delta.txSlice = true;
        delta.panId = QStringLiteral("0x40000000");
        delta.mode = QStringLiteral("USB");
        delta.frequency = 14.200;
        slice->applyChanges(delta);
        check(radio.txSlice() == slice, "fixture slice is the TX slice");
        QObject::connect(&radio, &RadioModel::interlockNotificationRequested, &radio,
                         [this](const QString&, const QString& key, const QString&) {
            interlocks << key;
        });
        radio.setClientTransverters({qo100(), twoMetres()});
    }

    bool active(const char* name) const
    {
        const ClientTransverter* t = radio.clientTransverters().active();
        return name ? (t && t->name == QLatin1String(name)) : t == nullptr;
    }
};
} // namespace

static void tuneTranslatesBothWays()
{
    Fixture f;
    check(f.radio.usesClientTransverters(), "a qualified backend uses client transverters");
    f.slice->setFrequency(10489.540);
    check(f.backend->tunes == 1 && near(f.backend->lastTuneHz, 28.540e6),
          "RF 10489.540 reaches the backend as IF 28.540");
    check(f.active("QO-100"), "tuning into the RF window activates the transverter");
    f.backend->reportFrequency(28.600);
    check(near(f.slice->frequency(), 10489.600), "an IF report of 28.600 shows RF 10489.600");

    f.slice->setFrequency(144.300);
    check(near(f.backend->lastTuneHz, 28.300e6) && f.active("2m"),
          "a second window selects its own transverter and LO");
}

static void leavingTheWindowDeactivates()
{
    Fixture f;
    f.slice->setFrequency(10489.540);
    f.slice->setFrequency(14.074);
    check(near(f.backend->lastTuneHz, 14.074e6) && f.active(nullptr),
          "a native tune is sent raw and clears the transverter");

    f.slice->setFrequency(10489.540);
    f.backend->reportFrequency(7.100);
    check(f.active(nullptr) && near(f.slice->frequency(), 7.100),
          "the radio's dial leaving the IF window shows the native frequency");
}

static void tunePastTheEdgeIsHeld()
{
    Fixture f;
    f.slice->setFrequency(10489.700);
    f.slice->setFrequency(10489.200);
    check(near(f.backend->lastTuneHz, 28.500e6) && f.active("QO-100"),
          "a tune below the RF window holds the radio at the bottom IF edge");
    f.slice->setFrequency(10495.000);
    check(near(f.backend->lastTuneHz, 29.000e6) && f.active("QO-100"),
          "a tune above the RF window holds the radio at the top IF edge");
}

static void transmitFrequencyUsesTheTxLo()
{
    Fixture f;
    check(near(f.radio.transmitFrequencyMhz(f.slice), 14.200),
          "with no transverter the transmit frequency is the slice frequency");
    f.slice->setFrequency(10489.540);
    check(near(f.radio.transmitFrequencyMhz(f.slice), 2400.040),
          "split LO: slice 10489.540 transmits on 2400.040");
    f.slice->setFrequency(144.300);
    check(near(f.radio.transmitFrequencyMhz(f.slice), 144.300),
          "single LO: the transmit frequency equals the slice frequency");
    check(near(f.radio.transmitFrequencyMhz(nullptr), 0.0), "no slice has no transmit frequency");
}

static void capabilitiesAdmitTheRfWindowAndLeaveTheBackendNative()
{
    Fixture f;
    const RadioCapabilities caps = f.radio.backendCapabilities();
    bool declared = false;
    for (const DeclaredBandRange& band : caps.declaredBandRanges) {
        declared = declared || (band.name == QStringLiteral("QO-100")
            && near(band.lowHz / 1.0e6, 10489.5) && near(band.highHz / 1.0e6, 10490.0));
    }
    check(declared, "the RF window is declared as a band above the seam");
    check(caps.tuningMaxHz >= 10490.0e6 && caps.sliceFrequencyControl.maximumHz >= 10'490'000'000LL,
          "the tuning range above the seam reaches the RF window");
    check(near(caps.txPowerMaxWattsAt(2400.040e6), caps.txPowerMaxWattsAt(28.540e6)),
          "the uplink window carries the IF band's own power rating");
    check(near(f.backend->caps.tuningMaxHz, 470.0e6) && f.backend->caps.declaredBandRanges.isEmpty(),
          "the backend's own capabilities stay native");
}

static void moxIsRefusedOnATransverter()
{
    Fixture f;
    f.slice->setFrequency(10489.540);
    f.radio.transmitModel().setMox(true);
    check(f.backend->keys == 0, "MOX on a transverter reaches no keying verb");
    check(f.interlocks.contains(QStringLiteral("xvtr-rx-only:QO-100")),
          "the MOX refusal names the transverter");
}

static void tuneIsRefusedOnATransverter()
{
    Fixture f;
    f.slice->setFrequency(144.300);
    f.radio.transmitModel().startTune();
    check(!f.radio.transmitModel().isTuning() && f.backend->keys == 0,
          "TUNE on a transverter does not latch or key");
    check(f.interlocks.contains(QStringLiteral("xvtr-rx-only:2m")),
          "the TUNE refusal names the transverter");
}

static void nativeBandStillKeys()
{
    Fixture f;
    f.slice->setFrequency(10489.540);
    f.slice->setFrequency(14.074);
    f.radio.transmitModel().setMox(true);
    check(f.backend->keys == 1 && f.interlocks.isEmpty(),
          "off the transverter band MOX keys once with no interlock");
    f.radio.transmitModel().setMox(false);
}

static void internalTunerIsLockedOut()
{
    Fixture f;
    check(f.radio.transmitModel().atuAvailable(), "the internal ATU is available natively");
    f.slice->setFrequency(10489.540);
    check(!f.radio.transmitModel().atuAvailable(), "an active transverter withdraws the internal ATU");
    f.radio.transmitModel().atuStart();
    check(f.backend->atuStarts == 0, "no ATU start reaches the backend on a transverter");
    f.slice->setFrequency(14.074);
    check(f.radio.transmitModel().atuAvailable(), "leaving the transverter restores the internal ATU");
}

static void headlessTargetsUseTheSameSeam()
{
    Fixture f;
    f.radio.requestBackendSliceTune(0, {10489.540e6, SliceTuneRequest::PanIntent::PreservePan});
    check(near(f.backend->lastTuneHz, 28.540e6) && f.active("QO-100"),
          "a headless slice tune is translated and activates the transverter");
    f.radio.requestBackendPanCenter(QStringLiteral("0"), 10489.750e6,
                                    IRadioBackend::PanCenterIntent::Range);
    check(f.backend->panCenters == 1 && near(f.backend->lastPanCenterHz, 28.750e6),
          "a headless pan centre is shifted to the IF");
    f.radio.requestBackendSliceTune(0, {7.100e6, SliceTuneRequest::PanIntent::PreservePan});
    f.radio.requestBackendPanCenter(QStringLiteral("0"), 7.150e6,
                                    IRadioBackend::PanCenterIntent::Range);
    check(near(f.backend->lastPanCenterHz, 7.150e6), "a native pan centre is sent raw");
}

static void editingTheListKeepsTheRadioOnItsIf()
{
    Fixture f;
    f.slice->setFrequency(10489.540);
    const int tunes = f.backend->tunes;
    f.radio.setClientTransverters({twoMetres()});
    check(f.backend->tunes == tunes, "removing the active transverter does not retune the radio");
    check(near(f.slice->frequency(), 144.540) || near(f.slice->frequency(), 28.540),
          "the slice is re-derived from the IF the radio is still on");
    check(!near(f.slice->frequency(), 10489.540),
          "the removed transverter's RF frequency is no longer shown");
}

static void listIsStoredPerRadio()
{
    Fixture f;
    f.slice->setFrequency(10489.540);
    const QJsonObject doc = f.radio.settingsScope().feature(QStringLiteral("ClientTransverters"));
    const QJsonArray saved = doc.value(QStringLiteral("transverters")).toArray();
    check(saved.size() == 2 && saved.first().toObject().value(QStringLiteral("name")).toString()
              == QStringLiteral("QO-100"),
          "the list is a feature document in the radio's settings scope");
    check(doc.value(QStringLiteral("active")).toInt(-1) == 0, "the active transverter is stored with it");
    check(!saved.isEmpty() && !saved.first().toObject().contains(QStringLiteral("rxOnly")),
          "no per-transverter transmit switch is stored");

    RadioModelWakeTestAccess::identity(f.radio, QStringLiteral("xvtr-b"));
    RadioModelWakeTestAccess::reload(f.radio);
    check(f.radio.clientTransverters().transverters().isEmpty(),
          "another radio of the same family sees none of it");
    RadioModelWakeTestAccess::identity(f.radio, QStringLiteral("xvtr-a"));
    RadioModelWakeTestAccess::reload(f.radio);
    check(f.radio.clientTransverters().transverters().size() == 2 && f.active("QO-100"),
          "the first radio gets its list and active transverter back");
}

static void unidentifiedRadioStoresNothing()
{
    Fixture f(/*qualified=*/true, QString());
    f.slice->setFrequency(10489.540);
    check(near(f.backend->lastTuneHz, 28.540e6), "an unidentified radio still translates this session");
    check(RadioSettingsScope(QStringLiteral("icom"), {})
              .featureExact(QStringLiteral("ClientTransverters")).isEmpty(),
          "but writes no family-wide list");
}

static void unqualifiedBackendIsUntouched()
{
    Fixture f(/*qualified=*/false);
    check(!f.radio.usesClientTransverters() && f.radio.clientTransverters().transverters().isEmpty(),
          "a backend that does not declare the capability keeps no client transverters");
    const RadioCapabilities caps = f.radio.backendCapabilities();
    check(near(caps.tuningMaxHz, 470.0e6) && caps.declaredBandRanges.isEmpty(),
          "its capabilities are passed through unchanged");
    f.backend->reportFrequency(28.540);
    check(near(f.slice->frequency(), 28.540), "its frequency reports are shown as reported");
    f.radio.transmitModel().setMox(true);
    check(f.backend->keys == 1 && f.interlocks.isEmpty(), "and it keys as before");
    f.radio.transmitModel().setMox(false);
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("client_transverter_seam_test"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    tuneTranslatesBothWays();
    leavingTheWindowDeactivates();
    tunePastTheEdgeIsHeld();
    transmitFrequencyUsesTheTxLo();
    capabilitiesAdmitTheRfWindowAndLeaveTheBackendNative();
    moxIsRefusedOnATransverter();
    tuneIsRefusedOnATransverter();
    nativeBandStillKeys();
    internalTunerIsLockedOut();
    headlessTargetsUseTheSameSeam();
    editingTheListKeepsTheRadioOnItsIf();
    listIsStoredPerRadio();
    unidentifiedRadioStoresNothing();
    unqualifiedBackendIsUntouched();

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
