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
#include "models/PanadapterModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"
#include "core/AppSettings.h"
#include "core/TciProtocol.h"

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
    static PanadapterModel* pan(RadioModel& radio, const QString& modelPanId)
    {
        return radio.ensureOwnedPanadapter(modelPanId, nullptr);
    }
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
    int unkeys{0};
    int tuneOns{0};
    int tuneOffs{0};
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
    void setKeying(bool key, const AetherSDR::TxCoordinator::Operation&,
                   const AetherSDR::TxCoordinator::Completion&) override
    {
        ++keys;
        unkeys += !key;
    }
    void setTune(bool on, int, const AetherSDR::TxCoordinator::Operation&,
                 const AetherSDR::TxCoordinator::Completion&) override
    {
        tuneOns += on;
        tuneOffs += !on;
    }
    void setAtu(bool start, const AetherSDR::TxCoordinator::Operation&,
                const AetherSDR::TxCoordinator::Completion&) override { atuStarts += start; }
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
    // The confirmed (wheel-step) tune path, off unless a case turns it on.
    ReceiveControlPolicy policy{ReceiveControlPolicy::Optimistic};
    bool admitReceiveTune{true};
    int receiveTunes{0};
    double lastReceiveTuneHz{0.0};
    ReceiveControlPolicy receiveControlPolicy() const override { return policy; }
    bool requestReceiveTune(int, double hz, ReceiveTuneView) override
    {
        ++receiveTunes;
        lastReceiveTuneHz = hz;
        return admitReceiveTune;
    }

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

    explicit Fixture(bool qualified = true, const QString& serial = QStringLiteral("xvtr-a"),
                     int receivers = 1)
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
        backend->caps.maxSlices = receivers;
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

    // A pan for backend pan "0", with the slice on it: the confirmed tune
    // path and the inbound pan geometry both resolve a PanadapterModel.
    PanadapterModel* addPan()
    {
        const int index = radio.panIndexForBackendIdForTest(QStringLiteral("0"));
        const QString panId = RadioModel::neutralPanIdStringForTest(index);
        PanadapterModel* pan = RadioModelWakeTestAccess::pan(radio, panId);
        SliceDelta delta;
        delta.panId = panId;
        slice->applyChanges(delta);
        return pan;
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
    f.slice->setFrequency(10490.600);
    check(near(f.backend->lastTuneHz, 29.000e6) && f.active("QO-100"),
          "a tune above the RF window holds the radio at the top IF edge");
    const int tunes = f.backend->tunes;
    f.slice->setFrequency(10495.000);
    check(f.backend->tunes == tunes && f.active("QO-100"),
          "a tune further than the hold reach past the edge is refused, not held");
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

// Blocker from the #7373 review: the key-on refusal cannot see a tune made
// after key-on, so the tune itself is refused while the transmitter is up.
static void tuningOntoATransverterWhileKeyedIsRefused()
{
    Fixture f;
    f.backend->reportFrequency(14.200);
    f.radio.transmitModel().setMox(true);
    check(f.radio.transmitModel().isTransmitting() && f.backend->keys == 1,
          "MOX on a native band keys");
    const int tunes = f.backend->tunes;
    f.slice->setFrequency(10489.540);
    check(f.backend->tunes == tunes && f.active(nullptr),
          "a tune onto a transverter band while keyed reaches no backend tune");
    check(f.interlocks.contains(QStringLiteral("xvtr-rx-only:QO-100")),
          "the refusal names the transverter");
    check(near(f.slice->frequency(), 14.200), "the slice goes back to what the radio reported");
    f.radio.requestBackendSliceTune(0, {144.300e6, SliceTuneRequest::PanIntent::PreservePan});
    check(f.backend->tunes == tunes && f.active(nullptr),
          "a headless tune onto a transverter band while keyed is refused too");
    f.slice->setFrequency(14.074);
    check(f.backend->tunes == tunes + 1 && near(f.backend->lastTuneHz, 14.074e6),
          "a native tune while keyed is still sent");
    f.radio.transmitModel().setMox(false);
    check(f.backend->unkeys == 1, "the unkey reaches the backend");
    f.slice->setFrequency(10489.540);
    check(near(f.backend->lastTuneHz, 28.540e6) && f.active("QO-100"),
          "once unkeyed the same tune is translated and activates the transverter");
}

static void tuningOntoATransverterDuringTuneIsRefused()
{
    Fixture f;
    f.backend->reportFrequency(14.200);
    f.radio.transmitModel().startTune();
    check(f.radio.transmitModel().isTuning() && f.backend->tuneOns == 1,
          "TUNE on a native band starts the carrier");
    const int tunes = f.backend->tunes;
    f.slice->setFrequency(10489.540);
    check(f.backend->tunes == tunes && f.active(nullptr),
          "a tune onto a transverter band during TUNE reaches no backend tune");
    check(f.interlocks.contains(QStringLiteral("xvtr-rx-only:QO-100")),
          "the refusal names the transverter");
    f.radio.transmitModel().stopTune();
    check(!f.radio.transmitModel().isTuning() && f.backend->tuneOffs == 1,
          "the tune stop reaches the backend");
    f.slice->setFrequency(10489.540);
    check(near(f.backend->lastTuneHz, 28.540e6) && f.active("QO-100"),
          "once the carrier is off the same tune activates the transverter");
}

// Blocker from the #7373 review: qualification belongs to the backend. The
// next backend in the same process must inherit none of it.
static void theNextBackendInheritsNothing()
{
    Fixture f;
    f.slice->setFrequency(10489.540);
    check(f.radio.usesClientTransverters() && f.active("QO-100"), "the Icom session is on QO-100");

    auto owned = std::make_unique<RecordingBackend>();
    RecordingBackend* flex = owned.get();
    flex->caps.family = QStringLiteral("flex");
    flex->caps.canTransmit = true;
    flex->caps.maxSlices = 8;
    flex->caps.tuningMinHz = 30.0e3;
    flex->caps.tuningMaxHz = 54.0e6;
    f.radio.setBackendForTest(std::move(owned), QStringLiteral("flex"));
    flex->connected = true;
    check(!f.radio.usesClientTransverters() && f.active(nullptr)
              && f.radio.clientTransverters().transverters().isEmpty(),
          "a backend that replaces it holds no client transverters");
    const RadioCapabilities caps = f.radio.backendCapabilities();
    check(near(caps.tuningMaxHz, 54.0e6) && caps.declaredBandRanges.isEmpty(),
          "its capabilities carry none of the Icom's bands or tuning range");
    f.radio.requestBackendSliceTune(0, {28.700e6, SliceTuneRequest::PanIntent::PreservePan});
    check(flex->tunes == 1 && near(flex->lastTuneHz, 28.700e6), "its tunes are sent raw");
    f.radio.requestBackendPanCenter(QStringLiteral("0"), 28.750e6,
                                    IRadioBackend::PanCenterIntent::Range);
    check(near(flex->lastPanCenterHz, 28.750e6), "its pan centres are sent raw");
    check(!f.radio.setClientTransverters({qo100()})
              && f.radio.clientTransverters().transverters().isEmpty(),
          "and it accepts no list");
}

// An Icom reports one receiver until it has identified itself, so the list is
// loaded for a model that turns out to have two.
static void qualificationFollowsTheIdentifiedModel()
{
    Fixture f;
    f.slice->setFrequency(10489.540);
    f.backend->reportFrequency(28.540);
    check(near(f.slice->frequency(), 10489.540), "before identification the slice shows RF");
    f.backend->caps.maxSlices = 2;
    emit f.backend->capabilitiesChanged();
    check(!f.radio.usesClientTransverters() && f.active(nullptr)
              && f.radio.clientTransverters().transverters().isEmpty(),
          "identifying as a two-receiver model withdraws client transverters");
    check(near(f.slice->frequency(), 28.540), "the slice shows the IF the radio is on");
    check(near(f.radio.backendCapabilities().tuningMaxHz, 470.0e6),
          "the capabilities above the seam are native again");
    f.slice->setFrequency(28.600);
    check(near(f.backend->lastTuneHz, 28.600e6), "tunes are sent raw");
    f.radio.transmitModel().setMox(true);
    check(f.backend->keys == 1, "and nothing refuses MOX in a transverter's name");
    f.radio.transmitModel().setMox(false);
    check(!f.radio.setClientTransverters({qo100()}),
          "Apply reports that the list was not stored");

    f.backend->caps.maxSlices = 1;
    emit f.backend->capabilitiesChanged();
    check(f.radio.usesClientTransverters()
              && f.radio.clientTransverters().transverters().size() == 2,
          "a backend that qualifies again gets this radio's stored list back");
}

// Review finding 4: Add, Remove, Add in the dialog used to produce two rows
// with one name, and the first namesake took the active transverter's place.
static void aNamesakeDoesNotTakeOverTheActiveTransverter()
{
    Fixture f;
    ClientTransverter namesake = twoMetres();
    namesake.name = QStringLiteral("QO-100");
    f.radio.setClientTransverters({qo100(), namesake});
    f.slice->setFrequency(144.300);
    f.backend->reportFrequency(28.300);
    check(f.radio.clientTransverters().activeIndex() == 1, "the second namesake is active");
    f.radio.setClientTransverters({qo100(), namesake});
    check(f.radio.clientTransverters().activeIndex() == 1 && near(f.slice->frequency(), 144.300),
          "re-applying the same list keeps the same row active and the slice on RF");
    f.radio.transmitModel().setMox(true);
    check(f.backend->keys == 0, "and the receive-only refusal still holds");
}

// Review finding 5: a wheel step the backend refuses must not leave a
// transverter active that the radio never reached.
static void aRefusedConfirmedTuneActivatesNothing()
{
    Fixture f;
    check(f.addPan() != nullptr, "the fixture has a pan");
    f.backend->policy = ReceiveControlPolicy::Confirmed;
    f.backend->admitReceiveTune = false;
    const bool sent = f.radio.requestConfirmedReceiveTune(
        0, 10489.540, IRadioBackend::ReceiveTuneView::Preserve);
    check(f.backend->receiveTunes == 1 && near(f.backend->lastReceiveTuneHz, 28.540e6),
          "the confirmed tune reaches the backend as the IF");
    check(!sent && f.active(nullptr), "a tune the backend refuses leaves no transverter active");
    check(f.radio.settingsScope().feature(QStringLiteral("ClientTransverters"))
              .value(QStringLiteral("active")).toInt(-1) == -1,
          "and none is stored as active");

    f.backend->admitReceiveTune = true;
    check(f.radio.requestConfirmedReceiveTune(0, 10489.540,
                                              IRadioBackend::ReceiveTuneView::Preserve)
              && f.active("QO-100"),
          "an admitted one activates it");
    const int before = f.backend->receiveTunes;
    check(!f.radio.requestConfirmedReceiveTune(0, 1296.200,
                                               IRadioBackend::ReceiveTuneView::Preserve)
              && f.backend->receiveTunes == before && f.active("QO-100"),
          "a request nothing reaches is refused before the backend and changes nothing");
}

// Review finding 6: turning VOX on is a client command.
static void voxCannotBeTurnedOnOnATransverter()
{
    Fixture f;
    f.slice->setFrequency(10489.540);
    f.radio.transmitModel().setVoxEnable(true);
    check(!f.radio.transmitModel().voxEnable(), "VOX is not turned on on a transverter band");
    bool told = false;
    for (const QString& key : std::as_const(f.interlocks)) {
        told = told || key.endsWith(QStringLiteral("vox-enable"));
    }
    check(told, "the operator is told why");
    f.slice->setFrequency(14.074);
    f.radio.transmitModel().setVoxEnable(true);
    check(f.radio.transmitModel().voxEnable(), "off the transverter band VOX turns on");
}

// Review finding 8: 0/0 is "not known yet", not a centre to shift.
static void anUnknownPanCentreIsNotShifted()
{
    Fixture f;
    f.slice->setFrequency(10489.540);
    PanadapterModel* pan = f.addPan();
    check(pan != nullptr, "the fixture has a pan");
    if (!pan) {
        return;
    }
    emit f.backend->panCenterBandwidthChanged(QStringLiteral("0"), 28.750, 0.2);
    check(near(pan->centerMhz(), 10489.750), "a known centre is shifted to RF");
    emit f.backend->panCenterBandwidthChanged(QStringLiteral("0"), 0.0, 0.0);
    check(!near(pan->centerMhz(), 10461.0),
          "an unknown centre is not turned into the LO frequency");
}

// Review finding 9: the radio's own range is cached, and follows the backend.
static void theNativeRangeFollowsTheBackend()
{
    Fixture f;
    f.backend->reportFrequency(14.200);
    const int tunes = f.backend->tunes;
    f.slice->setFrequency(1296.200);
    check(f.backend->tunes == tunes, "1296.200 is outside a 470 MHz radio and is not sent");
    f.backend->caps.tuningMaxHz = 1300.0e6;
    emit f.backend->capabilitiesChanged();
    f.slice->setFrequency(1296.200);
    check(f.backend->tunes == tunes + 1 && near(f.backend->lastTuneHz, 1296.200e6),
          "once the backend reports a wider range the same tune is sent raw");
}

// Maintainer ruling on #7373: TCI tx_enable is false while a transverter is
// active. TciServer re-broadcasts tx_enable on capabilitiesChanged, which
// every activation and deactivation publishes.
static void tciTxEnableFollowsTheActiveTransverter()
{
    Fixture f;
    int published = 0;
    QObject::connect(&f.radio, &RadioModel::capabilitiesChanged, &f.radio,
                     [&published](bool, const RadioCapabilities&) { ++published; });
    f.backend->reportFrequency(14.074);
    check(f.radio.isConnected() && TciProtocol::transmitPermitted(&f.radio, f.slice),
          "on a native band TCI tx_enable is true");
    f.slice->setFrequency(10489.540);
    check(!TciProtocol::transmitPermitted(&f.radio, f.slice),
          "on a transverter band TCI tx_enable is false");
    check(published >= 1, "activation republishes capabilities, so tx_enable is re-broadcast");
    const int before = published;
    f.backend->reportFrequency(7.100);
    check(TciProtocol::transmitPermitted(&f.radio, f.slice) && published > before,
          "the radio's dial leaving the IF makes it true again and re-broadcasts it");
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
    check(near(f.slice->frequency(), 28.540) && f.active(nullptr),
          "the slice shows the IF the radio is still on, under no transverter");
    check(!near(f.slice->frequency(), 10489.540),
          "the removed transverter's RF frequency is no longer shown");
}

// A third transverter whose IF window does not overlap the other two.
static ClientTransverter sixMetreIf()
{
    ClientTransverter t;
    t.name = QStringLiteral("23cm");
    t.ifLowMhz = 50.0;
    t.ifHighMhz = 52.0;
    t.rxLoMhz = 1246.0;
    return t;
}

static void removingAnotherRowKeepsTheActiveTransverter()
{
    Fixture f;
    // [23cm, QO-100, 2m]: removing the first row moves QO-100 to the index
    // 2m then holds, and 2m's IF window also contains 28.540.
    f.radio.setClientTransverters({sixMetreIf(), qo100(), twoMetres()});
    f.slice->setFrequency(10489.540);
    const int tunes = f.backend->tunes;
    f.radio.setClientTransverters({qo100(), twoMetres()});
    check(f.active("QO-100") && near(f.slice->frequency(), 10489.540),
          "removing a row above the active transverter keeps it active by name");
    f.radio.setClientTransverters({twoMetres(), qo100()});
    check(f.active("QO-100") && near(f.slice->frequency(), 10489.540),
          "reordering the rows keeps it active by name");
    f.radio.setClientTransverters({qo100()});
    check(f.active("QO-100") && near(f.slice->frequency(), 10489.540),
          "removing an unrelated row below it keeps it active");
    check(f.backend->tunes == tunes, "none of those edits retunes the radio");
    f.radio.transmitModel().setMox(true);
    check(f.backend->keys == 0 && f.interlocks.contains(QStringLiteral("xvtr-rx-only:QO-100")),
          "and the receive-only refusal still names it");
}

static void aSecondReceiverIsNotServed()
{
    Fixture f(true, QStringLiteral("xvtr-dual"), 2);
    check(!f.radio.usesClientTransverters() && f.radio.clientTransverters().transverters().isEmpty(),
          "a two-receiver backend does not engage client transverters, whatever it declares");
    f.slice->setFrequency(28.540);
    check(near(f.backend->lastTuneHz, 28.540e6), "its tunes are sent raw");
}

static void aFrequencyNothingReachesIsRefused()
{
    Fixture f;
    f.backend->reportFrequency(14.200);
    const int tunes = f.backend->tunes;
    f.slice->setFrequency(1296.200);
    check(f.backend->tunes == tunes, "a frequency outside the radio and every window is not sent");
    check(f.interlocks.contains(QStringLiteral("xvtr-tune-range")), "the operator is told why");
    check(near(f.slice->frequency(), 14.200), "the slice goes back to what the radio reported");

    f.slice->setFrequency(10489.540);
    f.backend->reportFrequency(28.540);
    const int onBand = f.backend->tunes;
    f.interlocks.clear();
    f.slice->setFrequency(1296.200);
    check(f.backend->tunes == onBand && f.active("QO-100"),
          "on a transverter, an unrelated band is refused, not held at the band edge");
    check(near(f.slice->frequency(), 10489.540),
          "and the slice still shows the transverter frequency");
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
    tuningOntoATransverterWhileKeyedIsRefused();
    tuningOntoATransverterDuringTuneIsRefused();
    theNextBackendInheritsNothing();
    qualificationFollowsTheIdentifiedModel();
    aNamesakeDoesNotTakeOverTheActiveTransverter();
    aRefusedConfirmedTuneActivatesNothing();
    voxCannotBeTurnedOnOnATransverter();
    anUnknownPanCentreIsNotShifted();
    theNativeRangeFollowsTheBackend();
    tciTxEnableFollowsTheActiveTransverter();
    internalTunerIsLockedOut();
    headlessTargetsUseTheSameSeam();
    editingTheListKeepsTheRadioOnItsIf();
    removingAnotherRowKeepsTheActiveTransverter();
    aSecondReceiverIsNotServed();
    aFrequencyNothingReachesIsRefused();
    listIsStoredPerRadio();
    unidentifiedRadioStoresNothing();
    unqualifiedBackendIsUntouched();

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
