// Unit test for RtlSdrBackend and RtlSdrDiscovery (Issue #4797).
// Verifies capabilities declaration, state restore contract, signal deltas,
// and discovery availability.

#include "core/backends/rtl/RtlSdrBackend.h"
#include "core/backends/rtl/RtlSdrDdc.h"
#include "core/backends/rtl/RtlSdrWorker.h"
#include "core/RtlSdrDiscovery.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <cstdio>
#include <memory>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

#ifdef AETHER_BACKEND_RTL
    auto backend = std::make_unique<rtl::RtlSdrBackend>();
    check(backend != nullptr, "RtlSdrBackend instantiation");

    // 1. Check capabilities declaration (Principle VI: receive-only)
    const auto caps = backend->capabilities();
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
    bool waterfallEmitted = false;
    bool audioEmitted = false;

    QObject::connect(&ddc, &rtl::RtlSdrDdc::spectrumFrameReady, [&spectrumEmitted](int panId, const QByteArray& frame) {
        Q_UNUSED(panId);
        if (!frame.isEmpty()) {
            spectrumEmitted = true;
        }
    });

    QObject::connect(&ddc, &rtl::RtlSdrDdc::waterfallRowReady, [&waterfallEmitted](int panId, const QByteArray& row) {
        Q_UNUSED(panId);
        if (!row.isEmpty()) {
            waterfallEmitted = true;
        }
    });

    QObject::connect(&ddc, &rtl::RtlSdrDdc::audioFrameReady, [&audioEmitted](const QByteArray& pcm) {
        if (!pcm.isEmpty()) {
            audioEmitted = true;
        }
    });

    // Feed 4096 synthetic complex float IQ samples
    QVector<std::complex<float>> syntheticSamples(4096, std::complex<float>(0.5f, 0.5f));
    ddc.processIqData(syntheticSamples);

    check(spectrumEmitted, "RtlSdrDdc emitted spectrumFrameReady");
    check(waterfallEmitted, "RtlSdrDdc emitted waterfallRowReady");
    check(audioEmitted, "RtlSdrDdc emitted audioFrameReady");

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
