// RadioModel::submitTxAudioDirect(): the AudioEngine thread's way to hand a
// transmit block to the backend without the hop through the GUI thread.
// Its return value tells the caller whether to queue the block to the GUI
// thread instead, so both answers are pinned here:
//   false  the backend offers no direct submitter, or the submitter's backend
//          is gone: the caller queues the block as before;
//   true   the block was taken, or its dispatch was refused: a refused block
//          must not be resent on the queued path.
// The submitter comes from IRadioBackend::directTxSubmitter(), not from a cast
// to a concrete backend. Socket-free.

#include "TestSettingsProfile.h"
#include "TxTestAuthority.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "models/RadioModel.h"

#include <QByteArray>
#include <QCoreApplication>

#include <cstdio>
#include <memory>

using namespace AetherSDR;

namespace {

int g_failures = 0;
void check(bool ok, const char* what)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

// The minimum a backend implements. directTxSubmitter() is left alone unless
// a test installs one, so the default is what is under test then.
class FakeBackend final : public IRadioBackend {
public:
    RadioCapabilities capabilities() const override
    {
        RadioCapabilities c;
        c.family = QStringLiteral("fake");
        c.canTransmit = true;
        return c;
    }
    bool isConnected() const override { return false; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}

    DirectTxSubmitter directTxSubmitter() override
    {
        return submitter ? submitter : IRadioBackend::directTxSubmitter();
    }
    DirectTxSubmitter submitter;
};

QByteArray block()
{
    return QByteArray(256 * 2 * 2, '\0');   // 256 stereo int16 frames
}

void testSeamDefaults()
{
    std::printf("\n  the seam\n");
    FakeBackend plain;
    check(!plain.IRadioBackend::directTxSubmitter(), "the default offers no direct submitter");
    FlexBackend flex;
    check(!static_cast<IRadioBackend&>(flex).directTxSubmitter(),
          "a Flex offers none: it does not modulate on the host");
    hl2::Hl2Backend hermesLite;
    check(static_cast<bool>(static_cast<IRadioBackend&>(hermesLite).directTxSubmitter()),
          "an HL2 offers one through the seam, no cast needed");
}

void testNoSubmitterReturnsFalse()
{
    std::printf("\n  a backend without a direct submitter\n");
    TxTestAuthority authority;
    RadioModel model;
    model.setBackendForTest(std::make_unique<FakeBackend>(), QStringLiteral("fake"));
    check(!model.submitTxAudioDirect(block(), 48000, TxAudioSource::ClientLeveled,
                                     authority.context),
          "false: the caller queues the block to the GUI thread");
}

void testTakenAndRefused()
{
    std::printf("\n  a backend with a direct submitter\n");
    TxTestAuthority authority;
    int calls = 0;
    auto owned = std::make_unique<FakeBackend>();
    owned->submitter = [&calls](const QByteArray&, int, TxAudioSource,
                                const TxCoordinator::Context&) {
        ++calls;
        return true;
    };
    RadioModel model;
    model.setBackendForTest(std::move(owned), QStringLiteral("fake"));

    check(model.submitTxAudioDirect(block(), 48000, TxAudioSource::ClientLeveled,
                                    authority.context),
          "true for an admitted block");
    check(calls == 1, "the admitted block reached the backend once");

    check(authority.coordinator.cancel(authority.actor, authority.operation),
          "control: the operation is cancelled");
    check(!authority.context.permitsDispatch(TxCoordinator::monotonicMs()),
          "control: its context now refuses dispatch");
    check(model.submitTxAudioDirect(block(), 48000, TxAudioSource::ClientLeveled,
                                    authority.context),
          "true for a refused block, so the caller does not resend it queued");
    check(calls == 1, "the refused block never reached the backend");
}

void testGoneBackendReturnsFalse()
{
    std::printf("\n  a submitter whose backend is gone\n");
    TxTestAuthority authority;
    auto owned = std::make_unique<FakeBackend>();
    owned->submitter = [](const QByteArray&, int, TxAudioSource,
                          const TxCoordinator::Context&) { return false; };
    RadioModel model;
    model.setBackendForTest(std::move(owned), QStringLiteral("fake"));
    check(!model.submitTxAudioDirect(block(), 48000, TxAudioSource::ClientLeveled,
                                     authority.context),
          "false is passed through: the caller queues the block");
}

void testReplacedBackendDropsTheSubmitter()
{
    std::printf("\n  replacing the backend\n");
    TxTestAuthority authority;
    int calls = 0;
    auto first = std::make_unique<FakeBackend>();
    first->submitter = [&calls](const QByteArray&, int, TxAudioSource,
                                const TxCoordinator::Context&) {
        ++calls;
        return true;
    };
    RadioModel model;
    model.setBackendForTest(std::move(first), QStringLiteral("fake"));
    model.setBackendForTest(std::make_unique<FakeBackend>(), QStringLiteral("fake"));
    check(!model.submitTxAudioDirect(block(), 48000, TxAudioSource::ClientLeveled,
                                     authority.context),
          "the next backend's answer applies, not the previous one's");
    check(calls == 0, "the previous backend's submitter is not called");
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("radio-model-direct-tx-test"));
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "settings are isolated");

    testSeamDefaults();
    testNoSubmitterReturnsFalse();
    testTakenAndRefused();
    testGoneBackendReturnsFalse();
    testReplacedBackendDropsTheSubmitter();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
