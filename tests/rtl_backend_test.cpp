// Unit test for RtlSdrBackend and RtlSdrDiscovery (Issue #4797).
// Verifies capabilities declaration, state restore contract, signal deltas,
// and discovery availability.

#include "core/backends/rtl/RtlSdrBackend.h"
#include "core/backends/rtl/RtlSdrDdc.h"
#include "core/backends/rtl/RtlSdrWorker.h"
#include "core/RtlSdrDiscovery.h"
#include "SeamThreadAffinityProbe.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QJsonObject>
#include <QPointer>
#include <QThread>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <numbers>
#include <cmath>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

#ifdef AETHER_BACKEND_RTL
namespace AetherSDR::rtl {

// Installs a worker the way connectRadio() does, without a dongle.
struct RtlSdrBackendTestAccess {
    static void connectWith(RtlSdrBackend& backend, RtlSdrWorker* worker)
    {
        using T = RtlCaptureTransaction;
        backend.m_capture.beginSession();
        T::Desired desired;
        desired.receivers = {{{0, 95'200'000, -100'000, 100'000, 0, 0, 0}, T::Mode::Wfm}};
        const auto submitted = backend.m_capture.submit(desired);
        const auto work = backend.m_capture.takeWork();
        if (!submitted || !work) { std::abort(); }
        backend.m_capture.complete({work->token, T::ResultCode::Applied, work->target, work->operation});
        backend.m_published = work->token;
        backend.m_viewport = RtlViewport::fit(work->target.capture, RtlSdrDdc::kSpectrumBinCount,
            work->target.capture.centerHz, work->target.capture.achievedSampleRateHz);
        workerToken = work->token;
        backend.m_worker.reset(worker);
        backend.wireWorker();
        worker->start();
        backend.m_connected = true;
    }
    // The same wiring with the connected flag still false.
    static void wireOnly(RtlSdrBackend& backend, RtlSdrWorker* worker)
    {
        backend.m_worker.reset(worker);
        backend.wireWorker();
        worker->start();
    }
    static inline RtlCaptureTransaction::Token workerToken;
};

}  // namespace AetherSDR::rtl

namespace {

// A USB reader that ignores cancellation, as a wedged libusb stack does, so
// stopReading() gives up and disconnectRadio() strands it. It emits spectrum
// from its own thread through the DDC, the production path.
class StuckWorker : public rtl::RtlSdrWorker {
public:
    StuckWorker() : RtlSdrWorker(nullptr) {}
    std::atomic<int> framesToEmit{0};
    std::atomic<bool> release{false};
    std::atomic<bool> failRead{false};
    std::atomic<bool> readFailed{false};

protected:
    void run() override
    {
        while (!release.load()) {
            if (failRead.exchange(false)) {
                emit readError(QStringLiteral("simulated USB read failure"));
                readFailed = true;
            }
            while (framesToEmit.load() > 0) {
                --framesToEmit;
                const auto token = rtl::RtlSdrBackendTestAccess::workerToken;
                emit spectrumFrameReady(token.session, token.revision, 0,
                    QByteArray(rtl::RtlSdrDdc::kSpectrumBinCount * int(sizeof(float)), '\x01'));
            }
            QThread::msleep(5);
        }
    }
};

bool spinUntil(const std::function<bool()>& done, int ms)
{
    QDeadlineTimer deadline(ms);
    while (!done() && !deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    return done();
}

}  // namespace
#endif

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

#ifdef AETHER_BACKEND_RTL
    auto backend = std::make_unique<rtl::RtlSdrBackend>();
    check(backend != nullptr, "RtlSdrBackend instantiation");

    // 1. Check capabilities declaration (Principle VI: receive-only)
    const auto caps = backend->capabilities();
    check(!caps.canCreateSlices, "RTL-SDR retains its fixed single receiver in P01");
    check(caps.family == "rtl", "capabilities.family is rtl");
    check(!caps.canTransmit, "RTL-SDR cannot transmit");
    check(caps.txPowerMaxWatts == 0.0, "RTL-SDR max TX power is 0");
    check(!caps.hostModulates, "RTL-SDR does not modulate on host");
    check(caps.maxSlices == 1, "RTL-SDR maxSlices is 1");
    check(caps.maxPanadapters == 1, "RTL-SDR maxPanadapters is 1");
    check(!caps.persistsMemories, "RTL-SDR does not persist memories on device");

    // Check ClientSettingsDomains
    check(caps.clientSettingsDomains.testFlag(RadioCapabilities::ClientSettingsDomain::Tuning),
          "ClientSettingsDomain::Tuning declared");
    check(caps.clientSettingsDomains.testFlag(RadioCapabilities::ClientSettingsDomain::RfGain),
          "ClientSettingsDomain::RfGain declared");

    // 2. Test state restoration (RFC #4603 typed restore contract)
    RestoredRadioState restoredState;
    restoredState.rfFrequencyHz = 144'200'000.0;
    restoredState.mode = QStringLiteral("USB");
    restoredState.filterLowHz = 300;
    restoredState.filterHighHz = 2'700;
    restoredState.sampleRateHz = 1'843'200;
    restoredState.extension[QStringLiteral("rfGain")] =
        QJsonObject{{QStringLiteral("gainDb"), 28}};

    backend->applyRestoredState(restoredState);

    // Save state back out and verify
    const auto savedState = backend->currentOperatingState();
    check(savedState.rfFrequencyHz == 144'200'000.0, "restored rfFrequencyHz");
    check(savedState.mode == QStringLiteral("USB"), "restored mode");
    check(savedState.sampleRateHz == 1'843'200, "restored sampleRateHz");
    check(savedState.filterLowHz == 300 && savedState.filterHighHz == 2'700,
          "restored passband");
    check(savedState.extension.contains("rfGain"), "saved state contains rfGain extension");
    const auto savedExt = savedState.extension.value("rfGain").toObject();
    check(savedExt.value("gainDb").toInt() == 28, "restored gainDb");

    // 3. Test RtlSdrDiscovery static check
    check(RtlSdrDiscovery::isAvailable(), "RtlSdrDiscovery::isAvailable() is true when built with librtlsdr");

    // 4. Test RtlSdrDdc processing & signal emissions
    rtl::RtlSdrDdc ddc;
    ddc.setSampleRate(2'400'000.0);
    ddc.setCenterFrequency(144'200'000.0);
    ddc.setSliceFrequency(144'200'000.0);
    ddc.setSliceMode(QStringLiteral("FM"));

    bool spectrumEmitted = false;
    bool audioEmitted = false;

    QObject::connect(&ddc, &rtl::RtlSdrDdc::spectrumFrameReady, [&spectrumEmitted](int panId, const QByteArray& frame) {
        Q_UNUSED(panId);
        if (!frame.isEmpty()) {
            spectrumEmitted = true;
        }
    });

    QObject::connect(&ddc, &rtl::RtlSdrDdc::audioFrameReady, [&audioEmitted](const QByteArray& pcm) {
        if (!pcm.isEmpty()) {
            audioEmitted = true;
        }
    });

    // Feed 4096 synthetic complex float IQ samples
    QVector<std::complex<float>> syntheticSamples(4096, std::complex<float>(0.5f, 0.5f));
    // A display frame now requires a complete continuous observation.
    for (int i = 0; i < rtl::RtlSdrDdc::kSpectrumBinCount / syntheticSamples.size(); ++i) {
        ddc.processIqData(syntheticSamples);
    }

    check(spectrumEmitted, "RtlSdrDdc emitted spectrumFrameReady");
    check(audioEmitted, "RtlSdrDdc emitted audioFrameReady");

    // WFM keeps its existing demodulation. Its independent tap must remain
    // identical while monitor mute clocks silence at the same 24 kHz rate.
    {
        rtl::RtlSdrDdc audible, muted;
        muted.setAudioMute(true); muted.setAudioGain(0);
        QByteArray reference, tap, silent;
        QObject::connect(&audible, &rtl::RtlSdrDdc::audioFrameReady,
            [&](const QByteArray&, const QByteArray& preMonitor) { reference.append(preMonitor); });
        QObject::connect(&muted, &rtl::RtlSdrDdc::audioFrameReady,
            [&](const QByteArray& monitor, const QByteArray& preMonitor) { silent.append(monitor); tap.append(preMonitor); });
        QVector<std::complex<float>> samples(8192);
        for (int block = 0; block < 32; ++block) {
            for (int i = 0; i < samples.size(); ++i) {
                const double t = (block * samples.size() + i) / 2400000.0;
                const double phase = 50 * std::sin(2 * std::numbers::pi * 1000 * t);
                samples[i] = {float(0.5 * std::cos(phase)), float(0.5 * std::sin(phase))};
            }
            audible.processIqData(samples); muted.processIqData(samples);
        }
        check(!reference.isEmpty() && reference == tap, "WFM pre-monitor samples unchanged by mute/gain");
        check(silent.size() == tap.size(), "muted WFM keeps truthful PCM duration");
        check(std::ranges::all_of(silent, [](char byte) { return byte == 0; }), "muted WFM monitor is silence");
    }

    // 5. Test direct sampling mode persistence contract
    RestoredRadioState hfState;
    hfState.rfFrequencyHz = 14'100'000.0;
    hfState.mode = QStringLiteral("USB");
    backend->applyRestoredState(hfState);
    const auto hfSaved = backend->currentOperatingState();
    check(hfSaved.rfFrequencyHz == 14'100'000.0, "HF state restored frequency");

    // Empty restore is a real reset, not a same-family state leak.
    backend->applyRestoredState({});
    const auto resetState = backend->currentOperatingState();
    check(resetState.rfFrequencyHz == 95'200'000.0, "empty restore resets frequency");
    check(resetState.mode == QStringLiteral("WFM"), "empty restore resets mode");
    check(resetState.sampleRateHz == 2'400'000, "empty restore resets sample rate");
    // The reset gain is the DEFAULT, and the default is deliberately not zero.
    //
    // 0 was the shipped value and it is the one number that must never come back:
    // both tuner families start their discrete gain table at exactly 0.0 dB, so
    // nearestGainTenths() snapped a 0 default onto the LOWEST gain the hardware has
    // and an unconfigured dongle came up deaf. Pinned as a literal rather than
    // written against the constant, so moving the constant to 0 fails here instead
    // of silently agreeing with itself.
    static_assert(rtl::RtlSdrBackend::kDefaultRfGainDb != 0,
                  "a 0 dB default programs the tuner's lowest gain, not 'unset'");
    check(resetState.extension.value("rfGain").toObject().value("gainDb").toInt()
              == rtl::RtlSdrBackend::kDefaultRfGainDb,
          "empty restore resets gain to the default");
    check(resetState.extension.value("rfGain").toObject().value("gainDb").toInt() != 0,
          "the reset gain is not the deaf-on-connect 0 dB the backend shipped with");

    // 6. Test sample rate validation & clamping (Priority 4)
    check(rtl::RtlSdrBackend::clampSampleRate(0) == 225'001u, "clampSampleRate(0) -> 225001");
    check(rtl::RtlSdrBackend::clampSampleRate(500'000u) == 300'000u, "forbidden-gap 500k -> 300k");
    check(rtl::RtlSdrBackend::clampSampleRate(768'000u) == 1'000'000u, "forbidden-gap 768k -> 1M");
    check(rtl::RtlSdrBackend::clampSampleRate(10'000'000u) == 3'000'000u, "clampSampleRate(10M) -> 3000000");
    check(rtl::RtlSdrBackend::clampSampleRate(2'400'000u) == 2'400'000u, "clampSampleRate(2.4M) -> 2400000");
    check(rtl::RtlSdrBackend::clampSampleRate(2'500'000u) == 2'400'000u, "clampSampleRate(2.5M) -> 2400000");

    // 7. Contract rule 6 (#6096): a worker stranded by a failed stopReading()
    // keeps running and stays connected to the backend. Its spectrum must not
    // reach the seam after disconnected().
    {
        rtl::RtlSdrBackend stranding;
        test::SeamThreadAffinityProbe probe(&stranding);
        test::attachAllSeamSignals(probe);
        auto* worker = new StuckWorker;
        QPointer<StuckWorker> alive(worker);
        rtl::RtlSdrBackendTestAccess::connectWith(stranding, worker);

        worker->framesToEmit = 1;
        check(spinUntil([&] { return probe.count("spectrumFrameReady") == 1; }, 2000),
              "a connected worker's spectrum reaches the seam");

        // A read failure posted to the backend just before the operator
        // disconnects is delivered after disconnected(); it must not surface.
        worker->failRead = true;
        QDeadlineTimer errorDeadline(2000);
        while (!worker->readFailed.load() && !errorDeadline.hasExpired()) {
            QThread::msleep(1);
        }
        check(worker->readFailed.load(), "the worker queues the read error within the deadline");

        stranding.disconnectRadio();   // stopReading() gives up after ~5 s
        check(alive && worker->isRunning(), "the stuck worker was stranded, not joined");
        check(probe.count("disconnected") == 1, "disconnectRadio() emitted disconnected()");

        worker->framesToEmit = 3;
        spinUntil([&] { return worker->framesToEmit.load() == 0; }, 2000);
        spinUntil([] { return false; }, 200);   // deliver whatever was queued
        check(probe.count("connectionError") == 0,
              "a read error queued before the disconnect is not reported after it");
        check(probe.afterDisconnect().isEmpty(),
              "a stranded worker emits nothing at the seam after disconnected()");
        check(probe.violations().isEmpty(), "every seam signal emitted on the backend thread");

        worker->release = true;
        check(spinUntil([&] { return alive.isNull(); }, 2000),
              "the stranded worker deletes itself once its thread finishes");
    }

    // 8. The connected flag gates on its own: a current worker's output is
    // dropped while the backend is not connected.
    {
        rtl::RtlSdrBackend unconnected;
        test::SeamThreadAffinityProbe probe(&unconnected);
        test::attachAllSeamSignals(probe);
        auto* worker = new StuckWorker;
        rtl::RtlSdrBackendTestAccess::wireOnly(unconnected, worker);

        worker->framesToEmit = 1;
        worker->failRead = true;
        spinUntil([&] { return worker->readFailed.load() && worker->framesToEmit.load() == 0; },
                  2000);
        spinUntil([] { return false; }, 200);   // deliver whatever was queued
        check(probe.count("spectrumFrameReady") == 0,
              "a current worker's spectrum is dropped while not connected");
        check(probe.count("connectionError") == 0,
              "a current worker's read error is dropped while not connected");

        worker->release = true;
        check(worker->wait(2000), "the unconnected worker's thread finishes");
    }
#else

    std::fprintf(stderr, "rtl_backend_test: SKIPPED (librtlsdr support disabled)\n");
    return 0;
#endif

    if (g_failures > 0) {
        std::fprintf(stderr, "rtl_backend_test: %d checks failed\n", g_failures);
        return 1;
    }

    std::fprintf(stderr, "rtl_backend_test: all checks passed\n");
    return 0;
}
