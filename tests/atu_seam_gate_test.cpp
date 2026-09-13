// The ATU start intent must pass the same TX gate on the IRadioBackend seam
// that MOX, TUNE and CW keying pass (#5558, split from #5554 §2.1).
//
// Socket-free: an injected backend records every setAtu() the model
// dispatches, and the test drives TransmitModel::atuStart()/atuBypass()
// exactly as the TX applet, the ATU dialog, the shortcut, the MIDI trigger
// and the automation bridge do. Three things are proved, per the acceptance
// criteria on the issue:
//   1. a blocked start dispatches NOTHING to the backend;
//   2. a permitted start dispatches EXACTLY ONCE;
//   3. bypass is dispatched regardless of the gate.
// Flex is covered too: all families now take the same typed seam exactly once.

#include "TestSettingsProfile.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>
#include <memory>

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool ok, const char* message)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}

class RecordingBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    bool connected{false};
    int atuStarts{0};
    int atuBypasses{0};
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override { connected = true; }
    void disconnectRadio() override { connected = false; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const AetherSDR::TxCoordinator::Operation&, const AetherSDR::TxCoordinator::Completion&) override {}
    void setAtu(bool start, const AetherSDR::TxCoordinator::Operation&, const AetherSDR::TxCoordinator::Completion&) override { start ? ++atuStarts : ++atuBypasses; }
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

struct Fixture {
    RadioModel radio;
    RecordingBackend* backend{nullptr};
    SliceModel* slice{nullptr};
    QStringList interlocks;

    explicit Fixture(const QString& family, bool canTransmit,
                     const QStringList& receiveOnlyModes = {})
    {
        auto owned = std::make_unique<RecordingBackend>();
        backend = owned.get();
        backend->caps.family = family;
        backend->caps.canTransmit = canTransmit;
        backend->caps.receiveOnlyModes = receiveOnlyModes;
        backend->caps.hasTuner = true;
        radio.setBackendForTest(std::move(owned), family);
        check(radio.automationApplySliceFixture(0, QStringLiteral("A")),
              "socket-free slice fixture installed");
        slice = radio.slice(0);
        check(slice != nullptr, "fixture slice exists");
        // The fixture creates the slice with tx=0; every gate resolves through
        // txSlice(), so promote it the way radio status would.
        // The fixture does not bind the slice to a pan; the pan TX inhibit is
        // keyed by pan id, so bind it the way radio status would.
        SliceDelta delta;
        delta.txSlice = true;
        delta.panId = QStringLiteral("0x40000000");
        slice->applyChanges(delta);
        check(radio.txSlice() == slice, "fixture slice is the TX slice");
        check(!slice->panId().isEmpty(), "fixture slice is bound to a pan");
        QObject::connect(&radio, &RadioModel::interlockNotificationRequested, &radio,
                         [this](const QString&, const QString& key, const QString&) {
            interlocks << key;
        });
    }

    void setMode(const QString& mode)
    {
        SliceDelta delta;
        delta.mode = mode;
        slice->applyChanges(delta);
    }
};
} // namespace

static void permittedStartDispatchesOnce()
{
    Fixture f(QStringLiteral("icom"), /*canTransmit=*/true);
    f.radio.transmitModel().atuStart();
    check(f.backend->atuStarts == 1, "permitted start reaches the backend exactly once");
    check(f.interlocks.isEmpty(), "permitted start raises no interlock");
    f.radio.transmitModel().atuBypass();
    check(f.backend->atuBypasses == 1, "bypass reaches the backend");
    check(f.backend->atuStarts == 1, "bypass does not re-dispatch a start");
}

static void receiveOnlyBackendBlocksStart()
{
    Fixture f(QStringLiteral("icom"), /*canTransmit=*/false);
    f.radio.transmitModel().atuStart();
    check(f.backend->atuStarts == 0, "receive-only backend: start dispatches nothing");
    check(f.interlocks.contains(QStringLiteral("rx-only-tx")),
          "receive-only backend: refusal is reported as an interlock");
    f.radio.transmitModel().atuBypass();
    check(f.backend->atuBypasses == 1, "receive-only backend: bypass still dispatches");
}

static void receiveOnlyModeBlocksStart()
{
    Fixture f(QStringLiteral("icom"), /*canTransmit=*/true, {QStringLiteral("WFM")});
    f.setMode(QStringLiteral("WFM"));
    f.radio.transmitModel().atuStart();
    check(f.backend->atuStarts == 0, "receive-only mode: start dispatches nothing");
    check(f.interlocks.contains(QStringLiteral("rx-only-mode:WFM")),
          "receive-only mode: refusal names the mode");
    f.radio.transmitModel().atuBypass();
    check(f.backend->atuBypasses == 1, "receive-only mode: bypass still dispatches");
    // Leaving the receive-only mode re-opens the gate.
    f.setMode(QStringLiteral("USB"));
    f.radio.transmitModel().atuStart();
    check(f.backend->atuStarts == 1, "transmit mode: start dispatches once");
}

static void panInhibitBlocksStart()
{
    Fixture f(QStringLiteral("icom"), /*canTransmit=*/true);
    f.radio.setPanTransmitInhibited(f.slice->panId(), true,
                                    QStringLiteral("Transmit is inhibited on this panadapter"));
    // Exclude the notification raised when the inhibit itself was enabled.
    f.interlocks.clear();
    f.radio.transmitModel().atuStart();
    check(f.backend->atuStarts == 0, "pan TX inhibit: start dispatches nothing");
    bool inhibitKey = false;
    for (const QString& key : f.interlocks) {
        inhibitKey = inhibitKey || key.startsWith(QStringLiteral("pan-tx-inhibit:"))
                                   && key.endsWith(QStringLiteral(":tune-start"));
    }
    check(inhibitKey, "pan TX inhibit: refusal carries the shared tune-start key");
    check(f.interlocks.size() == 1, "pan TX inhibit: exactly one refusal is reported");
    f.radio.transmitModel().atuStart();
    check(f.backend->atuStarts == 0, "pan TX inhibit: repeated start remains blocked");
    check(f.interlocks.size() == 1, "pan TX inhibit: repeated refusal is deduplicated");
    f.radio.transmitModel().atuBypass();
    check(f.backend->atuBypasses == 1, "pan TX inhibit: bypass still dispatches");
    f.radio.setPanTransmitInhibited(f.slice->panId(), false);
    f.radio.transmitModel().atuStart();
    check(f.backend->atuStarts == 1, "inhibit lifted: start dispatches once");
}

static void flexUsesTheSameTypedPath()
{
    Fixture f(QStringLiteral("flex"), /*canTransmit=*/true);
    f.radio.transmitModel().atuStart();
    f.radio.transmitModel().atuBypass();
    check(f.backend->atuStarts == 1 && f.backend->atuBypasses == 1,
          "flex: the same typed seam dispatches each intent exactly once");
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("atu-seam-gate"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    permittedStartDispatchesOnce();
    receiveOnlyBackendBlocksStart();
    receiveOnlyModeBlocksStart();
    panInhibitBlocksStart();
    flexUsesTheSameTypedPath();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
