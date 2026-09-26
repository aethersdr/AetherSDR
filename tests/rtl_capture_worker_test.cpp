// No socket or radio peer: inject device operations into the actual USB worker
// and drive the actual backend's publication path with controlled callbacks.
#include "core/backends/rtl/RtlSdrBackend.h"
#include "core/backends/rtl/RtlSdrWorker.h"
#include "SeamThreadAffinityProbe.h"
#include "RtlInjectedDevice.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QElapsedTimer>
#include <QEvent>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstring>
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
    static bool busy(const RtlSdrBackend& backend) { return backend.m_capture.busy(); }
    static T::State state(const RtlSdrBackend& backend) { return *backend.m_capture.confirmed(); }
    static void spectrum(RtlSdrBackend& backend, const QByteArray& frame, T::Token token)
    { emit backend.m_worker->spectrumFrameReady(token.session, token.revision, 0, frame); }
    static void waterfall(RtlSdrBackend& backend, const QByteArray& frame, T::Token token)
    { emit backend.m_worker->waterfallRowReady(token.session, token.revision, 0, frame); }
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
    // A malformed USB callback is an observable gap, even if capture geometry
    // does not move. The next display frame needs a wholly fresh observation.
    int gapFrames = 0;
    const auto gapConnection = QObject::connect(&backend, &IRadioBackend::spectrumFrameReady,
        [&](int, const QByteArray&) { ++gapFrames; });
    const auto clockBlocks = [&](int count) {
        const int expected = state->callbacks + count;
        for (int i = 0; i < count; ++i) { state->block(); }
        check(waitFor([&] { return state->callbacks >= expected; }), "controlled spectrum callbacks finish");
        QCoreApplication::processEvents();
    };
    clockBlocks(7);
    check(gapFrames == 0, "USB partial window is not padded into a frame");
    state->callbackBytes = 1;
    clockBlocks(1);
    state->callbackBytes = 16384;
    clockBlocks(7);
    check(gapFrames == 0, "malformed callback discards the previous partial spectrum");
    clockBlocks(1);
    check(waitFor([&] { return gapFrames == 1; }), "complete fresh USB window resumes display after gap");
    QObject::disconnect(gapConnection);
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
    backend.invokeExtension(QStringLiteral("rtl"), QStringLiteral("sample_rate.set"), 1000, 2'000'000);
    check(waitFor([&] { std::lock_guard lock(state->mutex); return state->inReadback; }), "rate change quiesced");
    check(backend.currentOperatingState().sampleRateHz == 2'400'000,
          "pending rate does not reach OperatingState");
    for (int i = 0; i < 100; ++i) {
        backend.invokeExtension(QStringLiteral("rtl"), QStringLiteral("sample_rate.set"), 1001 + i, 1'536'000);
    }
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
    check(changes == beforeFilter && !rtl::RtlCaptureBackendTestAccess::busy(backend),
          "unimplemented WFM filter request neither publishes nor queues work");
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
    backend.invokeExtension(QStringLiteral("rtl"), QStringLiteral("sample_rate.set"), 2000, 2'400'000);
    check(waitFor([&] { return !backend.isConnected(); }), "bad apply and rollback withdraw connection");
    check(probe.violations().isEmpty(), "backend seam signals stay on owner thread");
    QCoreApplication::processEvents();
    check(probe.afterDisconnect().isEmpty(), "no old worker emissions after disconnect");
    // A legacy demodulator can have a partial PCM batch when its receiver
    // parks without changing hardware. Resuming must start a fresh audio
    // epoch while the capture FFT remains available.
    {
        rtl::RtlSdrDdc ddc;
        ddc.applyCapture(2'400'000, 100'000'000, 100'000'000, T::Mode::Am, -8000, 8000);
        int emitted = 0;
        QByteArray resumedTap;
        QObject::connect(&ddc, &rtl::RtlSdrDdc::audioFrameReady,
            [&](const QByteArray&, const QByteArray& tap) {
                ++emitted;
                if (emitted == 2) { resumedTap = tap; }
            });
        const QVector<std::complex<float>> oldIq(8192, {0.6f, 0.0f});
        const QVector<std::complex<float>> newIq(8192, {0.3f, 0.0f});
        ddc.processIqData(oldIq);
        ddc.processIqData(oldIq);
        check(emitted == 1, "legacy DDC holds a partial second PCM batch before parking");
        ddc.resetReceiveAudio();
        ddc.processIqData(newIq);
        check(emitted == 2 && !resumedTap.isEmpty(),
            "legacy resume emits a fresh batch without waiting for pre-park PCM");
        float peak = 0;
        for (int offset = 0; offset + static_cast<int>(sizeof(float)) <= resumedTap.size();
             offset += static_cast<int>(sizeof(float))) {
            float sample = 0;
            std::memcpy(&sample, resumedTap.constData() + offset, sizeof(sample));
            peak = std::max(peak, std::abs(sample));
        }
        check(peak < 0.01f, "resumed legacy PCM excludes pre-park samples and demodulator history");
    }
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
    {
        auto device = std::make_shared<DeviceState>();
        rtl::RtlSdrBackend legacy;
        int speakerPackets = 0;
        QObject::connect(&legacy, &IRadioBackend::audioFrameReady,
            [&](const PcmFrame&) { ++speakerPackets; });
        rtl::RtlCaptureBackendTestAccess::start(legacy, std::make_unique<InjectedDevice>(device));
        device->releaseReadback();
        check(waitFor([&] { return legacy.isConnected(); }), "legacy receiver confirms initial capture");
        legacy.setSliceMode(0, QStringLiteral("AM"));
        check(waitFor([&] { device->block(); return !rtl::RtlCaptureBackendTestAccess::busy(legacy)
            && rtl::RtlCaptureBackendTestAccess::state(legacy).receivers.front().mode == T::Mode::Am; }),
            "legacy AM receiver adopts without changing hardware");
        legacy.setSliceFilter(0, -50'000, 50'000);
        check(waitFor([&] { device->block(); return !rtl::RtlCaptureBackendTestAccess::busy(legacy)
            && rtl::RtlCaptureBackendTestAccess::state(legacy).receivers.front().passband.filterHighHz == 50'000; }),
            "legacy filter narrows at accepted capture");
        legacy.setSliceFrequency(0, 101'000'000);
        check(waitFor([&] { device->block(); return !rtl::RtlCaptureBackendTestAccess::busy(legacy)
            && rtl::RtlCaptureBackendTestAccess::state(legacy).receivers.front().passband.carrierHz == 101'000'000; }),
            "legacy slice tunes near the usable capture edge");
        check(waitFor([&] { device->block(); return speakerPackets > 0; }),
            "legacy receiver emits PCM before parking");
        const int beforePartial = device->callbacks;
        device->block(); // leave a partial legacy PCM batch behind the first emission
        check(waitFor([&] { return device->callbacks > beforePartial; }),
            "legacy receiver processed a partial pre-park PCM block");
        const auto fixedHardware = rtl::RtlCaptureBackendTestAccess::state(legacy).hardware;
        legacy.setSliceFilter(0, -50'000, 100'000);
        const bool legacyParked = waitFor([&] { device->block(); return !rtl::RtlCaptureBackendTestAccess::busy(legacy)
            && rtl::RtlCaptureBackendTestAccess::state(legacy).receivingIds.empty(); });
        if (!legacyParked) {
            const auto debugState = rtl::RtlCaptureBackendTestAccess::state(legacy);
            std::fprintf(stderr, "legacy park center=%.0f rf=%.0f filter=[%.0f,%.0f] receiving=%zu busy=%d\n",
                debugState.capture.centerHz, debugState.receivers.front().passband.carrierHz,
                debugState.receivers.front().passband.filterLowHz,
                debugState.receivers.front().passband.filterHighHz,
                debugState.receivingIds.size(), rtl::RtlCaptureBackendTestAccess::busy(legacy));
        }
        check(legacyParked,
            "filter-only edge crossing parks legacy receive without moving hardware");
        const int atPark = speakerPackets;
        for (int i = 0; i < 12; ++i) { device->block(); QCoreApplication::processEvents(); }
        QThread::msleep(20); QCoreApplication::processEvents();
        check(speakerPackets == atPark, "parked legacy receiver emits no PCM");
        legacy.setSliceFilter(0, -50'000, 50'000);
        const bool legacyResumed = waitFor([&] { device->block(); return !rtl::RtlCaptureBackendTestAccess::busy(legacy)
            && rtl::RtlCaptureBackendTestAccess::state(legacy).receivingIds.size() == 1; });
        if (!legacyResumed) {
            const auto debugState = rtl::RtlCaptureBackendTestAccess::state(legacy);
            std::fprintf(stderr, "legacy resume center=%.0f rf=%.0f filter=[%.0f,%.0f] receiving=%zu busy=%d\n",
                debugState.capture.centerHz, debugState.receivers.front().passband.carrierHz,
                debugState.receivers.front().passband.filterLowHz,
                debugState.receivers.front().passband.filterHighHz,
                debugState.receivingIds.size(), rtl::RtlCaptureBackendTestAccess::busy(legacy));
        }
        check(legacyResumed,
            "legacy receiver resumes after its full passband fits again");
        check(waitFor([&] { device->block(); return speakerPackets > atPark; }),
            "resumed legacy receiver emits new PCM");
        check(rtl::RtlCaptureBackendTestAccess::state(legacy).receivers.front().passband.carrierHz
                == 101'000'000
                && rtl::RtlCaptureBackendTestAccess::state(legacy).hardware == fixedHardware
                && device->starts == 1 && device->cancels == 0,
            "legacy park and resume preserve slice RF, hardware and USB acquisition");
    }
    // Full backend slice lifecycle, with explicit offline test admission.
    // This does not raise the production architecture profile.
    {
        auto device = std::make_shared<DeviceState>();
        rtl::RtlSdrBackend multiple;
        QSet<int> live;
        QMap<int, PcmFrame> frames;
        int sliceAudioPackets = 0, spectra = 0, waterfalls = 0;
        QObject::connect(&multiple, &IRadioBackend::sliceChanged, [&](int id, const SliceDelta&) { live.insert(id); });
        QObject::connect(&multiple, &IRadioBackend::sliceRemoved, [&](int id) { live.remove(id); });
        QObject::connect(&multiple, &IRadioBackend::sliceAudioFrameReady, [&](int id, const PcmFrame& frame) {
            frames[id] = frame; ++sliceAudioPackets;
        });
        QObject::connect(&multiple, &IRadioBackend::spectrumFrameReady, [&](int, const QByteArray&) { ++spectra; });
        QObject::connect(&multiple, &IRadioBackend::waterfallRowReady, [&](int, const QByteArray&) { ++waterfalls; });
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
        // Dragging a full-width view away from every receiver moves capture,
        // but keeps each configured RF frequency and slice identity intact.
        check(waitFor([&] { pump(); return frames.contains(0) && frames[0].current(); }),
            "active sibling has a current PCM stream before free pan");
        const T::State beforePark = rtl::RtlCaptureBackendTestAccess::state(multiple);
        const PcmFrame activeFrame = frames[0];
        multiple.setPanCenter(QStringLiteral("0xe1000000"), 104'000'000,
            IRadioBackend::PanCenterIntent::Drag);
        check(waitFor([&] { return !rtl::RtlCaptureBackendTestAccess::busy(multiple)
            && rtl::RtlCaptureBackendTestAccess::state(multiple).receivingIds.empty(); }),
            "free pan confirms an empty receiving bank outside all guarded passbands");
        const T::State parked = rtl::RtlCaptureBackendTestAccess::state(multiple);
        check(parked.receivers == beforePark.receivers && live.contains(0) && live.contains(1)
            && live.contains(2) && parked.hardware.centerHz != beforePark.hardware.centerHz,
            "parked slices preserve configured RF, IDs and settings across capture retune");
        check(!activeFrame.current(), "park revokes the previously published native PCM stream");
        const int audioAtPark = sliceAudioPackets;
        const int iqSpectraAtPark = spectra, iqWaterfallsAtPark = waterfalls;
        for (int i = 0; i < 64; ++i) { pump(); QCoreApplication::processEvents(); }
        QThread::msleep(20); QCoreApplication::processEvents();
        check(sliceAudioPackets == audioAtPark,
            "IQ callbacks with every slice parked publish no slice PCM");
        check(spectra > iqSpectraAtPark && waterfalls > iqWaterfallsAtPark,
            "parked IQ callbacks continue producing spectrum and waterfall rows");
        const QByteArray raw(rtl::RtlSdrDdc::kSpectrumBinCount * static_cast<int>(sizeof(float)), '\0');
        const int spectraAtPark = spectra, waterfallsAtPark = waterfalls;
        rtl::RtlCaptureBackendTestAccess::spectrum(multiple, raw, parked.token);
        rtl::RtlCaptureBackendTestAccess::waterfall(multiple, raw, parked.token);
        check(spectra == spectraAtPark + 1 && waterfalls == waterfallsAtPark + 1,
            "current parked-capture token admits spectrum and waterfall frames");
        rtl::RtlCaptureBackendTestAccess::spectrum(multiple, raw, beforePark.token);
        rtl::RtlCaptureBackendTestAccess::waterfall(multiple, raw, beforePark.token);
        check(spectra == spectraAtPark + 1 && waterfalls == waterfallsAtPark + 1,
            "pre-park FFT and waterfall revisions cannot leak into the parked view");
        multiple.setPanCenter(QStringLiteral("0xe1000000"), 100'000'000,
            IRadioBackend::PanCenterIntent::Drag);
        check(waitFor([&] { return !rtl::RtlCaptureBackendTestAccess::busy(multiple)
            && rtl::RtlCaptureBackendTestAccess::state(multiple).receivingIds.size() == 3; }),
            "return drag resumes all configured receivers automatically");
        const T::State resumed = rtl::RtlCaptureBackendTestAccess::state(multiple);
        check(resumed.receivers == beforePark.receivers && resumed.token != parked.token,
            "resume preserves RF settings and publishes a new capture revision");
        check(waitFor([&] { pump(); return sliceAudioPackets > audioAtPark && frames[0].current(); }),
            "resumed receiver produces fresh native PCM");
        const int spectraAtResume = spectra, waterfallsAtResume = waterfalls;
        rtl::RtlCaptureBackendTestAccess::spectrum(multiple, raw, parked.token);
        rtl::RtlCaptureBackendTestAccess::waterfall(multiple, raw, parked.token);
        check(spectra == spectraAtResume && waterfalls == waterfallsAtResume,
            "parked-capture frames cannot leak after automatic resume");
        // Deliberately stop servicing the owner while the injected acquisition
        // produces real FM packets. This is queue saturation, not a fake counter.
        const int targetCallbacks = device->callbacks + 200;
        { std::lock_guard lock(device->mutex); device->blocks += 200; device->changed.notify_all(); }
        QElapsedTimer stalledOwner; stalledOwner.start();
        while (device->callbacks < targetCallbacks && stalledOwner.elapsed() < 3000) { QThread::msleep(1); }
        check(device->callbacks >= targetCallbacks, "injected acquisition reaches bounded queue saturation");
        check(waitFor([&] {
            return multiple.healthSnapshot().values.value("rtlQueueDrops").toULongLong() > 0;
        }), "actual pipeline drop counter reaches existing backend health diagnostics");
        const auto health = multiple.healthSnapshot();
        check(health.values.contains("rtlMixerLateFrames") && health.values.contains("rtlMixerRejectedBlocks")
            && health.values.value("rtlMixerConfigurationFailures").toULongLong() == 0,
            "observed mixer diagnostics have explicit values without an invariant failure");
        const PcmFrame reconnect = frames[0];
        bool invalidationObserved = false;
        bool reentrantCreate = true;
        QObject::connect(&multiple, &IRadioBackend::connectionError, [&] {
            invalidationObserved = multiple.isConnected();
            reentrantCreate = multiple.createSlice({}, 100400000);
        });
        device->badReadback = true;
        multiple.invokeExtension(QStringLiteral("rtl"), QStringLiteral("sample_rate.set"), 3000, 2000000);
        check(waitFor([&] { return !multiple.isConnected(); }), "multi-receiver capture invalidation disconnects");
        check(invalidationObserved && !reentrantCreate,
            "reentrant creation refuses the invalidated capture before disconnect notification");
        check(!reconnect.current(), "disconnect revokes native PCM immediately");
        check(multiple.healthSnapshot().isEmpty(), "disconnected health cannot report stale live counters");
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
