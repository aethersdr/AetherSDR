// On an HL2 whose transmit gate is open, a key in a receive-only mode is refused.
//
// The refusal lives above the backend (RadioModel::refuseKeyInReceiveOnlyMode);
// the HL2 declares the modes (receiveOnlyModes) and neither Hl2Backend nor
// MetisClient checks the mode, so this pins the one gate there is. Every mode
// the backend declares is tried with MOX, TUNE, a TCI/DAX key, TCI hardware PTT,
// CW PTT and a CW key. A USB key on the same fixture is the positive control.
// Production backend, no socket: the client is only marked as streaming.

#include "TestSettingsProfile.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QSignalSpy>

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace AetherSDR::hl2 {
struct MetisClientTestAccess {
    static void setStreaming(MetisClient& client) { client.m_running = true; }
};
// Reuses an existing friend rather than adding a production test hook.
struct Hl2TxGateTestAccess {
    static void startSession(Hl2Backend& backend)
    {
        QMetaObject::invokeMethod(backend.m_metis, [metis = backend.m_metis] {
            MetisClientTestAccess::setStreaming(*metis);
        }, Qt::BlockingQueuedConnection);
    }
    // The client's key state and the next EP2 control packet, read on its thread.
    static bool wireKeyed(Hl2Backend& backend)
    {
        bool keyed = false;
        QMetaObject::invokeMethod(backend.m_metis, [&keyed, metis = backend.m_metis] {
            const auto packet = metis->buildNextControlPacket();
            const std::size_t frameStarts[2] = {8, 8 + kFrameSize};
            for (const std::size_t fs : frameStarts) {
                keyed = keyed || (packet[fs + 3] & kC0MoxBit) != 0;
            }
            keyed = keyed || metis->isKeyed();
        }, Qt::BlockingQueuedConnection);
        return keyed;
    }
};
} // namespace AetherSDR::hl2

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const std::string& what)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", what.c_str());
    if (!condition) {
        ++failures;
    }
}

hl2::Hl2Backend* prepareFixture(RadioModel& model, const QString& mode)
{
    model.setBackendForTest(std::make_unique<hl2::Hl2Backend>(), QStringLiteral("hl2"));
    auto* backend = static_cast<hl2::Hl2Backend*>(model.backend());
    hl2::Hl2TxGateTestAccess::startSession(*backend);
    if (!model.automationApplySliceFixture(0, QStringLiteral("A")) || !model.slice(0)) {
        qFatal("Could not install the TX slice fixture");
    }
    SliceDelta delta;
    delta.txSlice = true;
    delta.mode = mode;
    model.slice(0)->applyChanges(delta);
    return backend;
}

void positiveControl()
{
    RadioModel model;
    hl2::Hl2Backend* backend = prepareFixture(model, QStringLiteral("USB"));
    check(model.backendCapabilities().canTransmit,
          "control: the HL2 transmit gate is open in this fixture");
    QSignalSpy edges(&model, &RadioModel::radioTransmittingChanged);
    model.transmitModel().setMox(true);
    check(!edges.isEmpty() && edges.first().first().toBool(),
          "control: USB MOX publishes the raw-TX edge");
    check(hl2::Hl2TxGateTestAccess::wireKeyed(*backend),
          "control: USB MOX keys the EP2 frame");
    model.transmitModel().setMox(false);
    check(!hl2::Hl2TxGateTestAccess::wireKeyed(*backend), "control: unkey clears the EP2 frame");
}

void refusedIn(const QString& mode)
{
    const std::string m = mode.toStdString();
    RadioModel model;
    hl2::Hl2Backend* backend = prepareFixture(model, mode);
    check(model.backendCapabilities().canTransmit,
          m + ": precondition, the transmit gate is open (the mode is what refuses)");

    QSignalSpy edges(&model, &RadioModel::radioTransmittingChanged);
    QSignalSpy notices(&model, &RadioModel::interlockNotificationRequested);
    TransmitModel& tx = model.transmitModel();

    tx.setMox(true);
    check(edges.isEmpty() && !tx.isTransmitting(), m + ": MOX is refused and rolled back");
    tx.startTune(TransmitModel::PttSource::Dax);
    check(edges.isEmpty() && !tx.isTuning(), m + ": TUNE is refused and does not latch");
    model.setTransmit(true, TransmitModel::PttSource::Dax);
    check(edges.isEmpty() && !model.isRadioTransmitting(), m + ": a TCI/DAX key is refused");
    tx.requestPttOn(TransmitModel::PttSource::TciHardware);
    check(edges.isEmpty() && !tx.isTransmitting(), m + ": a TCI hardware PTT is refused");
    model.sendCwPtt(true);
    model.sendCwKey(true);
    check(edges.isEmpty() && !model.isRadioTransmitting(), m + ": CW PTT and a CW key key nothing");
    check(!hl2::Hl2TxGateTestAccess::wireKeyed(*backend),
          m + ": no EP2 frame carries MOX and the client is unkeyed");

    bool named = false;
    const QString key = QStringLiteral("rx-only-mode:%1").arg(mode);
    for (const QList<QVariant>& notice : notices) {
        named = named || notice.value(1).toString() == key;
    }
    check(named, m + ": the refusal is the receive-only-mode interlock");

    model.sendCwKey(false);
    model.sendCwPtt(false);
    tx.setMox(false);
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("hl2-receive-only-mode-key"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "Cannot create isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    qunsetenv("AETHER_AUTOMATION");
    hl2::Hl2Backend::setHostOffersTransmit(true);

    positiveControl();

    QStringList modes;
    {
        hl2::Hl2Backend probe;
        modes = probe.capabilities().receiveOnlyModes;
    }
    check(!modes.isEmpty(), "the HL2 declares at least one receive-only mode");
    for (const QString& mode : modes) {
        refusedIn(mode);
    }

    if (failures == 0) {
        std::printf("hl2_receive_only_mode_key_test: all checks passed (%lld modes)\n",
                    static_cast<long long>(modes.size()));
    }
    return failures == 0 ? 0 : 1;
}
