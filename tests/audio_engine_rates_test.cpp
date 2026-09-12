// RFC #5468 A2: drive the production RX queue/DSP/mixer with a memory-backed
// output device. No QAudioSink, socket peer, radio connection, or RF is used.
#include "TestSettingsProfile.h"
#include "core/AudioEngine.h"
#include "core/ClientComp.h"
#include "core/ClientDeEss.h"
#include "core/ClientEq.h"
#include "core/ClientGate.h"
#include "core/ClientPudu.h"
#include "core/ClientTube.h"
#include "core/CwSidetoneGenerator.h"
#include "core/Resampler.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numbers>
#include <optional>

namespace AetherSDR {
class AudioEngineRatesTestAccess {
public:
    static void attach(AudioEngine& engine, QBuffer& output, int deviceRate)
    {
        engine.m_rxTimer->stop();
        engine.m_audioDevice = &output;
        engine.setRxDeviceRate(deviceRate);
    }
    static void detach(AudioEngine& engine) { engine.m_audioDevice = nullptr; }
    static void drain(AudioEngine& engine, qsizetype bytes) { engine.drainRxAudio(bytes); }
    static qsizetype rawBytes(const AudioEngine& engine)
    {
        qsizetype result = engine.m_rxBuffer.size();
        for (const QByteArray& packet : engine.m_rxPackets) {
            result += packet.size();
        }
        return result;
    }
    static qsizetype outputBytes(const AudioEngine& engine)
    {
        return engine.m_rxOutputBuffer.size();
    }
    static qsizetype legacyKiwiRawBytes(const AudioEngine& engine)
    {
        qsizetype result = engine.m_kiwiSdrRxBuffer.size();
        for (const QByteArray& packet : engine.m_kiwiSdrRxPackets) {
            result += packet.size();
        }
        return result;
    }
    static int producerRate(const AudioEngine& engine) { return engine.m_rxProducerRate.load(); }
    static int outputRate(const AudioEngine& engine) { return engine.m_rxOutputRate.load(); }
    static void deviceRate(AudioEngine& engine, int rate) { engine.setRxDeviceRate(rate); }
    static bool prepareKiwi(AudioEngine& engine)
    {
        engine.m_dspInitializationTasks.waitForFinished();
        return engine.ensureAllKiwiDspState();
    }
    static qsizetype kiwiBytes(AudioEngine& engine, const QString& id)
    {
        const auto* source = engine.externalKiwiSource(id, false);
        if (!source) {
            return 0;
        }
        qsizetype result = source->rxBuffer.size() + source->outputBuffer.size();
        for (const QByteArray& packet : source->rxPackets) {
            result += packet.size();
        }
        return result;
    }
    static void renderCwDecodeSilence(AudioEngine& engine, int rate)
    {
        engine.m_cwSidetone->setSampleRateHz(rate);
        QVector<float> silence(rate / 100 * 2, 0.0f);
        engine.m_cwSidetone->process(silence.data(), rate / 100);
    }
    static int recordedCwRate(const AudioEngine& engine)
    {
        return engine.m_cwRecordSidetone->sampleRateHz();
    }
    static double radeSourceRate(const AudioEngine& engine)
    {
        return engine.m_radeRxResampler ? engine.m_radeRxResampler->srcRate() : 24000.0;
    }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {
constexpr qsizetype kFrameBytes = 2 * static_cast<qsizetype>(sizeof(float));
constexpr double kTau = 2.0 * std::numbers::pi;
const QString kKiwiId = QStringLiteral("rate-test-kiwi");
int checks = 0;
int failures = 0;

void check(bool condition, const char* message)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

QByteArray bytes(const QVector<float>& samples)
{
    return {reinterpret_cast<const char*>(samples.constData()),
            samples.size() * static_cast<qsizetype>(sizeof(float))};
}

QVector<float> tone(int rate, int frames, quint64 offset = 0,
                    double leftHz = 1000.0, double rightHz = 2300.0,
                    PcmLayout layout = PcmLayout::Stereo)
{
    const int channels = layout == PcmLayout::Mono ? 1 : 2;
    QVector<float> result(frames * channels);
    for (int frame = 0; frame < frames; ++frame) {
        const double time = static_cast<double>(offset + static_cast<quint64>(frame)) / rate;
        result[frame * channels] = static_cast<float>(0.3 * std::sin(kTau * leftHz * time));
        if (channels == 2) {
            result[frame * channels + 1] = static_cast<float>(0.17 * std::sin(kTau * rightHz * time));
        }
    }
    return result;
}

double amplitude(const QByteArray& pcm, int rate, int channel, double frequency)
{
    const qsizetype frames = pcm.size() / kFrameBytes;
    // Ignore startup/filter transients, then measure half a second at most.
    const qsizetype count = std::min<qsizetype>(rate / 2, frames / 2);
    if (count < 100) {
        return 0.0;
    }
    const auto* samples = reinterpret_cast<const float*>(pcm.constData());
    double real = 0.0;
    double imaginary = 0.0;
    for (qsizetype index = 0; index < count; ++index) {
        const double phase = kTau * frequency * static_cast<double>(index) / rate;
        const float value = samples[(frames - count + index) * 2 + channel];
        real += value * std::cos(phase);
        imaginary += value * std::sin(phase);
    }
    return 2.0 * std::hypot(real, imaginary) / static_cast<double>(count);
}

bool finite(const QByteArray& pcm)
{
    if (pcm.size() % kFrameBytes != 0) {
        return false;
    }
    const auto* samples = reinterpret_cast<const float*>(pcm.constData());
    const qsizetype count = pcm.size() / static_cast<qsizetype>(sizeof(float));
    return std::all_of(samples, samples + count, [](float value) { return std::isfinite(value); });
}

struct Fixture {
    QBuffer output;
    AudioEngine engine;
    int deviceRate;

    explicit Fixture(int rate) : deviceRate(rate)
    {
        check(output.open(QIODevice::ReadWrite), "memory output opens");
        AudioEngineRatesTestAccess::attach(engine, output, rate);
        engine.setMuted(false);
        engine.setRxBoost(false);
        engine.setRxOutputTrimDb(0.0f);
        engine.setRxBufferCapMs(1000);
        engine.clientEqRx()->setEnabled(false);
        engine.clientGateRx()->setEnabled(false);
        engine.clientCompRx()->setEnabled(false);
        engine.clientDeEssRx()->setEnabled(false);
        engine.clientTubeRx()->setEnabled(false);
        engine.clientPuduRx()->setEnabled(false);
    }
    ~Fixture() { AudioEngineRatesTestAccess::detach(engine); }
    void capture()
    {
        check(engine.startAutomationAudioCapture(30000,
                  {QStringLiteral("raw"), QStringLiteral("post"),
                   QStringLiteral("output"), QStringLiteral("final")})
                  .value("ok").toBool(), "capture starts");
    }
    void tick() { AudioEngineRatesTestAccess::drain(engine, deviceRate / 100 * kFrameBytes); }
    void finish()
    {
        for (int tick = 0; tick < 8; ++tick) {
            AudioEngineRatesTestAccess::drain(engine, deviceRate * kFrameBytes);
        }
    }
    QByteArray captured(const QString& point, const QString& source = QStringLiteral("flex"),
                        const QString& sourceId = QString()) const
    {
        QByteArray result;
        const QJsonArray chunks = engine.automationAudioCaptureSnapshot(true).value("chunks").toArray();
        for (const QJsonValue& value : chunks) {
            const QJsonObject chunk = value.toObject();
            if (chunk.value("point").toString() == point
                && chunk.value("source").toString() == source
                && chunk.value("sourceId").toString() == sourceId) {
                const int expectedRate = point == QStringLiteral("raw")
                    ? (source == QStringLiteral("kiwi") ? 24000
                        : AudioEngineRatesTestAccess::producerRate(engine)) : deviceRate;
                check(chunk.value("sampleRate").toInt() == expectedRate,
                      "capture labels producer and device domains distinctly");
                result += QByteArray::fromBase64(chunk.value("pcmBase64").toString().toLatin1());
            }
        }
        return result;
    }
};

void feed(AudioEngine& engine, PcmProducer& producer, QVector<float> samples)
{
    const std::optional<PcmFrame> frame = producer.produce(std::move(samples));
    check(frame.has_value(), "test producer creates a valid frame");
    if (frame) {
        engine.feedPcmFrame(*frame);
    }
}

void rateMatrix()
{
    for (int producerRate : {24000, 48000}) {
        for (int deviceRate : {24000, 44100, 48000}) {
            Fixture fixture(deviceRate);
            fixture.capture();
            PcmProducer producer;
            check(producer.start(PcmPurpose::Speaker, -1, {producerRate, PcmLayout::Stereo}),
                  "matrix producer starts");
            QByteArray original;
            for (int tick = 0; tick < 100; ++tick) {
                const QVector<float> samples = tone(producerRate, producerRate / 100,
                    static_cast<quint64>(tick * (producerRate / 100)));
                original += bytes(samples);
                feed(fixture.engine, producer, samples);
                fixture.tick();
            }
            fixture.finish();
            const QByteArray output = fixture.output.data();
            const qsizetype frames = output.size() / kFrameBytes;
            std::printf("matrix producer=%d device=%d outputFrames=%lld\n",
                        producerRate, deviceRate, static_cast<long long>(frames));
            check(frames == deviceRate,
                  "one second producer input yields one second device output");
            check(finite(output), "matrix output is finite aligned stereo");
            check(amplitude(output, deviceRate, 0, 1000) > 0.27,
                  "matrix preserves left frequency and amplitude");
            check(amplitude(output, deviceRate, 1, 2300) > 0.15,
                  "matrix preserves independent right frequency and amplitude");
            check(amplitude(output, deviceRate, 1, 1000) < 0.002,
                  "matrix does not fold left into right");
            check(AudioEngineRatesTestAccess::producerRate(fixture.engine) == producerRate
                  && AudioEngineRatesTestAccess::outputRate(fixture.engine) == deviceRate,
                  "producer metadata never replaces device rate");
            check(fixture.engine.clientEqRx()->sampleRate() == producerRate
                  && fixture.engine.clientGateRx()->sampleRate() == producerRate
                  && fixture.engine.clientCompRx()->sampleRate() == producerRate
                  && fixture.engine.clientDeEssRx()->sampleRate() == producerRate
                  && fixture.engine.clientTubeRx()->sampleRate() == producerRate
                  && fixture.engine.clientPuduRx()->sampleRate() == producerRate,
                  "every main client effect is prepared in the producer domain");
            check(fixture.captured(QStringLiteral("raw")) == original,
                  "typed producer capture is byte exact");
            check(fixture.captured(QStringLiteral("output")) == output,
                  "exactly one output feed reaches memory device");
            if (producerRate == 24000 && deviceRate == 24000) {
                check(output == original, "legacy 24 kHz bypass remains byte exact");
            }
        }
    }
}

void bandwidthAndMono()
{
    for (int deviceRate : {24000, 44100, 48000}) {
        Fixture fixture(deviceRate);
        PcmProducer producer;
        producer.start(PcmPurpose::Speaker, -1, {48000, PcmLayout::Stereo});
        for (int tick = 0; tick < 100; ++tick) {
            feed(fixture.engine, producer, tone(48000, 480, tick * 480, 15000, 1000));
            fixture.tick();
        }
        fixture.finish();
        check(amplitude(fixture.output.data(), deviceRate, 1, 1000) > 0.15,
              "wide producer preserves low-band stereo channel");
        if (deviceRate >= 44100) {
            check(amplitude(fixture.output.data(), deviceRate, 0, 15000) > 0.27,
                  "48 kHz producer retains 15 kHz audio bandwidth");
        } else {
            check(amplitude(fixture.output.data(), deviceRate, 0, 9000) < 0.003,
                  "48 to 24 conversion rejects the 15 kHz alias");
        }
    }
    for (int producerRate : {24000, 48000}) {
        Fixture fixture(48000);
        PcmProducer producer;
        producer.start(PcmPurpose::Speaker, -1, {producerRate, PcmLayout::Mono});
        for (int tick = 0; tick < 100; ++tick) {
            feed(fixture.engine, producer,
                 tone(producerRate, producerRate / 100, tick * (producerRate / 100),
                      1000, 1000, PcmLayout::Mono));
            fixture.tick();
        }
        fixture.finish();
        const QByteArray output = fixture.output.data();
        const auto* samples = reinterpret_cast<const float*>(output.constData());
        bool identical = !output.isEmpty();
        for (qsizetype frame = 0; frame < output.size() / kFrameBytes; ++frame) {
            identical = identical && samples[frame * 2] == samples[frame * 2 + 1];
        }
        check(identical && amplitude(output, 48000, 0, 1000) > 0.27,
              "mono producers duplicate into stereo at the producer domain");
    }
}

void transitionsAndRejection()
{
    Fixture fixture(48000);
    fixture.capture();
    PcmProducer producer;
    producer.start();
    const PcmFrame first = *producer.produce(tone(24000, 240));
    fixture.engine.feedPcmFrame(first);
    const qsizetype queued = AudioEngineRatesTestAccess::rawBytes(fixture.engine);
    fixture.engine.feedPcmFrame(first);
    fixture.engine.feedPcmFrame(PcmFrame{});
    check(queued == 240 * kFrameBytes
          && AudioEngineRatesTestAccess::rawBytes(fixture.engine) == queued,
          "duplicate and invalid frames never enter the queue");
    check(fixture.captured(QStringLiteral("raw")).size() == queued,
          "duplicate first frame cannot reset and replace the queue as a second feed");
    fixture.engine.feedAudioData(bytes(tone(24000, 240)));
    check(AudioEngineRatesTestAccess::rawBytes(fixture.engine) == queued
          && fixture.captured(QStringLiteral("raw")).size() == queued,
          "legacy callback cannot double-feed a speaker route owned by typed PCM");
    check(!producer.produce({std::numeric_limits<float>::quiet_NaN(), 0.0f})
          && !producer.produce({0.0f})
          && !producer.setFormat({44100, PcmLayout::Stereo}),
          "malformed samples and unsupported producer rate are refused");

    PcmProducer competing;
    competing.start(PcmPurpose::Speaker, -1, {48000, PcmLayout::Stereo});
    feed(fixture.engine, competing, tone(48000, 480));
    check(AudioEngineRatesTestAccess::rawBytes(fixture.engine) == queued
          && AudioEngineRatesTestAccess::producerRate(fixture.engine) == 24000,
          "a second live producer cannot take over the speaker route");

    const PcmFrame next = *producer.produce(tone(24000, 240, 240));
    fixture.engine.feedPcmFrame(next);
    const qsizetype orderedBytes = AudioEngineRatesTestAccess::rawBytes(fixture.engine);
    fixture.engine.feedPcmFrame(next);
    fixture.engine.feedPcmFrame(first);
    check(orderedBytes == 2 * queued
          && AudioEngineRatesTestAccess::rawBytes(fixture.engine) == orderedBytes
          && fixture.captured(QStringLiteral("raw")).size() == orderedBytes,
          "midstream duplicates and older same-epoch frames are rejected before any feed");

    producer.setFormat({48000, PcmLayout::Stereo});
    feed(fixture.engine, producer, QVector<float>(960, 0.0f));
    check(AudioEngineRatesTestAccess::rawBytes(fixture.engine) == 480 * kFrameBytes
          && AudioEngineRatesTestAccess::producerRate(fixture.engine) == 48000,
          "format change replaces queued samples and producer rate together");
    fixture.engine.feedPcmFrame(first);
    check(AudioEngineRatesTestAccess::rawBytes(fixture.engine) == 480 * kFrameBytes,
          "old-format frame cannot enter the new-format queue");
    fixture.tick();
    check(fixture.output.data() == QByteArray(480 * kFrameBytes, '\0'),
          "format transition discards old waveform and resampler tail");

    feed(fixture.engine, producer, tone(48000, 960));
    const PcmFrame discontinuity = *producer.produce(QVector<float>(960, 0.0f), 4800, true);
    fixture.engine.feedPcmFrame(discontinuity);
    check(AudioEngineRatesTestAccess::rawBytes(fixture.engine) == 480 * kFrameBytes,
          "discontinuity flushes pending raw input before admitting new samples");
    fixture.tick();
    check(fixture.output.data().right(480 * kFrameBytes) == QByteArray(480 * kFrameBytes, '\0'),
          "discontinuity does not replay pre-gap processing state");

    feed(fixture.engine, producer, tone(48000, 480));
    producer.invalidate();
    fixture.tick();
    check(AudioEngineRatesTestAccess::rawBytes(fixture.engine) == 0
          && AudioEngineRatesTestAccess::outputBytes(fixture.engine) == 0,
          "disconnect revocation flushes queues without another producer frame");
    producer.start();
    feed(fixture.engine, producer, tone(24000, 240));
    check(AudioEngineRatesTestAccess::producerRate(fixture.engine) == 24000,
          "reconnected producer returns to its declared 24 kHz domain");
    producer.invalidate();
    feed(fixture.engine, competing, tone(48000, 480, 480));
    check(AudioEngineRatesTestAccess::producerRate(fixture.engine) == 48000
          && AudioEngineRatesTestAccess::rawBytes(fixture.engine) == 480 * kFrameBytes,
          "replacement producer is accepted after prior lease revocation");
}

void queueBudgetsAndDeviceTransitions()
{
    for (int producerRate : {24000, 48000}) {
        Fixture fixture(44100);
        fixture.engine.setRxBufferCapMs(100);
        PcmProducer producer;
        producer.start(PcmPurpose::Speaker, -1, {producerRate, PcmLayout::Stereo});
        feed(fixture.engine, producer, tone(producerRate, producerRate / 4));
        AudioEngineRatesTestAccess::drain(fixture.engine, 0);
        check(AudioEngineRatesTestAccess::rawBytes(fixture.engine)
                  == producerRate / 10 * kFrameBytes,
              "raw queue cap uses producer duration at either producer rate");
        check(fixture.engine.receivePresentationAudioQueues().flexRawBufferMs == 100,
              "raw queue diagnostics report producer milliseconds");
        fixture.engine.setRxBufferCapMs(1000);
        fixture.engine.setReceivePresentationDelays(100, 0);
        feed(fixture.engine, producer, tone(producerRate, producerRate / 5, producerRate / 4));
        fixture.tick();
        check(fixture.engine.receivePresentationAudioQueues().flexRawBufferMs >= 100,
              "presentation delay retains the configured producer duration");

        AudioEngineRatesTestAccess::deviceRate(fixture.engine, 24000);
        fixture.deviceRate = 24000;
        check(AudioEngineRatesTestAccess::rawBytes(fixture.engine) == 0
              && AudioEngineRatesTestAccess::outputBytes(fixture.engine) == 0,
              "device format transition discards queued old-rate samples");
        check(AudioEngineRatesTestAccess::producerRate(fixture.engine) == producerRate
              && AudioEngineRatesTestAccess::outputRate(fixture.engine) == 24000,
              "device renegotiation retains the producer rate");
        fixture.engine.setReceivePresentationDelays(0, 0);
        const qsizetype before = fixture.output.data().size();
        for (int tick = 0; tick < 100; ++tick) {
            feed(fixture.engine, producer, tone(producerRate, producerRate / 100,
                static_cast<quint64>(tick * (producerRate / 100))));
            fixture.tick();
        }
        fixture.finish();
        const QByteArray changed = fixture.output.data().mid(before);
        check(std::abs(changed.size() / kFrameBytes - 24000) <= 512
              && amplitude(changed, 24000, 0, 1000) > 0.27,
              "remaining producer continues at the new device rate without retiming");
    }
}

enum class Effect { None, Eq, Nr2, Rn2, Nr4, Dfnr, Mnr };

const char* effectName(Effect effect)
{
    switch (effect) {
    case Effect::None: return "bypass";
    case Effect::Eq: return "EQ";
    case Effect::Nr2: return "NR2";
    case Effect::Rn2: return "RN2";
    case Effect::Nr4: return "NR4";
    case Effect::Dfnr: return "DFNR";
    case Effect::Mnr: return "MNR";
    }
    return "unknown";
}

bool enableEffect(AudioEngine& engine, Effect effect)
{
    switch (effect) {
    case Effect::None:
        return true;
    case Effect::Eq: {
        ClientEq::BandParams band;
        band.type = ClientEq::FilterType::LowPass;
        band.freqHz = 1800.0f;
        band.enabled = true;
        engine.clientEqRx()->setActiveBandCount(1);
        engine.clientEqRx()->setBand(0, band);
        engine.clientEqRx()->setMasterGain(0.5f);
        engine.clientEqRx()->setEnabled(true);
        return true;
    }
    case Effect::Nr2:
        engine.setNr2Enabled(true);
        return engine.nr2Enabled();
    case Effect::Rn2:
        engine.setRn2Enabled(true);
        engine.setRn2DryMix(0.5f);
        return engine.rn2Enabled();
    case Effect::Nr4:
        engine.setNr4Enabled(true);
        return engine.nr4Enabled();
    case Effect::Dfnr:
        engine.setDfnrEnabled(true);
        return engine.dfnrEnabled();
    case Effect::Mnr:
#ifdef Q_OS_MAC
        engine.setMnrEnabled(true);
        return engine.mnrEnabled();
#else
        return false;
#endif
    }
    return false;
}

struct Rendered {
    bool enabled = false;
    QByteArray main;
    QByteArray kiwi;
    QByteArray final;
};

Rendered renderSources(int mainRate, Effect effect, bool mainEnabled, bool kiwiEnabled)
{
    Fixture fixture(48000);
    PcmProducer main;
    PcmProducer kiwi;
    if (mainEnabled) {
        main.start(PcmPurpose::Speaker, -1, {mainRate, PcmLayout::Stereo});
        feed(fixture.engine, main, QVector<float>(mainRate / 100 * 2, 0.0f));
        fixture.tick();
    }
    Rendered result;
    result.enabled = enableEffect(fixture.engine, effect);
    if (!result.enabled) {
        return result;
    }
    if (kiwiEnabled) {
        fixture.engine.setKiwiSdrAudioSourceEnabled(kKiwiId, true);
        check(AudioEngineRatesTestAccess::prepareKiwi(fixture.engine),
              "selected Kiwi effect initializes before audio is measured");
        kiwi.start(PcmPurpose::Auxiliary);
    }
    fixture.output.buffer().clear();
    fixture.output.seek(0);
    fixture.capture();
    quint64 kiwiOffset = 0;
    if (kiwiEnabled) {
        // Exercise the real 360 ms Kiwi prebuffer instead of defeating it.
        for (int packet = 0; packet < 40; ++packet) {
            fixture.engine.feedKiwiPcmFrame(kKiwiId,
                *kiwi.produce(tone(24000, 240, kiwiOffset, 700, 3200)));
            kiwiOffset += 240;
        }
    }
    for (int tick = 0; tick < 150; ++tick) {
        if (mainEnabled) {
            feed(fixture.engine, main, tone(mainRate, mainRate / 100,
                static_cast<quint64>(tick * (mainRate / 100))));
        }
        if (kiwiEnabled) {
            fixture.engine.feedKiwiPcmFrame(kKiwiId,
                *kiwi.produce(tone(24000, 240, kiwiOffset, 700, 3200)));
            kiwiOffset += 240;
        }
        fixture.tick();
    }
    fixture.finish();
    result.main = fixture.captured(QStringLiteral("post"));
    result.kiwi = fixture.captured(QStringLiteral("post"), QStringLiteral("kiwi"), kKiwiId);
    result.final = fixture.output.data();
    return result;
}

bool samePcm(const QByteArray& lhs, const QByteArray& rhs)
{
    if (lhs.isEmpty() || lhs.size() != rhs.size()) {
        return false;
    }
    const auto* first = reinterpret_cast<const float*>(lhs.constData());
    const auto* second = reinterpret_cast<const float*>(rhs.constData());
    for (qsizetype index = 0; index < lhs.size() / static_cast<qsizetype>(sizeof(float)); ++index) {
        if (!std::isfinite(first[index]) || !std::isfinite(second[index])
            || std::abs(first[index] - second[index]) > 0.00002f) {
            return false;
        }
    }
    return true;
}

void concurrentSourcesAndEffects()
{
    for (int mainRate : {24000, 48000}) {
        for (Effect effect : {Effect::None, Effect::Eq, Effect::Nr2, Effect::Rn2,
                              Effect::Nr4, Effect::Dfnr, Effect::Mnr}) {
            const Rendered mainOnly = renderSources(mainRate, effect, true, false);
            if (!mainOnly.enabled) {
                const bool optional = effect == Effect::Nr4 || effect == Effect::Dfnr
                    || effect == Effect::Mnr;
                check(optional, "mandatory NR2/RN2 effect must be available");
                std::printf("SKIP: optional effect %s unavailable at %d Hz\n",
                            effectName(effect), mainRate);
                continue;
            }
            const Rendered kiwiOnly = renderSources(mainRate, effect, false, true);
            const Rendered combined = renderSources(mainRate, effect, true, true);
            std::printf("effects mainRate=%d effect=%s main=%lld kiwi=%lld combined=%lld/%lld\n",
                        mainRate, effectName(effect),
                        static_cast<long long>(mainOnly.main.size()),
                        static_cast<long long>(kiwiOnly.kiwi.size()),
                        static_cast<long long>(combined.main.size()),
                        static_cast<long long>(combined.kiwi.size()));
            check(kiwiOnly.enabled && combined.enabled,
                  "selected effect is enabled for concurrent-source comparison");
            check(samePcm(mainOnly.main, combined.main),
                  "concurrent Kiwi24 does not alter main processing state");
            check(samePcm(kiwiOnly.kiwi, combined.kiwi),
                  "main producer rate does not alter independent Kiwi24 processing");
            check(finite(combined.final) && !combined.final.isEmpty(),
                  "concurrent effects mix finite output through the real device writer");
            if (effect == Effect::Eq) {
                const double gain = amplitude(mainOnly.main, 48000, 0, 1000);
                check(gain > 0.11 && gain < 0.16,
                      "enabled EQ applies its configured gain in the producer domain");
            }
        }
    }

    Fixture fixture(48000);
    PcmProducer main;
    PcmProducer kiwi;
    PcmProducer collision;
    main.start(PcmPurpose::Speaker, -1, {48000, PcmLayout::Stereo});
    kiwi.start(PcmPurpose::Auxiliary);
    collision.start(PcmPurpose::Auxiliary);
    fixture.engine.setKiwiSdrAudioSourceEnabled(kKiwiId, true);
    fixture.engine.setKiwiSdrAudioSourceEnabled(QStringLiteral("second-kiwi"), true);
    feed(fixture.engine, main, tone(48000, 480));
    const PcmFrame kiwiFrame = *kiwi.produce(tone(24000, 240));
    fixture.engine.feedKiwiPcmFrame(kKiwiId, kiwiFrame);
    const qsizetype kiwiBytes = AudioEngineRatesTestAccess::kiwiBytes(fixture.engine, kKiwiId);
    fixture.engine.feedKiwiPcmFrame(kKiwiId, kiwiFrame);
    fixture.engine.feedKiwiPcmFrame(QStringLiteral("second-kiwi"), kiwiFrame);
    fixture.engine.feedKiwiPcmFrame(kKiwiId, *collision.produce(tone(24000, 240)));
    check(kiwiBytes == 240 * kFrameBytes
          && AudioEngineRatesTestAccess::kiwiBytes(fixture.engine, kKiwiId) == kiwiBytes
          && AudioEngineRatesTestAccess::kiwiBytes(fixture.engine, QStringLiteral("second-kiwi")) == 0,
          "Kiwi duplicate, duplicate route and live-producer replacement are refused");
    main.invalidate();
    AudioEngineRatesTestAccess::drain(fixture.engine, 0);
    check(AudioEngineRatesTestAccess::kiwiBytes(fixture.engine, kKiwiId) == kiwiBytes,
          "main revocation preserves a different source's pending PCM");
    kiwi.invalidate();
    AudioEngineRatesTestAccess::drain(fixture.engine, 0);
    check(AudioEngineRatesTestAccess::kiwiBytes(fixture.engine, kKiwiId) == 0,
          "Kiwi revocation drains its own pending PCM without a later feed");
}

void kiwiDeviceRateMatrix()
{
    for (int deviceRate : {24000, 44100, 48000}) {
        for (bool legacy : {false, true}) {
            Fixture fixture(deviceRate);
            PcmProducer kiwi;
            if (legacy) {
                fixture.engine.setKiwiSdrAudioEnabled(true);
            } else {
                fixture.engine.setKiwiSdrAudioSourceEnabled(kKiwiId, true);
                check(kiwi.start(PcmPurpose::Auxiliary), "Kiwi matrix producer starts");
            }
            check(AudioEngineRatesTestAccess::prepareKiwi(fixture.engine),
                  "Kiwi matrix processing state initializes");
            for (int packet = 0; packet < 100; ++packet) {
                const QVector<float> samples = tone(24000, 240, packet * 240);
                if (legacy) {
                    fixture.engine.feedKiwiSdrAudioData(bytes(samples));
                } else {
                    fixture.engine.feedKiwiPcmFrame(kKiwiId, *kiwi.produce(samples));
                }
                if (packet >= 40) {
                    fixture.tick();
                }
            }
            fixture.finish();
            const QByteArray output = fixture.output.data();
            std::printf("Kiwi route=%s device=%d outputFrames=%lld\n",
                        legacy ? "legacy" : "typed", deviceRate,
                        static_cast<long long>(output.size() / kFrameBytes));
            check(output.size() / kFrameBytes == deviceRate,
                  "one second Kiwi24 input yields one second at every device rate");
            check(finite(output) && amplitude(output, deviceRate, 0, 1000) > 0.27
                  && amplitude(output, deviceRate, 1, 2300) > 0.15,
                  "Kiwi rate conversion preserves independent stereo frequencies");
        }
    }
}

void processingStateReset()
{
    for (Effect effect : {Effect::Eq, Effect::Nr2, Effect::Rn2}) {
        Fixture fixture(48000);
        PcmProducer producer;
        producer.start(PcmPurpose::Speaker, -1, {24000, PcmLayout::Stereo});
        feed(fixture.engine, producer, QVector<float>(480, 0.0f));
        fixture.tick();
        check(enableEffect(fixture.engine, effect), "state-reset effect enables");
        for (int tick = 0; tick < 20; ++tick) {
            feed(fixture.engine, producer, tone(24000, 240, tick * 240));
            fixture.tick();
        }
        producer.setFormat({48000, PcmLayout::Stereo});
        fixture.output.buffer().clear();
        fixture.output.seek(0);
        for (int tick = 0; tick < 40; ++tick) {
            feed(fixture.engine, producer, QVector<float>(960, 0.0f));
            fixture.tick();
        }
        fixture.finish();
        const QByteArray silence = fixture.output.data();
        const auto* samples = reinterpret_cast<const float*>(silence.constData());
        check(!silence.isEmpty() && std::all_of(samples,
                  samples + silence.size() / static_cast<qsizetype>(sizeof(float)),
                  [](float value) { return std::isfinite(value) && std::abs(value) < 0.00001f; }),
              "producer format change discards enabled-effect and resampler history");

        for (int tick = 0; tick < 20; ++tick) {
            feed(fixture.engine, producer, tone(48000, 480, tick * 480));
            fixture.tick();
        }
        fixture.output.buffer().clear();
        fixture.output.seek(0);
        const std::optional<PcmFrame> gap = producer.produce(QVector<float>(960, 0.0f),
                                                             std::nullopt, true);
        check(gap.has_value(), "explicit discontinuity creates a frame");
        if (gap) {
            fixture.engine.feedPcmFrame(*gap);
        }
        fixture.tick();
        for (int tick = 0; tick < 39; ++tick) {
            feed(fixture.engine, producer, QVector<float>(960, 0.0f));
            fixture.tick();
        }
        fixture.finish();
        check(samePcm(silence, fixture.output.data()),
              "discontinuity resets selected effect to the same silent initial state");
    }
}

void legacyReplacementAndKiwiReset()
{
    Fixture fixture(48000);
    PcmProducer producer;
    producer.start(PcmPurpose::Speaker, -1, {48000, PcmLayout::Stereo});
    feed(fixture.engine, producer, tone(48000, 480));
    producer.invalidate();
    fixture.engine.feedAudioData(bytes(tone(24000, 240)));
    check(AudioEngineRatesTestAccess::producerRate(fixture.engine) == 24000
          && AudioEngineRatesTestAccess::rawBytes(fixture.engine) == 240 * kFrameBytes,
          "legacy24 replacement retires typed48 before choosing its processing rate");

    Fixture kiwiFixture(48000);
    enableEffect(kiwiFixture.engine, Effect::Eq);
    kiwiFixture.engine.setKiwiSdrAudioSourceEnabled(kKiwiId, true);
    AudioEngineRatesTestAccess::prepareKiwi(kiwiFixture.engine);
    PcmProducer kiwi;
    kiwi.start(PcmPurpose::Auxiliary);
    for (int packet = 0; packet < 40; ++packet) {
        kiwiFixture.engine.feedKiwiPcmFrame(kKiwiId,
            *kiwi.produce(QVector<float>(480, 0.3f)));
    }
    kiwiFixture.finish();
    check(!kiwiFixture.output.data().isEmpty(), "Kiwi EQ history is exercised before mute");
    kiwiFixture.engine.setKiwiSdrAudioSourceMuted(kKiwiId, true);
    kiwiFixture.engine.setKiwiSdrAudioSourceMuted(kKiwiId, false);
    AudioEngineRatesTestAccess::prepareKiwi(kiwiFixture.engine);
    kiwiFixture.output.buffer().clear();
    kiwiFixture.output.seek(0);
    for (int packet = 0; packet < 40; ++packet) {
        kiwiFixture.engine.feedKiwiPcmFrame(kKiwiId,
            *kiwi.produce(QVector<float>(480, 0.0f)));
    }
    kiwiFixture.finish();
    const QByteArray muted = kiwiFixture.output.data();
    check(!muted.isEmpty() && muted == QByteArray(muted.size(), '\0'),
          "Kiwi unmute cannot replay the previous client-effect or resampler tail");

    Fixture stopped(48000);
    stopped.engine.setNr2Enabled(true);
    stopped.engine.setKiwiSdrAudioEnabled(true);
    AudioEngineRatesTestAccess::prepareKiwi(stopped.engine);
    stopped.output.close();
    const QByteArray packet = bytes(tone(24000, 240));
    for (int index = 0; index < 300; ++index) {
        stopped.engine.feedKiwiSdrAudioData(packet);
    }
    const qsizetype queued = AudioEngineRatesTestAccess::legacyKiwiRawBytes(stopped.engine);
    check(queued > 0 && queued <= 24000 * kFrameBytes,
          "legacy Kiwi NR2 ingress stays bounded while the device drain is stopped");
}

void fixedProcessingDomains()
{
    for (int deviceRate : {24000, 44100, 48000}) {
        Fixture fixture(deviceRate);
        PcmProducer producer;
        producer.start(PcmPurpose::Speaker, -1, {48000, PcmLayout::Stereo});
        feed(fixture.engine, producer, QVector<float>(960, 0.0f));
        fixture.tick();
        QByteArray cw;
        QObject::connect(&fixture.engine, &AudioEngine::txDecodeAudioReady,
                         &fixture.engine, [&](const QByteArray& pcm) { cw = pcm; });
        fixture.engine.setCwDecodeTxTapEnabled(true);
        AudioEngineRatesTestAccess::renderCwDecodeSilence(fixture.engine, deviceRate);
        check(cw.size() == 240 * kFrameBytes,
              "CW decoder tap remains 24 kHz independently of RX producer/device rate");
        check(AudioEngineRatesTestAccess::recordedCwRate(fixture.engine) == 24000,
              "CW recorder generator retains its fixed 24 kHz contract");
        const qsizetype before = fixture.output.data().size();
        for (int tick = 0; tick < 100; ++tick) {
            fixture.engine.feedDecodedSpeech(bytes(tone(24000, 240, tick * 240, 1000, 1000)));
            fixture.tick();
        }
        fixture.finish();
        const QByteArray rade = fixture.output.data().mid(before);
        std::printf("RADE device=%d frames=%lld amplitude=%f\n", deviceRate,
                    static_cast<long long>(rade.size() / kFrameBytes),
                    amplitude(rade, deviceRate, 0, 1000));
        check(std::abs(rade.size() / kFrameBytes - deviceRate) <= 512
              && amplitude(rade, deviceRate, 0, 1000) > 0.20,
              "RADE speech remains 24 kHz and resamples directly to the device");
        check(AudioEngineRatesTestAccess::radeSourceRate(fixture.engine) == 24000.0,
              "RADE resampler never adopts main producer's 48 kHz rate");
    }
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("audio-engine-rates"));
    qputenv("AETHER_AUTOMATION", "1");
    QCoreApplication application(argc, argv);
    check(settings.isValid(), "isolated settings profile");
    rateMatrix();
    bandwidthAndMono();
    transitionsAndRejection();
    queueBudgetsAndDeviceTransitions();
    concurrentSourcesAndEffects();
    kiwiDeviceRateMatrix();
    processingStateReset();
    legacyReplacementAndKiwiReset();
    fixedProcessingDomains();
    std::printf("AudioEngine rates: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
