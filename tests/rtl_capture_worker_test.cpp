// No socket or radio peer: inject device operations into the actual USB worker
// and drive the actual backend's publication path with controlled callbacks.
#include "core/backends/rtl/RtlSdrBackend.h"
#include "core/backends/rtl/RtlSdrWorker.h"
#include "SeamThreadAffinityProbe.h"
#include "RtlInjectedDevice.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>

using namespace AetherSDR;
using T = rtl::RtlCaptureTransaction;
namespace AetherSDR::rtl {
struct RtlCaptureBackendTestAccess {
    static void start(RtlSdrBackend& backend, std::unique_ptr<RtlSdrWorker::Device> device, int capacity = 1)
    {
        backend.m_requested.hardware = {100'000'000, 2'400'000, 0, 0, 0, 240};
        backend.m_requested.receivers = {{{0, 100'000'000, -100'000, 100'000, 0, 0, 0}, T::Mode::Wfm}};
        if (capacity > 1) {
            backend.m_receiverCapacity = capacity;
            backend.m_capture = RtlCaptureTransaction({8, static_cast<std::size_t>(capacity)});
            backend.m_requested.receivers = {{{0, 100'000'000, -8000, 8000, 0, 3000, 3000}, T::Mode::Fm}};
        }
        backend.startCapture(std::make_unique<RtlSdrWorker>(std::move(device), nullptr, capacity));
    }
    static std::size_t pending(const RtlSdrBackend& backend) { return backend.m_capture.pendingCount(); }
};
}
static int failures = 0;
static void check(bool value, const char* message)
{
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
using DeviceState = AetherSDR::test::DeviceState;
using InjectedDevice = AetherSDR::test::InjectedDevice;
template<class Predicate> bool waitFor(Predicate predicate)
{
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents(); QThread::msleep(1);
    }
    return predicate();
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    auto state = std::make_shared<DeviceState>();
    rtl::RtlSdrBackend backend;
    test::SeamThreadAffinityProbe probe(&backend);
    test::attachAllSeamSignals(probe);
    int changes = 0, slices = 0, audio = 0;
    QObject::connect(&backend, &IRadioBackend::operatingStateChanged, [&] { ++changes; });
    QObject::connect(&backend, &IRadioBackend::sliceChanged, [&](int, const SliceDelta&) { ++slices; });
    QObject::connect(&backend, &IRadioBackend::audioFrameReady, [&](const PcmFrame&) { ++audio; });
    rtl::RtlCaptureBackendTestAccess::start(backend, std::make_unique<InjectedDevice>(state));
    check(waitFor([&] { std::lock_guard lock(state->mutex); return state->inReadback; }),
          "initial hardware request reached readback");
    check(!backend.isConnected() && changes == 0 && slices == 0,
          "initial requested state never published before readback");
    state->releaseReadback();
    check(waitFor([&] { return backend.isConnected() && state->starts == 1; }), "initial capture confirmed");
    check(backend.currentOperatingState().rfFrequencyHz == 100'000'000, "initial confirmed center");
    const int initialChanges = changes;
    backend.setSliceFrequency(0, 100'200'000);
    check(changes == initialChanges, "in-window request waits for callback adoption");
    state->block();
    check(waitFor([&] { state->block(); QThread::msleep(5); return changes > initialChanges; }), "in-window receiver adoption acknowledged");
    check(state->starts == 1 && state->cancels == 0 && state->writes == 6,
          "receiver-only change never cancels/restarts/writes USB");
    const int noOpChanges = changes;
    backend.setSliceFrequency(0, 100'200'000); state->block();
    check(waitFor([&] { state->block(); QThread::msleep(5); return changes > noOpChanges; }), "no-op acknowledged");
    check(state->starts == 1 && state->cancels == 0, "no-op does not restart USB");

    // Hold the device readback, supersede a rate change, then release it. Old
    // success must compensate before the latest desired set publishes.
    {
        std::lock_guard lock(state->mutex); state->holdReadback = true;
    }
    backend.setPanBandwidth({}, 2'000'000);
    check(waitFor([&] { std::lock_guard lock(state->mutex); return state->inReadback; }), "rate change quiesced");
    check(backend.currentOperatingState().sampleRateHz == 2'400'000,
          "pending rate does not reach OperatingState");
    for (int i = 0; i < 100; ++i) { backend.setPanBandwidth({}, 1'536'000); }
    check(rtl::RtlCaptureBackendTestAccess::pending(backend) == 1, "backend pending work stays bounded");
    bool obsoletePublished = false;
    QObject::connect(&backend, &IRadioBackend::operatingStateChanged, [&] {
        obsoletePublished |= backend.currentOperatingState().sampleRateHz == 2'000'000;
    });
    state->releaseReadback();
    check(waitFor([&] { return backend.currentOperatingState().sampleRateHz == 1'536'000; }),
          "newest capture confirmed after compensation");
    check(!obsoletePublished, "superseded readback never published by backend");
    check(state->starts == 4, "initial, provisional, rollback and final captures each start exactly once");
    check(state->cancels >= 3, "each of the three hardware transitions cancels acquisition");
    const int startsBeforeReceiver = state->starts;
    const int cancelsBeforeReceiver = state->cancels;
    const int beforeFilter = changes;
    backend.setSliceFilter(0, -8000, 8000);
    check(waitFor([&] { state->block(); QThread::msleep(5); return changes > beforeFilter; }),
          "narrow filter accepted before selecting narrow demodulation");
    const int beforeMode = changes;
    backend.setSliceMode(0, QStringLiteral("FM"));
    check(waitFor([&] { state->block(); QThread::msleep(5); return changes > beforeMode; }), "mode adopted at callback boundary");
    check(state->starts == startsBeforeReceiver && state->cancels == cancelsBeforeReceiver,
          "mode-only change preserves USB acquisition");
    for (int i = 0; i < 16; ++i) { state->block(); }
    check(waitFor([&] { return audio > 0; }), "confirmed worker feeds existing PCM route");

    // A synchronous observer can submit an extension during publication. Its
    // promise belongs to that NEW transaction, not the just-completed one.
    bool injectedExtension = false;
    bool prematureExtension = false;
    bool extensionFinished = false;
    QObject::connect(&backend, &IRadioBackend::extensionResult, [&](quint64 id, const QVariant&) {
        if (id == 77) {
            std::lock_guard lock(state->mutex);
            prematureExtension = state->holdReadback;
            extensionFinished = true;
        }
    });
    QObject::connect(&backend, &IRadioBackend::operatingStateChanged, [&] {
        if (!injectedExtension) {
            injectedExtension = true;
            { std::lock_guard lock(state->mutex); state->holdReadback = true; }
            backend.invokeExtension(QStringLiteral("rtl"), QStringLiteral("ppm.set"), 77, 10);
        }
    });
    backend.setSliceFilter(0, -8000, 8000); state->block();
    check(waitFor([&] { state->block(); QThread::msleep(5); std::lock_guard lock(state->mutex); return state->inReadback; }),
          "reentrant extension reached its own readback");
    check(!prematureExtension && !extensionFinished, "extension cannot complete on an older publication");
    state->releaseReadback();
    check(waitFor([&] { return extensionFinished; }), "extension resolved after its own transaction");

    state->badReadback = true;
    backend.setPanBandwidth({}, 2'400'000);
    check(waitFor([&] { return !backend.isConnected(); }), "bad apply and rollback withdraw connection");
    check(probe.violations().isEmpty(), "backend seam signals stay on owner thread");
    QCoreApplication::processEvents();
    check(probe.afterDisconnect().isEmpty(), "no old worker emissions after disconnect");
    // A client may cancel synchronously from connected(). Initial deltas must
    // not escape after disconnected(), even though readback was successful.
    {
        auto device = std::make_shared<DeviceState>();
        rtl::RtlSdrBackend canceled;
        test::SeamThreadAffinityProbe cancelProbe(&canceled);
        test::attachAllSeamSignals(cancelProbe);
        bool sawConnect = false;
        QObject::connect(&canceled, &IRadioBackend::connected, [&] {
            sawConnect = true; canceled.disconnectRadio();
        });
        rtl::RtlCaptureBackendTestAccess::start(canceled, std::make_unique<InjectedDevice>(device));
        device->releaseReadback();
        check(waitFor([&] { return sawConnect; }), "reentrant connect cancellation reached");
        QCoreApplication::processEvents();
        check(cancelProbe.afterDisconnect().isEmpty(), "reentrant cancellation fences initial publication");
    }
    // Full backend slice lifecycle, with explicit offline test admission.
    // This does not raise the production architecture profile.
    {
        auto device = std::make_shared<DeviceState>();
        rtl::RtlSdrBackend multiple;
        QSet<int> live;
        QMap<int, PcmFrame> frames;
        QObject::connect(&multiple, &IRadioBackend::sliceChanged, [&](int id, const SliceDelta&) { live.insert(id); });
        QObject::connect(&multiple, &IRadioBackend::sliceRemoved, [&](int id) { live.remove(id); });
        QObject::connect(&multiple, &IRadioBackend::sliceAudioFrameReady, [&](int id, const PcmFrame& frame) { frames[id] = frame; });
        rtl::RtlCaptureBackendTestAccess::start(multiple, std::make_unique<InjectedDevice>(device), 4);
        device->releaseReadback();
        check(waitFor([&] { return multiple.isConnected(); }), "multi-receiver backend starts on accepted capture");
        const auto pump = [&] { device->block(); QThread::msleep(5); };
        check(multiple.createSlice({}, 100200000), "second receiver admitted in fixed capture");
        check(waitFor([&] { pump(); return live.contains(1); }), "second receiver publishes after preparation and adoption");
        check(multiple.createSlice({}, 99800000), "third receiver admitted");
        check(waitFor([&] { pump(); return live.contains(2) && frames.contains(0) && frames.contains(1); }), "independent native PCM reaches actual backend seam");
        const PcmFrame survivor = frames[0];
        const PcmFrame removed = frames[1];
        check(removed.stream().format.sampleRateHz == 48000, "upgraded backend publishes truthful 48 kHz format");
        check(multiple.removeSlice(1), "middle receiver removal requested");
        check(waitFor([&] { pump(); return !live.contains(1); }), "accepted middle removal preserves sparse IDs");
        check(!removed.current() && survivor.current(), "removal revokes old tap without resetting sibling producer");
        QThread::msleep(30); QCoreApplication::processEvents();
        check(multiple.createSlice({}, 100300000), "lowest retired slot can be reused");
        check(waitFor([&] { pump(); return live.contains(1) && frames[1].stream().receiverInstance != removed.stream().receiverInstance; }),
            "reused stable ID receives a fresh instance and cannot inherit stale audio");
        check(device->starts == 1 && device->cancels == 0, "add remove and reuse never restart USB acquisition");
        const PcmFrame reconnect = frames[0];
        multiple.disconnectRadio();
        check(!reconnect.current(), "disconnect revokes native PCM immediately");
    }
    // Cancellation cannot interrupt an in-progress device control. Timeout
    // must retain the device; its later thread exit must still retire it.
    {
        auto delayed = std::make_shared<DeviceState>();
        rtl::RtlSdrBackend stopped;
        test::SeamThreadAffinityProbe stoppedProbe(&stopped);
        test::attachAllSeamSignals(stoppedProbe);
        rtl::RtlCaptureBackendTestAccess::start(stopped, std::make_unique<InjectedDevice>(delayed));
        check(waitFor([&] { std::lock_guard lock(delayed->mutex); return delayed->inReadback; }),
              "delayed reader holds a device operation");
        stopped.disconnectRadio();
        check(!delayed->destroyed, "stop timeout never closes a live device");
        delayed->releaseReadback();
        check(waitFor([&] {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            return delayed->destroyed.load();
        }), "late reader exit retires its retained device");
        QCoreApplication::processEvents();
        check(stoppedProbe.afterDisconnect().isEmpty(), "late reader emits no retired seam state");
    }
    std::fprintf(stderr, "rtl_capture_worker_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
