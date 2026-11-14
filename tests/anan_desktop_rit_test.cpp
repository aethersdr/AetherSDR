// The desktop's RIT control reaches the ANAN receiver: SliceModel::setRit() ->
// RadioModel's typed slice-control dispatch -> AnanBackend. Socket-free: a real
// RadioModel with the production backend; P2Client is never started.
#include "TestSettingsProfile.h"
#include "core/backends/anan/AnanBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>

#include <cstdio>
#include <memory>

using namespace AetherSDR;
using namespace AetherSDR::anan;

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) {
        ++failures;
    }
}

class ConnectedAnan : public AnanBackend {
public:
    bool isConnected() const override { return true; }
};

void testDesktopRit()
{
    RadioModel radio;
    auto owned = std::make_unique<ConnectedAnan>();
    ConnectedAnan* backend = owned.get();
    radio.setBackendForTest(std::move(owned), QStringLiteral("anan"));
    backend->setSliceFilter(0, 100, 2900);   // publishes slice 0
    SliceModel* slice = radio.slice(0);
    check(slice != nullptr, "ANAN publication creates slice 0");
    if (!slice) {
        return;
    }

    slice->setRit(true, -350);
    // An unrelated publication: the slice shows what the receiver has, so an
    // intent that never reached the backend is reset here.
    backend->setSliceFilter(0, 100, 2900);
    check(slice->ritOn() && slice->ritFreq() == -350,
          "a desktop RIT intent reaches the receiver and survives its next publication");

    slice->setRit(true, 50'000);
    backend->setSliceFilter(0, 100, 2900);
    check(slice->ritOn() && slice->ritFreq() == AnanBackend::kMaxRitOffsetHz,
          "a desktop offset past the bound is clamped by the receiver");

    slice->setRit(false, -120);
    backend->setSliceFilter(0, 100, 2900);
    check(!slice->ritOn() && slice->ritFreq() == -120,
          "turning RIT off from the desktop keeps the offset");

    check(backend->requestSliceControl(0, {SliceControlRequest::Field::Xit,
                                           IncrementalTuning{true, 100}})
              == ReceiveDispatch::Unsupported,
          "XIT stays unsupported (receive-only)");
    check(backend->requestSliceControl(7, {SliceControlRequest::Field::Rit,
                                           IncrementalTuning{true, 100}})
              == ReceiveDispatch::Unsupported,
          "RIT on a slice with no receiver is refused");
}
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    TestSettingsProfile profile(QStringLiteral("anan-desktop-rit"));
    if (!profile.isValid()) {
        return 1;
    }
    testDesktopRit();
    std::printf("%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
