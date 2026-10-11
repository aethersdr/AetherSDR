// Pins AsrAudioTap's tap-point selection (RFC #4861) end to end: a real
// AudioEngine emitting both presentation feeds, a real AsrAudioTap choosing
// between them, and a real AsrEngine counting what arrives.
//
// AudioEngine emits BOTH feeds for every block, so the tap — not the engine —
// decides which chain reaches the recogniser. Two things follow, and this test
// pins both:
//
//  1. Exactly one chunk reaches the engine per block, whichever point is
//     selected. A tap that ignored the point would deliver two.
//  2. The reason onRxAudio()'s `from != m_tapPoint` guard exists: the feeds are
//     connected Qt::QueuedConnection, and disconnect() does not withdraw an
//     invocation the queue has already accepted. A block emitted from the old
//     point before a live switch therefore still arrives after it, and must be
//     dropped rather than opening the new session with audio from the wrong
//     chain. Delete that guard and the stale-block check goes red, along with
//     the count right after it — the stale block is counted into the next
//     total. Nothing else in the suite notices, because only one feed is
//     connected at a time, so an ignored `from` is invisible until a switch.
//
// Observable: AsrEngine::requestProcess, emitted once per accepted chunk.
// Chosen over backlogChanged() because it counts what the tap handed over and
// does not move as the worker drains it.
//
// Because the connections are queued, every arrangement step is followed by
// processEvents(): without it the tap's slots never run at all.
//
// Socket-free; drives processMixedRxAudioData() through the
// AudioEngineRatesTestAccess friend seam, as audio_engine_rates_test and
// asr_pre_dsp_emit_test do.

#include "TestSettingsProfile.h"
#include "asr/AsrEngine.h"
#include "asr/IAsrBackend.h"
#include "core/AudioEngine.h"
#include "gui/AsrAudioTap.h"
#include "gui/AsrTapPolicy.h"

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QString>
#include <QVector>

#include <cmath>
#include <cstdio>
#include <memory>

namespace AetherSDR {

// Defined per translation unit against AudioEngine's friend declaration, as the
// other headless engine tests do.
class AudioEngineRatesTestAccess {
public:
    static void attach(AudioEngine& engine, QBuffer& output)
    {
        engine.m_rxTimer->stop();
        engine.m_audioDevice = &output;
        engine.setRxDeviceRate(24000);
    }
    static void detach(AudioEngine& engine) { engine.m_audioDevice = nullptr; }
    static void main(AudioEngine& engine, const QByteArray& pcm)
    {
        engine.processMixedRxAudioData(pcm, AudioEngine::RxDspSource::Main, nullptr);
    }
};

} // namespace AetherSDR

using namespace AetherSDR;

namespace {

int g_failures = 0;

void expect(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", description);
    if (!condition) {
        ++g_failures;
    }
}

// Accepts any audio and returns nothing: this test never reaches a decode, it
// only counts how many chunks the tap handed over.
class SilentBackend : public IAsrBackend {
public:
    bool load(const QString&, QString*) override
    {
        m_loaded = true;
        return true;
    }
    bool isLoaded() const override { return m_loaded; }
    AsrTranscript transcribe(const std::vector<float>&, QString*) override { return {}; }
    void unload() override { m_loaded = false; }

private:
    bool m_loaded = false;
};

AsrBackendFactory silentFactory()
{
    return [] { return std::unique_ptr<IAsrBackend>(new SilentBackend); };
}

// Interleaved stereo float32 at the producer rate — the shape every
// processMixedRxAudioData() caller passes.
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

constexpr int kBlocks = 5;

// One arrangement: engine + tap + engine-side chunk counter, all headless.
struct Rig {
    QBuffer sink;
    AudioEngine engine;
    AsrEngine asr{silentFactory()};
    AsrAudioTap tap{&engine, &asr, nullptr};
    int chunks = 0;

    Rig()
    {
        sink.open(QIODevice::ReadWrite);
        AudioEngineRatesTestAccess::attach(engine, sink);
        asr.setEnabled(true);
        QObject::connect(&asr, &AsrEngine::requestProcess,
                         [this](const QVector<float>&, int) { ++chunks; });
        tap.setBaseSpeechRms(0.010f);
    }
    ~Rig() { AudioEngineRatesTestAccess::detach(engine); }

    void push(const QByteArray& pcm, int blocks)
    {
        for (int i = 0; i < blocks; ++i) {
            AudioEngineRatesTestAccess::main(engine, pcm);
        }
    }
};

// The queued tap invocations only run here.
void drain()
{
    QCoreApplication::processEvents();
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("asr-audio-tap"));
    QCoreApplication app(argc, argv);

    const QByteArray input = stereoBlock(480, 0.037f);

    // ---- The selected point is the only one that reaches the engine --------
    {
        Rig rig;
        rig.tap.setEnabled(true);          // defaults to PostDsp
        rig.push(input, kBlocks);
        drain();
        expect(rig.chunks == kBlocks,
               "post-DSP tap delivers one chunk per block, not one per feed");
    }
    {
        Rig rig;
        rig.tap.setTapPoint(AsrTapPoint::PreDsp);
        rig.tap.setEnabled(true);
        rig.push(input, kBlocks);
        drain();
        expect(rig.chunks == kBlocks,
               "pre-DSP tap delivers one chunk per block, not one per feed");
    }

    // ---- A disabled tap delivers nothing ----------------------------------
    {
        Rig rig;
        rig.push(input, kBlocks);
        drain();
        expect(rig.chunks == 0, "a tap that was never enabled delivers nothing");
    }

    // ---- The load-bearing check: a stale block from the old point ----------
    // Emit while the tap is on PostDsp, switch before the queue is drained, and
    // only then let the invocation run. It names PostDsp, the tap now wants
    // PreDsp, so it must be discarded — otherwise the new session opens with a
    // block from the other chain, at a different level and latency.
    {
        Rig rig;
        rig.tap.setEnabled(true);          // PostDsp
        rig.push(input, 1);                // posted, not yet delivered
        rig.tap.setTapPoint(AsrTapPoint::PreDsp);
        drain();
        expect(rig.chunks == 0,
               "a block queued from the old tap point is dropped after a switch");

        // And the new point works immediately afterwards: the guard drops stale
        // blocks without wedging the tap.
        rig.push(input, kBlocks);
        drain();
        expect(rig.chunks == kBlocks,
               "the new tap point delivers normally once the switch is done");
    }

    // ---- A live switch leaves the tap on exactly one feed ------------------
    {
        Rig rig;
        rig.tap.setEnabled(true);
        rig.tap.setTapPoint(AsrTapPoint::PreDsp);
        drain();                            // settle the switch
        expect(rig.tap.tapPoint() == AsrTapPoint::PreDsp, "the live switch took");
        rig.chunks = 0;
        rig.push(input, kBlocks);
        drain();
        expect(rig.chunks == kBlocks,
               "after a live switch the tap still follows exactly one feed");
    }

    std::printf(g_failures == 0 ? "\nASR audio tap: ALL PASS\n"
                                : "\nASR audio tap: %d FAILURE(S)\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
