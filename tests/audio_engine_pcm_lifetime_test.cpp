// RFC #5468 A2: the real AudioEngine auxiliary ingress/retirement methods
// overlap its production asynchronous DSP initializer. No sink, audio device,
// socket, radio transport, hardware or transmitter is opened. Run under TSan
// for the lifetime claim; the native checks alone cannot establish race freedom.
#include "TestSettingsProfile.h"
#include "core/AudioEngine.h"
#include "core/ClientComp.h"
#include "core/DeepFilterFilter.h"
#include "core/SpecbleachFilter.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QJsonArray>
#include <QTimer>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>

namespace AetherSDR {

// Same friend seam as audio_engine_rates_test, in a separate executable.
class AudioEngineRatesTestAccess {
public:
    static void stopTimer(AudioEngine& engine)
    {
        engine.m_rxTimer->stop();
    }

    static void waitInitialization(AudioEngine& engine)
    {
        engine.m_dspInitializationTasks.waitForFinished();
    }

    static void enableSource(AudioEngine& engine, const QString& id)
    {
        std::lock_guard<std::recursive_mutex> lock(engine.m_dspMutex);
        AudioEngine::ExternalRxAudioSourceState* source = engine.externalKiwiSource(id, true);
        source->enabled = true;
    }

    static void clearSourceDsp(AudioEngine& engine, const QString& id)
    {
        std::lock_guard<std::recursive_mutex> lock(engine.m_dspMutex);
        AudioEngine::ExternalRxAudioSourceState* source = engine.externalKiwiSource(id, false);
        if (source) {
            engine.clearExternalKiwiDspState(*source);
        }
    }

    static bool initializeSource(AudioEngine& engine, const QString& id)
    {
        return engine.ensureExternalKiwiSourceDspState(id);
    }

    static bool retireAndCheck(AudioEngine& engine, const QString& id)
    {
        // Do not lock around retirement here: the production method itself
        // must serialize its filter reset against the initializer's install.
        const bool retired = engine.retireInvalidPcmSources();
        std::lock_guard<std::recursive_mutex> lock(engine.m_dspMutex);
        AudioEngine::ExternalRxAudioSourceState* source = engine.externalKiwiSource(id, false);
        return retired && source && !source->pcmFrame
            && source->rxBuffer.isEmpty() && source->rxPackets.empty()
            && source->outputBuffer.isEmpty() && source->prebuffering;
    }

    static bool sourcePrepared(AudioEngine& engine, const QString& id)
    {
        std::lock_guard<std::recursive_mutex> lock(engine.m_dspMutex);
        const AudioEngine::ExternalRxAudioSourceState* source = engine.externalKiwiSource(id, false);
        return source && source->rn2 && !source->dspInitializationPending;
    }

    static void attachMemoryOutput(AudioEngine& engine, QBuffer& output)
    {
        engine.m_audioDevice = &output;
        engine.setRxDeviceRate(24000);
    }

    static void detachMemoryOutput(AudioEngine& engine)
    {
        engine.m_audioDevice = nullptr;
    }

    static void processSource(AudioEngine& engine, const QString& id, const QByteArray& pcm)
    {
        std::lock_guard<std::recursive_mutex> lock(engine.m_dspMutex);
        engine.processMixedRxAudioData(pcm, AudioEngine::RxDspSource::KiwiSdr,
                                      engine.externalKiwiSource(id, true));
    }

    static void processMain(AudioEngine& engine, const QByteArray& pcm)
    {
        engine.processMixedRxAudioData(pcm, AudioEngine::RxDspSource::Main);
    }

    static qsizetype queuedKiwiBytes(AudioEngine& engine, const QString& id)
    {
        std::lock_guard<std::recursive_mutex> lock(engine.m_dspMutex);
        if (id.isEmpty()) {
            qsizetype bytes = engine.m_kiwiSdrRxBuffer.size() + engine.m_kiwiSdrOutputBuffer.size();
            for (const QByteArray& packet : engine.m_kiwiSdrRxPackets) {
                bytes += packet.size();
            }
            return bytes;
        }
        const AudioEngine::ExternalRxAudioSourceState* source = engine.externalKiwiSource(id, false);
        if (!source) {
            return 0;
        }
        qsizetype bytes = source->rxBuffer.size() + source->outputBuffer.size();
        for (const QByteArray& packet : source->rxPackets) {
            bytes += packet.size();
        }
        return bytes;
    }

    static void retire(AudioEngine& engine)
    {
        engine.retireInvalidPcmSources();
    }

    static void populateDurationQueues(AudioEngine& engine, int producerRate,
                                       int deviceRate, int scale)
    {
        engine.setRxDeviceRate(deviceRate);
        engine.resetMainPcmState(producerRate);
        const auto queued = [scale](int rate, int milliseconds) {
            return QByteArray(rate / 1000 * milliseconds * scale * 2 * sizeof(float), '\0');
        };
        engine.m_rxBuffer = queued(producerRate, 10);
        engine.m_rxPackets.push_back(queued(producerRate, 5));
        engine.m_rxPackets.push_back(queued(producerRate, 3));
        engine.m_rxOutputBuffer = queued(deviceRate, 7);
        engine.m_kiwiSdrRxBuffer = queued(24000, 11);
        engine.m_kiwiSdrRxPackets.push_back(queued(24000, 13));
        engine.m_kiwiSdrOutputBuffer = queued(deviceRate, 17);
        AudioEngine::ExternalRxAudioSourceState* first =
            engine.externalKiwiSource(QStringLiteral("duration-first"), true);
        first->enabled = true;
        first->rxBuffer = queued(24000, 19);
        first->rxPackets.push_back(queued(24000, 23));
        first->outputBuffer = queued(deviceRate, 29);
        AudioEngine::ExternalRxAudioSourceState* second =
            engine.externalKiwiSource(QStringLiteral("duration-second"), true);
        second->enabled = true;
        second->rxBuffer = queued(24000, 31);
        second->rxPackets.push_back(queued(24000, 37));
        second->outputBuffer = queued(deviceRate, 41);
        engine.m_radeRxBuffer = queued(deviceRate, 43);
        engine.updateRxBufferStats();
    }

    static bool drainWithInvalidProcessor(AudioEngine& engine, const QString& route,
                                         bool deepFilter, const QByteArray& pcm)
    {
        // Unsupported construction supplies a real, nonnull wrapper whose
        // model state is invalid, as after failed model preparation/reset.
        // No substitute algorithm or SDK/model load is involved.
        AudioEngine::ExternalRxAudioSourceState* source = nullptr;
        if (route == QStringLiteral("legacy")) {
            engine.m_kiwiSdrAudioEnabled = true;
        } else if (route == QStringLiteral("managed")) {
            source = engine.externalKiwiSource(route, true);
            source->enabled = true;
        }
        bool invalid = false;
        if (deepFilter) {
#ifdef HAVE_DFNR
            std::unique_ptr<DeepFilterFilter> filter = std::make_unique<DeepFilterFilter>(32000);
            invalid = !filter->isValid();
            engine.m_dfnrEnabled = true;
            if (source) {
                source->dfnr = std::move(filter);
            } else if (route == QStringLiteral("legacy")) {
                engine.m_kiwiSdrDfnr = std::move(filter);
            } else {
                engine.m_dfnr = std::move(filter);
            }
#endif
        } else {
#ifdef HAVE_SPECBLEACH
            std::unique_ptr<SpecbleachFilter> filter = std::make_unique<SpecbleachFilter>(32000);
            invalid = !filter->isValid();
            engine.m_nr4Enabled = true;
            if (source) {
                source->nr4 = std::move(filter);
            } else if (route == QStringLiteral("legacy")) {
                engine.m_kiwiSdrNr4 = std::move(filter);
            } else {
                engine.m_nr4 = std::move(filter);
            }
#endif
        }
        if (source) {
            engine.feedKiwiSdrAudioData(route, pcm);
            source->prebuffering = false;
        } else if (route == QStringLiteral("legacy")) {
            engine.feedKiwiSdrAudioData(pcm);
            engine.m_kiwiSdrPrebuffering = false;
        } else {
            engine.feedAudioData(pcm);
        }
        engine.drainRxAudio(pcm.size());
        return invalid && engine.rxBufferBytes() == 0;
    }
};

} // namespace AetherSDR

namespace {

bool checkAggregateDurations()
{
    AetherSDR::AudioEngine engine;
    AetherSDR::AudioEngineRatesTestAccess::stopTimer(engine);
    bool passed = true;
    for (int producerRate : {24000, 48000}) {
        for (int deviceRate : {24000, 44100, 48000}) {
            // 44100 cannot represent every integer millisecond exactly. Each
            // device queue uses explicit whole frames; account for those below.
            const double expectedMs = 152.0 + 137.0 * (deviceRate / 1000) * 1000.0 / deviceRate;
            const qsizetype expectedBytes =
                (producerRate / 1000 * 18 + 24000 / 1000 * 134
                 + deviceRate / 1000 * 137) * 2 * sizeof(float);
            AetherSDR::AudioEngineRatesTestAccess::populateDurationQueues(
                engine, producerRate, deviceRate, 2);
            passed = passed && std::abs(engine.rxBufferMs() - expectedMs * 2) < 0.00001
                && engine.rxBufferBytes() == expectedBytes * 2;
            const double peakMs = engine.rxBufferPeakMs();
            const qsizetype peakBytes = engine.rxBufferPeakBytes();
            AetherSDR::AudioEngineRatesTestAccess::populateDurationQueues(
                engine, producerRate, deviceRate, 1);
            passed = passed && std::abs(engine.rxBufferMs() - expectedMs) < 0.00001
                && engine.rxBufferBytes() == expectedBytes
                && engine.rxBufferPeakMs() == peakMs
                && engine.rxBufferPeakBytes() == peakBytes;
        }
    }
    engine.stopRxStream();
    passed = passed && engine.rxBufferMs() == 0.0 && engine.rxBufferPeakMs() == 0.0
        && engine.rxBufferBytes() == 0 && engine.rxBufferPeakBytes() == 0;
    std::printf("%s: aggregate durations sum all producer/device queues and retain independent peaks across rate changes\n",
                passed ? "PASS" : "FAIL");
    return passed;
}

bool checkInvalidProcessingWithheld()
{
    bool passed = true;
    for (bool deepFilter : {false, true}) {
#ifndef HAVE_DFNR
        if (deepFilter) {
            std::printf("SKIP: invalid DFNR state admission (DFNR unavailable in this build)\n");
            continue;
        }
#endif
#ifndef HAVE_SPECBLEACH
        if (!deepFilter) {
            std::printf("SKIP: invalid Specbleach state admission (Specbleach unavailable in this build)\n");
            continue;
        }
#endif
        for (const QString& route : {QStringLiteral("main"), QStringLiteral("legacy"), QStringLiteral("managed")}) {
            AetherSDR::AudioEngine engine;
            AetherSDR::AudioEngineRatesTestAccess::stopTimer(engine);
            QBuffer output;
            output.open(QIODevice::WriteOnly);
            AetherSDR::AudioEngineRatesTestAccess::attachMemoryOutput(engine, output);
            const QVector<float> samples(480, 0.25f);
            const QByteArray pcm(reinterpret_cast<const char*>(samples.constData()),
                                 samples.size() * static_cast<qsizetype>(sizeof(float)));
            const bool casePassed = AetherSDR::AudioEngineRatesTestAccess::drainWithInvalidProcessor(
                engine, route, deepFilter, pcm) && output.data().isEmpty();
            AetherSDR::AudioEngineRatesTestAccess::detachMemoryOutput(engine);
            passed = passed && casePassed;
            std::printf("%s: %s %s invalid nonnull processor withholds device output\n",
                casePassed ? "PASS" : "FAIL", deepFilter ? "DFNR" : "Specbleach", qPrintable(route));
        }
    }
    return passed;
}

bool checkPresentedMeters()
{
    AetherSDR::AudioEngine engine;
    AetherSDR::AudioEngineRatesTestAccess::stopTimer(engine);
    QBuffer output;
    output.open(QIODevice::WriteOnly);
    AetherSDR::AudioEngineRatesTestAccess::attachMemoryOutput(engine, output);
    const QString sourceId = QStringLiteral("meter-test-kiwi");
    AetherSDR::AudioEngineRatesTestAccess::enableSource(engine, sourceId);
    AetherSDR::ClientComp* const parameters = engine.clientCompRx();
    parameters->setEnabled(true);
    parameters->setThresholdDb(-30.0f);
    parameters->setRatio(6.0f);
    parameters->setAttackMs(1.0f);
    parameters->setLimiterEnabled(false);
    const QVector<float> loudSamples(480, 0.5f);
    const QByteArray loud(reinterpret_cast<const char*>(loudSamples.constData()),
                         loudSamples.size() * static_cast<qsizetype>(sizeof(float)));
    for (int block = 0; block < 20; ++block) {
        AetherSDR::AudioEngineRatesTestAccess::processSource(engine, sourceId, loud);
    }
    const bool auxiliaryMeterVisible = parameters->gainReductionDb() < -10.0f;

    AetherSDR::PcmProducer main;
    main.start();
    const std::optional<AetherSDR::PcmFrame> frame = main.produce(QVector<float>(480, 0.0f));
    engine.feedPcmFrame(*frame);
    AetherSDR::AudioEngineRatesTestAccess::processMain(engine, QByteArray(loud.size(), '\0'));
    const float mainGainReduction = parameters->gainReductionDb();
    AetherSDR::AudioEngineRatesTestAccess::processSource(engine, sourceId, loud);
    const bool mainPreferred = mainGainReduction == 0.0f
        && parameters->gainReductionDb() == mainGainReduction;

    main.invalidate();
    AetherSDR::AudioEngineRatesTestAccess::processSource(engine, sourceId, loud);
    const bool auxiliaryReturns = parameters->gainReductionDb() < -10.0f;
    const bool stableParameterOwner = engine.clientCompRx() == parameters
        && parameters->thresholdDb() == -30.0f;
    AetherSDR::AudioEngineRatesTestAccess::detachMemoryOutput(engine);
    const bool passed = auxiliaryMeterVisible && mainPreferred
        && auxiliaryReturns && stableParameterOwner;
    std::printf("%s: Kiwi-only dynamics meters, current-main preference and stable UI parameter owner\n",
                passed ? "PASS" : "FAIL");
    return passed;
}

bool checkSingleAuxiliaryFeed()
{
    bool passed = true;
    for (const QString& sourceId : {QString(), QStringLiteral("single-feed-kiwi")}) {
        AetherSDR::AudioEngine engine;
        AetherSDR::AudioEngineRatesTestAccess::stopTimer(engine);
        QBuffer output;
        output.open(QIODevice::WriteOnly);
        AetherSDR::AudioEngineRatesTestAccess::attachMemoryOutput(engine, output);
        if (sourceId.isEmpty()) {
            engine.setKiwiSdrAudioEnabled(true);
        } else {
            AetherSDR::AudioEngineRatesTestAccess::enableSource(engine, sourceId);
        }
        AetherSDR::PcmProducer producer;
        producer.start(AetherSDR::PcmPurpose::Auxiliary);
        const std::optional<AetherSDR::PcmFrame> frame = producer.produce(QVector<float>(480, 0.25f));
        const QByteArray pcm = frame->legacyStereo24();
        const bool captureStarted = engine.startAutomationAudioCapture(
            5000, {QStringLiteral("raw")}).value(QStringLiteral("ok")).toBool();
        engine.feedKiwiPcmFrame(sourceId, *frame);
        const auto feedRaw = [&]() {
            if (sourceId.isEmpty()) {
                engine.feedKiwiSdrAudioData(pcm);
            } else {
                engine.feedKiwiSdrAudioData(sourceId, pcm);
            }
        };
        feedRaw();
        const bool once = AetherSDR::AudioEngineRatesTestAccess::queuedKiwiBytes(engine, sourceId) == pcm.size()
            && engine.automationAudioCaptureSnapshot(false).value(QStringLiteral("chunks")).toArray().size() == 1;
        producer.invalidate();
        feedRaw();
        // A subsequent timer retirement must not discard the newly admitted
        // compatibility replacement because its old typed lease was stale.
        AetherSDR::AudioEngineRatesTestAccess::retire(engine);
        const bool replacementKept = AetherSDR::AudioEngineRatesTestAccess::queuedKiwiBytes(engine, sourceId) == pcm.size()
            && engine.automationAudioCaptureSnapshot(false).value(QStringLiteral("chunks")).toArray().size() == 2;
        AetherSDR::AudioEngineRatesTestAccess::detachMemoryOutput(engine);
        const bool casePassed = captureStarted && once && replacementKept;
        std::printf("%s: %s Kiwi typed/raw route feeds once and preserves raw replacement after revocation\n",
                    casePassed ? "PASS" : "FAIL", sourceId.isEmpty() ? "legacy" : "named");
        passed = passed && casePassed;
    }
    return passed;
}

} // namespace

int main(int argc, char** argv)
{
    const TestSettingsProfile profile(QStringLiteral("audio-engine-pcm-lifetime"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "FAIL: isolated settings profile unavailable\n");
        return 1;
    }
    qputenv("AETHER_AUTOMATION", "1");
    QCoreApplication app(argc, argv);
    const bool durationChecks = checkAggregateDurations();
    const bool invalidProcessingChecks = checkInvalidProcessingWithheld();
    const bool meterChecks = checkPresentedMeters();
    const bool singleFeedChecks = checkSingleAuxiliaryFeed();
    AetherSDR::AudioEngine engine;
    AetherSDR::AudioEngineRatesTestAccess::stopTimer(engine);
    engine.setRn2Enabled(true);
    AetherSDR::AudioEngineRatesTestAccess::waitInitialization(engine);
    const QString sourceId = QStringLiteral("lifetime-test-kiwi");
    AetherSDR::AudioEngineRatesTestAccess::enableSource(engine, sourceId);
    if (!AetherSDR::AudioEngineRatesTestAccess::initializeSource(engine, sourceId)
        || !AetherSDR::AudioEngineRatesTestAccess::sourcePrepared(engine, sourceId)) {
        std::fprintf(stderr, "FAIL: production RN2 initialization did not prepare auxiliary source\n");
        return 1;
    }

    std::atomic<bool> done{false};
    std::atomic<bool> initialized{true};
    std::atomic<unsigned> completedInitializations{0};
    std::thread initializer([&]() {
        // This is the method scheduleAllKiwiDspStateInitialization dispatches
        // off-thread. Repeated attempts overlap typed ingress, epoch retirement
        // and source removal/recreation on the engine's owning thread.
        while (!done.load(std::memory_order_acquire)) {
            if (!AetherSDR::AudioEngineRatesTestAccess::initializeSource(engine, sourceId)) {
                initialized.store(false, std::memory_order_relaxed);
            }
            completedInitializations.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    AetherSDR::PcmProducer producer;
    int retiredEpochs = 0;
    for (int cycle = 0; cycle < 300; ++cycle) {
        if (cycle % 8 == 0) {
            engine.removeKiwiSdrAudioSource(sourceId);
            AetherSDR::AudioEngineRatesTestAccess::enableSource(engine, sourceId);
        }
        AetherSDR::AudioEngineRatesTestAccess::clearSourceDsp(engine, sourceId);
        if (!producer.start(AetherSDR::PcmPurpose::Auxiliary)) {
            break;
        }
        const std::optional<AetherSDR::PcmFrame> frame = producer.produce(QVector<float>(480, 0.25f));
        if (!frame) {
            break;
        }
        engine.feedKiwiPcmFrame(sourceId, *frame);
        std::this_thread::yield();
        producer.invalidate();
        if (AetherSDR::AudioEngineRatesTestAccess::retireAndCheck(engine, sourceId)) {
            ++retiredEpochs;
        }
    }
    done.store(true, std::memory_order_release);
    initializer.join();
    const bool prepared = AetherSDR::AudioEngineRatesTestAccess::initializeSource(engine, sourceId)
        && AetherSDR::AudioEngineRatesTestAccess::sourcePrepared(engine, sourceId);
    const unsigned initializations = completedInitializations.load(std::memory_order_relaxed);
    const bool passed = durationChecks && invalidProcessingChecks && meterChecks && singleFeedChecks
        && initialized.load(std::memory_order_relaxed)
        && initializations > 0 && retiredEpochs == 300 && prepared;
    std::printf("%s: %d typed epochs retired; %u concurrent initialization attempts; final source %s\n",
        passed ? "PASS" : "FAIL", retiredEpochs, initializations, prepared ? "prepared" : "unprepared");
    return passed ? 0 : 1;
}
