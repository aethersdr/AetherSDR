// Socket-free proof that RadioModel APPLIES the IC-9700 per-deck PA ceiling
// (100 / 75 / 10 W) as the TX frequency moves. The production IcomCivBackend is
// built through RadioModel's own family switch; its session is never started
// and frequency replies enter onCivFrame directly.
#include "TestSettingsProfile.h"
#include "core/backends/icom/IcomCivBackend.h"
#include "core/backends/icom/IcomSession.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>

#include <cstdio>
#include <memory>

using namespace AetherSDR;
using namespace AetherSDR::icom;

namespace AetherSDR::icom {

struct IcomCivBackendTestAccess {
    static void connectAs(IcomCivBackend& b, const char* modelName)
    {
        b.m_model = modelForName(modelName);
        b.m_session = std::make_unique<IcomSession>();
        b.m_session->setCivAddress(b.m_model->civAddress);
        b.m_civReported = b.m_model->civAddress;
        b.m_civModelId = b.m_model->civAddress;
        b.m_connected = true;
        b.m_sessionGeneration = 1;
        b.publishCapabilities();
    }
    static void reportFrequency(IcomCivBackend& b, std::uint64_t hz)
    {
        CivFrame f;
        f.to = kControllerAddress;
        f.from = b.m_model->civAddress;
        f.cmd = cmd::kReadFreq;
        f.data = encodeFreq(hz);
        b.onCivFrame(f, 1);
    }
};

}  // namespace AetherSDR::icom

namespace {
int g_failures = 0;
void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}
}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("icom_power_clamp_model_test"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);

    RadioModel model;
    check(model.rebuildBackendForTest(QStringLiteral("icom")), "RadioModel builds the Icom backend");
    auto* backend = qobject_cast<IcomCivBackend*>(model.backend());
    check(backend != nullptr, "the built backend is the production IcomCivBackend");
    if (!backend) {
        return 1;
    }
    IcomCivBackendTestAccess::connectAs(*backend, "IC-9700");
    check(model.isConnected(), "the model sees the backend connected");

    // The one slice a connected Icom announces, as onSessionConnected emits it.
    SliceDelta slice;
    slice.panId = QStringLiteral("0");
    slice.inUse = true;
    slice.active = true;
    slice.txSlice = true;
    model.emitBackendSliceChangedForTest(0, slice);
    check(model.txSlice() != nullptr, "the backend's slice is the TX slice");

    TransmitModel& tx = model.transmitModel();
    IcomCivBackendTestAccess::reportFrequency(*backend, 435'000'000ULL);
    check(tx.maxPowerLevel() == 75, "tuning to 70 cm clamps the TX power ceiling to 75 W");
    IcomCivBackendTestAccess::reportFrequency(*backend, 1'296'000'000ULL);
    check(tx.maxPowerLevel() == 10, "tuning to 23 cm clamps the TX power ceiling to 10 W");
    IcomCivBackendTestAccess::reportFrequency(*backend, 145'000'000ULL);
    check(tx.maxPowerLevel() == 100, "tuning back to 2 m restores the 100 W ceiling");

    if (g_failures == 0) {
        std::printf("icom_power_clamp_model_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
