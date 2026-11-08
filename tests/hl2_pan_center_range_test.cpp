// A Hermes-Lite 2 pan centre outside the declared centre range stops at the
// edge, in the backend and in the pan model the GUI draws from.
//
// Every limit is read from capabilities().receivePanCenterControl, the record
// aetherd's receive control refuses against, never a re-typed copy.
// Socket-free: the backend is never connected, so no Metis transport, no DSP
// chain and no radio. Nothing is keyed.

#include "TestSettingsProfile.h"
#include "SeamThreadAffinityProbe.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/anan/AnanBackend.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/icom/IcomCivBackend.h"
#include "core/backends/sim/SimBackend.h"
#ifdef AETHER_BACKEND_RTL
#include "core/backends/rtl/RtlSdrBackend.h"
#endif
#include "models/PanadapterModel.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QString>

#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

using namespace AetherSDR;

namespace AetherSDR::hl2 {
struct Hl2DspReadbackTestAccess {
    // One receiver at 14.0 MHz, at the narrowest rate, so the model's
    // half-span floor (24 kHz) sits below the declared minimum.
    static void receiver(Hl2Backend& backend)
    {
        backend.m_rx.resize(1);
        backend.m_ids.reset(1);
        backend.m_rx[0].sliceFreqHz = 14'000'000;
        backend.m_rx[0].ncoHz = 14'000'000;
        backend.m_sampleRateHz = kIqSampleRatesHz[0];
    }
    static double ncoHz(const Hl2Backend& backend) { return backend.m_rx[0].ncoHz; }
    static void publishPan(Hl2Backend& backend) { backend.emitPanState(0); }
};
} // namespace AetherSDR::hl2

namespace {

using Access = hl2::Hl2DspReadbackTestAccess;

int g_failed = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) {
        ++g_failed;
    }
}

class LinkedHl2Backend final : public hl2::Hl2Backend
{
public:
    bool isConnected() const override { return true; }
};

void testBackendClampsToTheDeclaration()
{
    hl2::Hl2Backend backend;
    test::SeamThreadAffinityProbe probe(&backend);
    test::attachAllSeamSignals(probe);
    const auto range = backend.capabilities().receivePanCenterControl;
    check(range.has_value(), "the HL2 declares a pan centre range");
    if (!range) {
        return;
    }
    const double minHz = static_cast<double>(range->minimumHz);
    const double maxHz = static_cast<double>(range->maximumHz);
    check(minHz == backend.capabilities().tuningMinHz && maxHz == backend.capabilities().tuningMaxHz,
          "the centre range is the declared tuning range, one source");

    Access::receiver(backend);
    double echoed = 0.0;
    QObject::connect(&backend, &IRadioBackend::panCenterBandwidthChanged, &backend,
        [&](const QString&, double mhz, double) { echoed = mhz; });
    const auto centre = [&](double requestHz, double startHz = 14'000'000.0) {
        backend.setPanCenter({}, startHz, IRadioBackend::PanCenterIntent::Range);
        echoed = 0.0;
        backend.setPanCenter({}, requestHz, IRadioBackend::PanCenterIntent::Range);
    };

    centre(minHz - 50'000.0);
    check(Access::ncoHz(backend) == minHz && echoed == minHz / 1.0e6,
          "below the range the DDC stops at the minimum and the echo says so");
    centre(maxHz + 100'000.0);
    check(Access::ncoHz(backend) == maxHz && echoed == maxHz / 1.0e6,
          "above the range the DDC stops at the maximum and the echo says so");
    centre(minHz);
    check(Access::ncoHz(backend) == minHz && echoed == minHz / 1.0e6,
          "the minimum itself is accepted");
    centre(maxHz);
    check(Access::ncoHz(backend) == maxHz && echoed == maxHz / 1.0e6,
          "the maximum itself is accepted");
    centre(std::numeric_limits<double>::quiet_NaN());
    check(Access::ncoHz(backend) == 14'000'000.0 && echoed == 0.0,
          "a NaN centre is refused and the DDC stays put");
    centre(std::numeric_limits<double>::infinity());
    check(Access::ncoHz(backend) == 14'000'000.0 && echoed == 0.0,
          "an infinite centre is refused and the DDC stays put");
    check(probe.count(QStringLiteral("panCenterBandwidthChanged")) > 0
              && probe.violations().isEmpty(),
          "pan geometry echoes are observed on the backend owner thread");
}

// Exercise the shared model's request and optimistic paths with each family's
// production declarations. The recorder has no transport and emits no reply.
class RecordingRangeBackend final : public hl2::Hl2Backend
{
public:
    explicit RecordingRangeBackend(const IRadioBackend& source)
        : m_caps(source.capabilities()), m_policy(source.receiveControlPolicy()) {}
    RadioCapabilities capabilities() const override { return m_caps; }
    ReceiveControlPolicy receiveControlPolicy() const override { return m_policy; }
    bool isConnected() const override { return true; }
    void setPanCenter(const QString&, double hz, PanCenterIntent) override { requestedHz = hz; }
    std::optional<double> requestedHz;

private:
    RadioCapabilities m_caps;
    ReceiveControlPolicy m_policy;
};

void testOtherFamilyRequestsAreUnchanged(const IRadioBackend& source)
{
    const RadioCapabilities caps = source.capabilities();
    std::printf("-- unchanged model routing: %s\n", qPrintable(caps.family));
    check(!caps.receivePanCenterControl
              || source.receiveControlPolicy() == ReceiveControlPolicy::Confirmed,
          "other family either declares no range or confirms receive controls");
    RadioModel radio;
    auto owned = std::make_unique<RecordingRangeBackend>(source);
    RecordingRangeBackend* backend = owned.get();
    radio.setBackendForTest(std::move(owned), caps.family);
    Access::receiver(*backend);
    Access::publishPan(*backend);
    const QString panId = RadioModel::neutralPanIdStringForTest(0);
    PanadapterModel* pan = radio.panadapter(panId);
    check(pan != nullptr, "fixture: recording backend publishes a pan");
    if (!pan) { return; }
    for (const double mhz : {0.05, 38.5, 1800.0}) {
        const double before = pan->centerMhz();
        backend->requestedHz.reset();
        radio.requestPanCenter(panId, mhz);
        check(backend->requestedHz && std::abs(*backend->requestedHz - mhz * 1e6) < 0.5,
              "centre reaches the backend unchanged, including outside HL2/RTL limits");
        check(pan->centerMhz() == (source.receiveControlPolicy() == ReceiveControlPolicy::Confirmed
                  ? before : mhz),
              "confirmed geometry waits for status; optimistic geometry retains the requested centre");
    }
}

void testModelShowsTheCentreTheRadioRuns()
{
    RadioModel radio;
    auto owned = std::make_unique<LinkedHl2Backend>();
    LinkedHl2Backend* backend = owned.get();
    radio.setBackendForTest(std::move(owned), QStringLiteral("hl2"));
    Access::receiver(*backend);
    Access::publishPan(*backend);
    const QString panId = RadioModel::neutralPanIdStringForTest(0);
    PanadapterModel* pan = radio.panadapter(panId);
    check(pan != nullptr, "fixture: the backend's geometry materialised a pane");
    if (!pan) {
        return;
    }
    const auto range = backend->capabilities().receivePanCenterControl;
    const double minMhz = range->minimumHz / 1.0e6;
    const double maxMhz = range->maximumHz / 1.0e6;
    const auto agrees = [&] {
        return std::abs(pan->centerMhz() * 1.0e6 - Access::ncoHz(*backend)) < 0.5;
    };

    // Twice each: the second request finds the DDC already at the edge, so the
    // backend has nothing new to echo and the model keeps what it was given.
    for (int pass = 1; pass <= 2; ++pass) {
        radio.requestPanCenter(panId, maxMhz + 0.1);
        check(pan->centerMhz() == maxMhz && agrees(),
              pass == 1 ? "a centre above the range shows the maximum the radio runs"
                        : "and again with the DDC already at the maximum");
    }
    for (int pass = 1; pass <= 2; ++pass) {
        radio.requestPanCenter(panId, minMhz - 0.05);
        check(pan->centerMhz() == minMhz && agrees(),
              pass == 1 ? "a centre below the range shows the minimum the radio runs"
                        : "and again with the DDC already at the minimum");
    }
    radio.requestPanCenter(panId, 7.1);
    check(pan->centerMhz() == 7.1 && agrees(), "a centre inside the range is taken as asked");
}

} // namespace

int main(int argc, char** argv)
{
    // Hl2Backend reads AppSettings while it is constructed.
    TestSettingsProfile settingsProfile(QStringLiteral("hl2-pan-center-range-test"));
    if (!settingsProfile.isValid()) {
        std::fprintf(stderr, "FAIL: could not create an isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    testBackendClampsToTheDeclaration();
    testModelShowsTheCentreTheRadioRuns();
    testOtherFamilyRequestsAreUnchanged(FlexBackend{});
    testOtherFamilyRequestsAreUnchanged(icom::IcomCivBackend{});
    testOtherFamilyRequestsAreUnchanged(anan::AnanBackend{});
    testOtherFamilyRequestsAreUnchanged(SimBackend{});
#ifdef AETHER_BACKEND_RTL
    testOtherFamilyRequestsAreUnchanged(rtl::RtlSdrBackend{});
#endif
    std::printf("%s: %d failure(s)\n", argv[0], g_failed);
    return g_failed == 0 ? 0 : 1;
}
