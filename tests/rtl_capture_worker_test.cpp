// No socket or radio peer: inject device operations into the actual USB worker
// and drive the actual backend's publication path with controlled callbacks.
#include "core/backends/rtl/RtlSdrBackend.h"
#include "core/backends/rtl/RtlSdrWorker.h"
#include "SeamThreadAffinityProbe.h"

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
    static void start(RtlSdrBackend& backend, std::unique_ptr<RtlSdrWorker::Device> device)
    {
        backend.m_requested.hardware = {100'000'000, 2'400'000, 0, 0, 0, 240};
        backend.m_requested.receivers = {{{0, 100'000'000, -100'000, 100'000, 0, 0, 0}, T::Mode::Wfm}};
        backend.startCapture(std::make_unique<RtlSdrWorker>(std::move(device)));
    }
    static std::size_t pending(const RtlSdrBackend& backend) { return backend.m_capture.pendingCount(); }
};
}
static int failures = 0;
static void check(bool value, const char* message)
{
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
struct DeviceState {
    std::mutex mutex;
    std::condition_variable changed;
    bool canceled = false;
    bool holdReadback = true;
    bool inReadback = false;
    int blocks = 0;
    std::atomic<int> starts{0}, cancels{0}, writes{0}, callbacks{0};
    std::atomic<bool> badReadback{false};
    std::atomic<bool> destroyed{false};
    T::Hardware hardware;
    void releaseReadback()
    {
        std::lock_guard lock(mutex); holdReadback = false; changed.notify_all();
    }
    void block()
    {
        std::lock_guard lock(mutex); ++blocks; changed.notify_all();
    }
};
class InjectedDevice final : public rtl::RtlSdrWorker::Device {
public:
    explicit InjectedDevice(std::shared_ptr<DeviceState> state) : m_state(std::move(state)) {}
    ~InjectedDevice() override { m_state->destroyed = true; }
    bool set(T::Control control, std::int64_t value) override
    {
        ++m_state->writes;
        switch (control) {
        case T::Control::DirectSampling: m_state->hardware.directSampling = int(value); break;
        case T::Control::SampleRate: m_state->hardware.sampleRateHz = std::uint32_t(value); break;
        case T::Control::Ppm: m_state->hardware.ppm = int(value); break;
        case T::Control::OffsetTuning: m_state->hardware.offsetTuning = int(value); break;
        case T::Control::Center: m_state->hardware.centerHz = std::uint32_t(value); break;
        case T::Control::Gain: m_state->hardware.gainTenths = int(value); break;
        }
        return true;
    }
    std::optional<T::Hardware> read() override
    {
        std::unique_lock lock(m_state->mutex);
        m_state->inReadback = true;
        m_state->changed.notify_all();
        m_state->changed.wait(lock, [&] { return !m_state->holdReadback; });
        m_state->inReadback = false;
        auto actual = m_state->hardware;
        if (m_state->badReadback) { actual.sampleRateHz = 0; }
        return actual;
    }
    bool resetBuffer() override { return true; }
    int readAsync(Callback callback, void* context) override
    {
        std::unique_lock lock(m_state->mutex);
        m_state->canceled = false;
        ++m_state->starts;
        std::array<unsigned char, 16384> iq;
        iq.fill(130);
        while (!m_state->canceled) {
            m_state->changed.wait(lock, [&] { return m_state->canceled || m_state->blocks > 0; });
            if (m_state->canceled) { break; }
            --m_state->blocks;
            lock.unlock();
            callback(iq.data(), static_cast<std::uint32_t>(iq.size()), context);
            ++m_state->callbacks;
            lock.lock();
        }
        return 0;
    }
    void cancelAsync() override
    {
        ++m_state->cancels;
        std::lock_guard lock(m_state->mutex);
        m_state->canceled = true;
        m_state->changed.notify_all();
    }
private:
    std::shared_ptr<DeviceState> m_state;
};
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
    check(waitFor([&] { return changes > initialChanges; }), "in-window receiver adoption acknowledged");
    check(state->starts == 1 && state->cancels == 0 && state->writes == 6,
          "receiver-only change never cancels/restarts/writes USB");
    const int noOpChanges = changes;
    backend.setSliceFrequency(0, 100'200'000); state->block();
    check(waitFor([&] { return changes > noOpChanges; }), "no-op acknowledged");
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
    const int beforeMode = changes;
    backend.setSliceMode(0, QStringLiteral("FM")); state->block();
    check(waitFor([&] { return changes > beforeMode; }), "mode adopted at callback boundary");
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
    check(waitFor([&] { std::lock_guard lock(state->mutex); return state->inReadback; }),
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
