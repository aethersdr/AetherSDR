// #5687 follow-up, defect 5: AudioEngine::nnrModel() published a slot that
// could never catch up. setNnrModel() stores m_nnr->modelSlot(), but NnrFilter
// applies a requested switch on the AUDIO thread inside process() -- so the
// store on the main thread reads the pre-switch slot, and nothing refreshed it
// afterwards. createNnrFilter() seeded from that same lagging value, so a
// sample-rate change rebuilt NNR on the superseded model.
//
// The fix republishes the applied slot from the RX path after the block in
// which the switch lands. This pins that: with the republish removed, the
// published slot stays behind the one the filter actually applied.
//
// Socket-free: drives the real processMixedRxAudioData() against a QBuffer
// sink. No audio device, socket, radio transport, hardware or transmitter.
#include "TestSettingsProfile.h"
#include "core/AudioEngine.h"
#include "core/NnrFilter.h"
#include "core/NnrSettings.h"

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QTimer>

#include <cstdio>
#include <mutex>

namespace AetherSDR {

class AudioEngineRatesTestAccess {
public:
    static void stopTimer(AudioEngine& engine) { engine.m_rxTimer->stop(); }

    static void attachMemoryOutput(AudioEngine& engine, QBuffer& output)
    {
        engine.m_audioDevice = &output;
        engine.setRxDeviceRate(24000);
    }

    static void detachMemoryOutput(AudioEngine& engine)
    {
        engine.m_audioDevice = nullptr;
    }

    // The slot WDSP actually has live, straight off the main-RX filter.
    static int mainNnrAppliedSlot(AudioEngine& engine)
    {
        std::lock_guard<std::recursive_mutex> lock(engine.m_dspMutex);
        return engine.m_nnr ? engine.m_nnr->modelSlot() : -1;
    }

    static bool hasMainNnr(AudioEngine& engine)
    {
        std::lock_guard<std::recursive_mutex> lock(engine.m_dspMutex);
        return static_cast<bool>(engine.m_nnr);
    }

    // One block down the real main-RX path, which is where the republish lives.
    static void processMainBlock(AudioEngine& engine, const QByteArray& pcm)
    {
        engine.processMixedRxAudioData(pcm, AudioEngine::RxDspSource::Main, nullptr);
    }
};

} // namespace AetherSDR

namespace {

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

QByteArray stereoBlock(int frames)
{
    QByteArray pcm(frames * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* s = reinterpret_cast<float*>(pcm.data());
    for (int i = 0; i < frames; ++i) {
        // Content is irrelevant to the published slot; it only has to be a
        // non-empty block so NnrFilter::process() applies pending parameters.
        const float v = 0.05f * static_cast<float>((i % 64) - 32) / 32.0f;
        s[2 * i] = v;
        s[2 * i + 1] = v;
    }
    return pcm;
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("nnr-model-publication-test"));
    QCoreApplication app(argc, argv);

    using Access = AetherSDR::AudioEngineRatesTestAccess;

    QBuffer sink;
    sink.open(QIODevice::ReadWrite);

    AetherSDR::AudioEngine engine;
    Access::stopTimer(engine);
    Access::attachMemoryOutput(engine, sink);

    engine.setNnrEnabled(true);
    check(engine.nnrEnabled(), "NNR enables on the main RX path");
    check(Access::hasMainNnr(engine), "a main-RX NNR filter exists");

    // Ask for the other model. WDSP reports the slot it actually selected, so
    // read that rather than assuming the request was honoured by this build.
    const int startingSlot = engine.nnrModel();
    const int requested = startingSlot == 0 ? 1 : 0;
    engine.setNnrModel(requested);

    const QByteArray block = stereoBlock(480);
    for (int i = 0; i < 4; ++i) {
        Access::processMainBlock(engine, block);
    }

    const int applied = Access::mainNnrAppliedSlot(engine);
    const int published = engine.nnrModel();

    // The property under test: what nnrModel() reports is what the filter has
    // live. Before the fix the published value stayed at the pre-switch slot
    // while the filter had already moved.
    check(published == applied,
          "nnrModel() converges to the slot the filter actually applied");

    // Guard against the test passing vacuously on a build where WDSP refused
    // the requested slot and nothing ever moved.
    check(applied != startingSlot,
          "WDSP honoured the model switch, so convergence was actually exercised");

    // New instances must seed from the persisted request, not the published
    // value -- this is the half that made a sample-rate change revert the model.
    check(AetherSDR::NnrSettings::model() == requested,
          "setNnrModel() persists the request for createNnrFilter() to seed from");

    Access::detachMemoryOutput(engine);
    engine.setNnrEnabled(false);

    if (failures == 0) {
        std::printf("nnr_model_publication_test: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
