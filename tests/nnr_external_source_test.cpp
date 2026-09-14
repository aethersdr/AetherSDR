// #5687 follow-up: ensureExternalKiwiSourceDspState() declared a local
// NnrFilter and moved it into the source, but never called createNnrFilter()
// the way its six siblings do -- so every managed Kiwi source installed a null
// NNR. nnrForSource() then returned nullptr and the RX dispatch dropped the
// block, leaving the source silent while the UI reported NNR active. needNnr
// is computed as !source->nnr, which a null move never clears, so the
// initializer also re-armed on every pass.
//
// Socket-free by construction: this drives the real initializer through the
// existing friend seam. No sink, audio device, socket, radio transport,
// hardware or transmitter is opened.
#include "TestSettingsProfile.h"
#include "core/AudioEngine.h"
#include "core/NnrFilter.h"

#include <QCoreApplication>
#include <QString>
#include <QTimer>

#include <cstdio>
#include <mutex>

namespace AetherSDR {

// Same friend seam as audio_engine_rates_test / audio_engine_pcm_lifetime_test,
// in a separate executable, carrying only the members this test needs.
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
        AudioEngine::ExternalRxAudioSourceState* source =
            engine.externalKiwiSource(id, true);
        source->enabled = true;
    }

    static bool initializeSource(AudioEngine& engine, const QString& id)
    {
        return engine.ensureExternalKiwiSourceDspState(id);
    }

    static NnrFilter* sourceNnr(AudioEngine& engine, const QString& id)
    {
        std::lock_guard<std::recursive_mutex> lock(engine.m_dspMutex);
        AudioEngine::ExternalRxAudioSourceState* source =
            engine.externalKiwiSource(id, false);
        return source ? source->nnr.get() : nullptr;
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

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("nnr-external-source-test"));
    QCoreApplication app(argc, argv);

    using Access = AetherSDR::AudioEngineRatesTestAccess;

    AetherSDR::AudioEngine engine;
    Access::stopTimer(engine);

    // needNnr requires the main-RX filter to exist, so enable NNR first.
    engine.setNnrEnabled(true);
    Access::waitInitialization(engine);
    check(engine.nnrEnabled(), "NNR enables on the main RX path");

    const QString id = QStringLiteral("nnr-external-kiwi");
    Access::enableSource(engine, id);
    check(Access::sourceNnr(engine, id) == nullptr,
          "a fresh managed Kiwi source starts with no NNR filter");

    check(Access::initializeSource(engine, id),
          "external Kiwi DSP state initializes without error");

    // The regression: before the fix this was nullptr, and every later pass
    // recomputed needNnr as true because the null move never cleared it.
    AetherSDR::NnrFilter* nnr = Access::sourceNnr(engine, id);
    check(nnr != nullptr,
          "the managed Kiwi source is given an NNR filter (it got nullptr)");
    check(nnr != nullptr && nnr->isValid(),
          "and that filter is a live WDSP instance, not a failed construction");

    // Re-arming is gated on `!source->nnr` (AudioEngine.cpp, needNnr), so the
    // non-null assertion above is what actually pins it -- there is no separate
    // observable. Deliberately NOT asserting dspInitializationPending here: it
    // is an in-flight flag cleared unconditionally at the end of the function,
    // so it reads false against the unfixed code too and would pin nothing.
    // What is checkable is that further passes are idempotent.
    check(Access::initializeSource(engine, id),
          "a second initialization pass still reports success");
    check(Access::sourceNnr(engine, id) == nnr,
          "and the installed filter is not churned on the second pass");
    check(Access::initializeSource(engine, id),
          "a third pass still reports success");
    check(Access::sourceNnr(engine, id) == nnr,
          "and still does not churn the filter");

    // Disabling NNR must take the per-source filter with it.
    engine.setNnrEnabled(false);
    check(Access::sourceNnr(engine, id) == nullptr,
          "disabling NNR clears the managed Kiwi source's filter");

    if (failures == 0) {
        std::printf("nnr_external_source_test: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
