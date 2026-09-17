// RFC #5468 A2: the real AudioEngine auxiliary ingress/retirement methods
// overlap its production asynchronous DSP initializer. No sink, audio device,
// socket, radio transport, hardware or transmitter is opened. Run under TSan
// for the lifetime claim; the native checks alone cannot establish race freedom.
#include "TestSettingsProfile.h"
#include "core/AudioEngine.h"
#include "core/ClientComp.h"
#include "core/DeepFilterFilter.h"
#include "core/SpecbleachFilter.h"
#include "core/SpectralNR.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QJsonArray>
#include <QTimer>

#include <atomic>
#include <barrier>
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

    static void feedMainPcm(AudioEngine& engine, const PcmFrame& frame)
    {
        engine.feedPcmFrame(frame);
    }

    static void drain(AudioEngine& engine, qsizetype freeBytes)
    {
        engine.drainRxAudio(freeBytes);
    }

    static const void* mainNr2Identity(AudioEngine& engine)
    {
        std::lock_guard<std::recursive_mutex> lock(engine.m_dspMutex);
        return engine.m_nr2.get();
    }

    // NR2's FFT size scales with the producer rate it was built for, so it
    // identifies the domain a filter belongs to.
    static int mainNr2FftSize(AudioEngine& engine)
    {
        std::lock_guard<std::recursive_mutex> lock(engine.m_dspMutex);
        return engine.m_nr2 ? engine.m_nr2->fftSize() : 0;
    }

    // Overlap-add output for one block. A filter that has been rebuilt starts
    // cold, so its output differs from one that has been running — which is the
    // only reliable way to tell a retained filter from a replacement. Neither
    // pointer identity nor FFT size can: a same-rate rebuild frees and
    // reallocates, and the allocator commonly returns the same address.
    static QByteArray mainNr2Process(AudioEngine& engine, const QByteArray& stereo)
    {
        std::lock_guard<std::recursive_mutex> lock(engine.m_dspMutex);
        if (!engine.m_nr2) {
            return {};
        }
        QByteArray out = stereo;
        const int frames = out.size() / (2 * static_cast<int>(sizeof(float)));
        engine.m_nr2->processStereoSharedMask(
            reinterpret_cast<const float*>(stereo.constData()),
            reinterpret_cast<float*>(out.data()), frames);
        return out;
    }

    static void setDeviceRate(AudioEngine& engine, int rate)
    {
        engine.setRxDeviceRate(rate);
    }

    static void setProducerRate(AudioEngine& engine, int rate)
    {
        engine.resetMainPcmState(rate);
    }

    static bool mainPcmFrameHeld(AudioEngine& engine)
    {
        std::lock_guard<std::recursive_mutex> lock(engine.m_dspMutex);
        return engine.m_mainPcmFrame.has_value();
    }
};

} // namespace AetherSDR

namespace {

// A producer revoked WHILE the drain is mixing must not reach the device. The
// top-of-drain retirement cannot see it — the epoch is still live when the drain
// starts — so the recheck immediately before the device write is the only thing
// standing between a dead radio and a chunk of its audio being played.
//
// receivePresentationOutputAudioReady fires from inside the mix, after the chunk
// is assembled and before that recheck, so a direct-connected slot revoking the
// producer there reproduces the concurrent case deterministically, on the real
// production path, with no test hook in the engine. Without the recheck this
// test observes the chunk arriving at the device.
bool checkRevocationDuringDspWithheld()
{
    AetherSDR::AudioEngine engine;
    AetherSDR::AudioEngineRatesTestAccess::stopTimer(engine);
    // The presentation signal is only emitted while a Kiwi source is active.
    const QString kiwiId = QStringLiteral("revocation-during-dsp");
    AetherSDR::AudioEngineRatesTestAccess::enableSource(engine, kiwiId);

    QBuffer output;
    output.open(QIODevice::ReadWrite);
    AetherSDR::AudioEngineRatesTestAccess::attachMemoryOutput(engine, output);

    AetherSDR::PcmProducer producer;
    if (!producer.start(AetherSDR::PcmPurpose::Speaker)) {
        std::fprintf(stderr, "FAIL: speaker producer would not start\n");
        return false;
    }
    const int frames = 480;
    QByteArray pcm(frames * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* samples = reinterpret_cast<float*>(pcm.data());
    for (int i = 0; i < frames * 2; ++i) {
        samples[i] = 0.25f;
    }
    const auto frame = producer.legacyStereo24(pcm);
    if (!frame) {
        std::fprintf(stderr, "FAIL: producer would not publish the test block\n");
        return false;
    }
    AetherSDR::AudioEngineRatesTestAccess::feedMainPcm(engine, *frame);

    int revocations = 0;
    QObject::connect(&engine, &AetherSDR::AudioEngine::receivePresentationOutputAudioReady,
                     &engine, [&](const QString&, const QString&, const QByteArray&, int) {
        // Mid-mix: exactly where a disconnect on the radio thread would land.
        if (revocations++ == 0) {
            producer.invalidate();
        }
    }, Qt::DirectConnection);

    AetherSDR::AudioEngineRatesTestAccess::drain(engine, pcm.size());

    const bool fired = revocations > 0;
    const bool withheld = output.data().isEmpty();
    const bool retired = !AetherSDR::AudioEngineRatesTestAccess::mainPcmFrameHeld(engine);
    AetherSDR::AudioEngineRatesTestAccess::detachMemoryOutput(engine);

    if (!fired) {
        std::fprintf(stderr,
            "FAIL: presentation signal never fired; the revocation seam did not run\n");
        return false;
    }
    std::printf("%s: revoked-during-DSP chunk withheld from the device\n",
                withheld ? "PASS" : "FAIL");
    std::printf("%s: revoked main source retired by the pre-write recheck\n",
                retired ? "PASS" : "FAIL");
    return withheld && retired;
}

// The optional NR chain belongs to the PRODUCER domain: its filters are built
// for a producer rate and never see the device rate. startRxStream() reaches
// setRxDeviceRate() on every sink open — including the #1361 zombie-sink and
// #1411 liveness watchdogs, which run on the GUI thread — so rebuilding there
// charged each of those recovery paths a full model construction (DFNR measures
// ~450 ms) for no change. Pin both directions: a device-rate change keeps the
// filters, a producer-rate change replaces them.
bool checkDeviceRateKeepsNrChain()
{
    // 480 stereo frames of a steady tone: enough to drive the overlap-add.
    const int frames = 480;
    QByteArray block(frames * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* samples = reinterpret_cast<float*>(block.data());
    for (int i = 0; i < frames; ++i) {
        const float v = 0.3f * std::sin(2.0f * float(M_PI) * 1000.0f * float(i) / 24000.0f);
        samples[i * 2] = v;
        samples[i * 2 + 1] = v;
    }

    auto primed = [&](AetherSDR::AudioEngine& engine) {
        engine.setNr2Enabled(true);
        for (int i = 0; i < 8; ++i) {
            AetherSDR::AudioEngineRatesTestAccess::mainNr2Process(engine, block);
        }
    };

    // A: primed, no device-rate change. The reference "still running" output.
    AetherSDR::AudioEngine warmEngine;
    AetherSDR::AudioEngineRatesTestAccess::stopTimer(warmEngine);
    primed(warmEngine);
    const QByteArray warm =
        AetherSDR::AudioEngineRatesTestAccess::mainNr2Process(warmEngine, block);

    // B: fresh filter, never primed. The "was rebuilt" output.
    AetherSDR::AudioEngine coldEngine;
    AetherSDR::AudioEngineRatesTestAccess::stopTimer(coldEngine);
    coldEngine.setNr2Enabled(true);
    const QByteArray cold =
        AetherSDR::AudioEngineRatesTestAccess::mainNr2Process(coldEngine, block);

    // The detector is only meaningful if warm and cold actually differ.
    const bool sensitive = !warm.isEmpty() && warm != cold;
    std::printf("%s: primed and cold NR2 outputs differ (detector is sensitive)\n",
                sensitive ? "PASS" : "FAIL");

    // C: primed, then two device-rate changes. Must still match the warm output.
    AetherSDR::AudioEngine engine;
    AetherSDR::AudioEngineRatesTestAccess::stopTimer(engine);
    primed(engine);
    const int builtFft = AetherSDR::AudioEngineRatesTestAccess::mainNr2FftSize(engine);
    AetherSDR::AudioEngineRatesTestAccess::setDeviceRate(engine, 48000);
    AetherSDR::AudioEngineRatesTestAccess::setDeviceRate(engine, 44100);
    const QByteArray afterDevice =
        AetherSDR::AudioEngineRatesTestAccess::mainNr2Process(engine, block);
    const bool kept = afterDevice == warm
        && AetherSDR::AudioEngineRatesTestAccess::mainNr2FftSize(engine) == builtFft;
    std::printf("%s: device-rate change keeps the producer-domain NR chain and its history\n",
                kept ? "PASS" : "FAIL");

    // A real producer-rate move must still replace the chain; FFT size scales
    // with the producer rate, so it survives allocator address reuse.
    AetherSDR::AudioEngineRatesTestAccess::setProducerRate(engine, 48000);
    const int afterProducerFft = AetherSDR::AudioEngineRatesTestAccess::mainNr2FftSize(engine);
    const bool rebuilt = builtFft > 0 && afterProducerFft == builtFft * 2;
    std::printf("%s: producer-rate change still rebuilds the NR chain (fft %d -> %d)\n",
                rebuilt ? "PASS" : "FAIL", builtFft, afterProducerFft);

    return sensitive && kept && rebuilt;
}

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
    const bool revocationDuringDspChecks = checkRevocationDuringDspWithheld();
    const bool deviceRateNrChecks = checkDeviceRateKeepsNrChain();
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

    std::atomic<bool> initialized{true};
    std::atomic<unsigned> completedInitializations{0};
    constexpr int kRetirementCycles = 300;
    std::barrier initializationPhase(2);
    std::thread initializer([&]() {
        // This is the method scheduleAllKiwiDspStateInitialization dispatches
        // off-thread. Repeated attempts overlap typed ingress, epoch retirement
        // and source removal/recreation on the engine's owning thread.
        for (int cycle = 0; cycle < kRetirementCycles; ++cycle) {
            initializationPhase.arrive_and_wait();
            if (!AetherSDR::AudioEngineRatesTestAccess::initializeSource(engine, sourceId)) {
                initialized.store(false, std::memory_order_relaxed);
            }
            completedInitializations.fetch_add(1, std::memory_order_relaxed);
            initializationPhase.arrive_and_wait();
        }
    });

    AetherSDR::PcmProducer producer;
    int retiredEpochs = 0;
    for (int cycle = 0; cycle < kRetirementCycles; ++cycle) {
        // Each churn window has an initializer attempt. A yield alone lets
        // the owning thread finish every epoch before the worker is scheduled.
        initializationPhase.arrive_and_wait();
        if (cycle % 8 == 0) {
            engine.removeKiwiSdrAudioSource(sourceId);
            AetherSDR::AudioEngineRatesTestAccess::enableSource(engine, sourceId);
        }
        AetherSDR::AudioEngineRatesTestAccess::clearSourceDsp(engine, sourceId);
        if (producer.start(AetherSDR::PcmPurpose::Auxiliary)) {
            const std::optional<AetherSDR::PcmFrame> frame = producer.produce(QVector<float>(480, 0.25f));
            if (frame) {
                engine.feedKiwiPcmFrame(sourceId, *frame);
                std::this_thread::yield();
                producer.invalidate();
                if (AetherSDR::AudioEngineRatesTestAccess::retireAndCheck(engine, sourceId)) {
                    ++retiredEpochs;
                }
            }
        }
        // Never leave the peer waiting when a producer assertion fails: the
        // retired-epoch count still reports that failure after both join.
        initializationPhase.arrive_and_wait();
    }
    initializer.join();
    const bool prepared = AetherSDR::AudioEngineRatesTestAccess::initializeSource(engine, sourceId)
        && AetherSDR::AudioEngineRatesTestAccess::sourcePrepared(engine, sourceId);
    const unsigned initializations = completedInitializations.load(std::memory_order_relaxed);
    const bool passed = durationChecks && invalidProcessingChecks && revocationDuringDspChecks
        && deviceRateNrChecks && meterChecks && singleFeedChecks
        && initialized.load(std::memory_order_relaxed)
        && initializations == kRetirementCycles && retiredEpochs == kRetirementCycles && prepared;
    std::printf("%s: %d typed epochs retired; %u concurrent initialization attempts; final source %s\n",
        passed ? "PASS" : "FAIL", retiredEpochs, initializations, prepared ? "prepared" : "unprepared");
    return passed ? 0 : 1;
}
