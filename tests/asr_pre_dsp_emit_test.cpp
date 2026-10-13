// Pins AudioEngine::receivePresentationPreDspAudioReady — the pre-NR fan-out
// Copy Assist's "unprocessed audio" tap point subscribes to (RFC #4861).
//
// Socket-free: drives the real processMixedRxAudioData() against a QBuffer
// sink through the AudioEngineRatesTestAccess friend seam, the same way
// audio_engine_rates_test, audio_engine_pcm_lifetime_test and
// nnr_model_publication_test already construct an AudioEngine headless.
//
// The load-bearing check is the LAST one: an NR processor that is enabled but
// not ready returns early from processMixedRxAudioData(), and the pre-DSP feed
// must keep flowing anyway. Move the emit below the NR if/else and only that
// check goes red — which is the placement RFC #4861 called correctness-
// critical and which nothing else in the suite pins.

#include "TestSettingsProfile.h"
#include "core/AudioEngine.h"
#include "core/ClientComp.h"
#include "core/ClientEq.h"
#include "core/ClientTube.h"

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QString>

#include <cmath>
#include <cstdio>

namespace AetherSDR {

// Defined per translation unit against AudioEngine's friend declaration, as
// the three existing headless engine tests do.
class AudioEngineRatesTestAccess {
public:
    static void attach(AudioEngine& engine, QBuffer& output)
    {
        engine.m_rxTimer->stop();
        engine.m_audioDevice = &output;
        engine.setRxDeviceRate(24000);
    }
    static void detach(AudioEngine& engine) { engine.m_audioDevice = nullptr; }
    // Enabled-but-not-ready RN2: m_rn2 stays null, so the NR branch returns early.
    static void forceRn2NotReady(AudioEngine& engine, bool on)
    {
        engine.m_rn2Enabled.store(on);
    }
    static bool hasMainRn2(const AudioEngine& engine)
    {
        return static_cast<bool>(engine.m_rn2);
    }
    static int producerRate(const AudioEngine& engine)
    {
        return engine.m_rxProducerRate.load();
    }
    static void main(AudioEngine& engine, const QByteArray& pcm)
    {
        engine.processMixedRxAudioData(pcm, AudioEngine::RxDspSource::Main, nullptr);
    }
    static void kiwi(AudioEngine& engine, const QByteArray& pcm)
    {
        engine.processMixedRxAudioData(pcm, AudioEngine::RxDspSource::KiwiSdr, nullptr);
    }
};

} // namespace AetherSDR

namespace {

int g_failures = 0;

void expect(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", description);
    if (!condition) {
        ++g_failures;
    }
}

struct Capture {
    int count = 0;
    QString source;
    QString sourceId;
    int rate = 0;
    int channels = 0;
    QByteArray pcm;
};

// Interleaved stereo float32, the shape every processMixedRxAudioData() caller
// passes. 0.037 peak ≈ 0.026 RMS, the speech level measured off air.
QByteArray stereoBlock(int frames, float amplitude)
{
    QByteArray pcm(frames * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* samples = reinterpret_cast<float*>(pcm.data());
    for (int i = 0; i < frames; ++i) {
        const float v =
            amplitude * std::sin(2.0f * 3.14159265f * 700.0f * static_cast<float>(i) / 24000.0f);
        samples[2 * i] = v;
        samples[2 * i + 1] = v;
    }
    return pcm;
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("asr-pre-dsp-emit"));
    QCoreApplication app(argc, argv);
    using Access = AetherSDR::AudioEngineRatesTestAccess;

    QBuffer sink;
    sink.open(QIODevice::ReadWrite);
    AetherSDR::AudioEngine engine;
    Access::attach(engine, sink);

    Capture pre;
    Capture post;
    QObject::connect(&engine, &AetherSDR::AudioEngine::receivePresentationPreDspAudioReady,
                     [&pre](const QString& source, const QString& sourceId,
                            const QByteArray& pcm, int rate, int channels) {
                         ++pre.count;
                         pre.source = source;
                         pre.sourceId = sourceId;
                         pre.rate = rate;
                         pre.channels = channels;
                         pre.pcm = pcm;
                     });
    QObject::connect(&engine, &AetherSDR::AudioEngine::receivePresentationPostDspAudioReady,
                     [&post](const QString& source, const QString&, const QByteArray& pcm,
                             int rate, int channels) {
                         ++post.count;
                         post.source = source;
                         post.rate = rate;
                         post.channels = channels;
                         post.pcm = pcm;
                     });

    const QByteArray input = stereoBlock(480, 0.037f);

    // ---- Main source, defaults -------------------------------------------
    Access::main(engine, input);
    expect(pre.count == 1 && post.count == 1,
           "one block produces one pre-DSP and one post-DSP emit");
    expect(pre.pcm == input,
           "the pre-DSP payload is the input block, byte for byte");
    expect(pre.rate == Access::producerRate(engine),
           "pre-DSP carries the producer rate, not the output rate");
    expect(pre.channels == 2, "pre-DSP is tagged as stereo");
    expect(pre.source == QStringLiteral("flex"), "a Main block is tagged flex");
    expect(pre.sourceId.isEmpty(), "no external source means an empty sourceId");

    // ---- Legacy Kiwi source ----------------------------------------------
    Access::kiwi(engine, input);
    expect(pre.source == QStringLiteral("kiwi") && pre.rate == 24000,
           "a KiwiSdr block is tagged kiwi at 24 kHz");

    // ---- The load-bearing placement check --------------------------------
    // An enabled NR processor with no instance makes processMixedRxAudioData()
    // return early. The post-DSP feed goes silent; the pre-DSP feed must not,
    // because it is emitted above the NR branch. This is what fails if the
    // emit is ever moved below it.
    {
        const int preBefore = pre.count;
        const int postBefore = post.count;
        Access::forceRn2NotReady(engine, true);
        expect(!Access::hasMainRn2(engine),
               "RN2 is flagged enabled with no filter instance");
        Access::main(engine, input);
        Access::forceRn2NotReady(engine, false);
        expect(post.count == postBefore,
               "post-DSP is silent while RN2 is enabled but not ready");
        expect(pre.count == preBefore + 1,
               "pre-DSP still emits ahead of the NR early return");
    }

    // ---- Both feeds share the open-sink gate ------------------------------
    {
        const int preBefore = pre.count;
        const int postBefore = post.count;
        sink.close();
        Access::main(engine, input);
        expect(pre.count == preBefore && post.count == postBefore,
               "a closed sink silences both feeds together");
    }

    // ---- The static chain makeup the pre-DSP feed is missing --------------
    // Copy Assist's threshold scaling divides by this, so it has to count the
    // stages the operator actually has in the chain and nothing else. EQ master
    // gain, compressor makeup and tube output gain are signal-independent; the
    // gate and pudu are not, and contribute nothing here by design.
    {
        using Stage = AetherSDR::AudioEngine::RxChainStage;
        engine.setRxChainStages({Stage::Eq, Stage::Gate, Stage::Comp, Stage::Tube,
                                 Stage::Pudu});

        expect(std::fabs(engine.rxStaticChainMakeupDb()) < 0.01f,
               "a chain with every stage disabled contributes 0 dB");

        engine.clientCompRx()->setEnabled(true);
        engine.clientCompRx()->setMakeupDb(6.0f);
        expect(std::fabs(engine.rxStaticChainMakeupDb() - 6.0f) < 0.01f,
               "an enabled compressor contributes its makeup gain");

        engine.clientTubeRx()->setEnabled(true);
        engine.clientTubeRx()->setOutputGainDb(3.0f);
        expect(std::fabs(engine.rxStaticChainMakeupDb() - 9.0f) < 0.01f,
               "the tube's output gain adds to it");

        engine.clientEqRx()->setEnabled(true);
        engine.clientEqRx()->setMasterGain(2.0f);   // +6.02 dB
        expect(std::fabs(engine.rxStaticChainMakeupDb() - 15.02f) < 0.02f,
               "the EQ master gain adds as dB, converted from linear");

        // Disabled stages drop out even with their values still set — the
        // operator hears no makeup, so the threshold must not assume any.
        engine.clientCompRx()->setEnabled(false);
        expect(std::fabs(engine.rxStaticChainMakeupDb() - 9.02f) < 0.02f,
               "a disabled stage stops contributing");
        engine.clientCompRx()->setEnabled(true);

        // The load-bearing one: a stage the operator removed from the chain
        // order never runs, so its gain must not be counted even while the
        // module is enabled and configured. Only the walk over the stored
        // order makes this true.
        engine.setRxChainStages({Stage::Eq, Stage::Tube});
        expect(std::fabs(engine.rxStaticChainMakeupDb() - 9.02f) < 0.02f,
               "a stage absent from the chain order contributes nothing");

        // A master gain of zero silences the post-DSP feed: -inf dB, which the
        // scaler turns into "leave the threshold alone".
        engine.setRxChainStages({Stage::Eq});
        engine.clientEqRx()->setMasterGain(0.0f);
        expect(!std::isfinite(engine.rxStaticChainMakeupDb()),
               "an EQ master gain of zero reports -inf, not a finite number");
    }

    Access::detach(engine);

    std::printf(g_failures == 0 ? "\nASR pre-DSP emit: ALL PASS\n"
                                : "\nASR pre-DSP emit: %d FAILURE(S)\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
