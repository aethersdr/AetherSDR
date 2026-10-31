// The age gate of the ADC pairing where Hl2Backend::healthSnapshot() applies
// it: the pairing gets the freshness of the slice peak the snapshot displays,
// taken at the snapshot's own instant. hl2_adc_pairing_test owns the rule.
// Socket-free: link and telemetry arrive through MetisClient's signals, a real
// Hl2RxDsp stamps the peak, and the snapshot is read at chosen instants.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/hl2/Hl2AdcPairing.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>
#include <QEvent>
#include <QMetaObject>
#include <QString>
#include <QVariant>

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace AetherSDR::hl2 {

struct Hl2AdcAgeTestAccess {
    // Receiver 0 gets a configured chain on this thread, owned by the backend
    // (its destructor deletes m_rx[].dsp), so the test feeds it directly.
    static Hl2RxDsp* attachChain(Hl2Backend& backend)
    {
        if (backend.m_rx.empty()) {
            return nullptr;
        }
        auto dsp = std::make_unique<Hl2RxDsp>(nullptr);
        Hl2RxDsp::Config cfg;
        cfg.inputSampleRateHz = 48000;
        cfg.audioSampleRateHz = 48000;
        cfg.dspBlockSize = 1024;
        cfg.fftSize = 256;
        cfg.blockForOutput = true;
        std::string err;
        if (!dsp->configure(cfg, &err)) {
            std::fprintf(stderr, "configure failed: %s\n", err.c_str());
            return nullptr;
        }
        backend.m_rx[0].dsp = dsp.release();
        return backend.m_rx[0].dsp;
    }
    // The handlers are queued onto the backend's thread, and this test never
    // returns to an event loop to service them.
    static void linkUp(Hl2Backend& backend)
    {
        QMetaObject::invokeMethod(backend.m_metis, "linkUp", Qt::BlockingQueuedConnection);
        QCoreApplication::sendPostedEvents(&backend, QEvent::MetaCall);
    }
    static void telemetry(Hl2Backend& backend, const Hl2Telemetry& t)
    {
        MetisClient* metis = backend.m_metis;
        QMetaObject::invokeMethod(metis, [metis, t] {
            emit metis->telemetryUpdated(t);
        }, Qt::BlockingQueuedConnection);
        QCoreApplication::sendPostedEvents(&backend, QEvent::MetaCall);
    }
    static const Hl2RxDsp* chain(const Hl2Backend& backend)
    {
        return backend.m_rx.empty() ? nullptr : backend.m_rx[0].dsp;
    }
    // The pairing's other liveness input, as healthSnapshot() evaluates it.
    static bool samplingGateOpen(const Hl2Backend& backend)
    {
        const Hl2RxDsp* dsp = chain(backend);
        return dsp && backend.m_sliceSampling.applied(dsp->adcPeakObservedAtNs());
    }
    static AetherSDR::IRadioBackend::HealthSnapshot snapshotAt(const Hl2Backend& backend,
                                                               std::int64_t nowNs)
    {
        return backend.healthSnapshot(nowNs);
    }
};

}  // namespace AetherSDR::hl2

using namespace AetherSDR;
using Access = AetherSDR::hl2::Hl2AdcAgeTestAccess;

namespace {

int g_failures = 0;
void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

constexpr double kPi = 3.14159265358979323846;
constexpr int kFs = 48000;
constexpr std::size_t kBlock = 1024;
constexpr std::int64_t kNsPerMs = 1'000'000;
// 20 dB below wire full scale: a real reading, and outside kSliceHotHeadroomDb,
// so with the flag clear the pairing is BothClear.
constexpr float kToneAmplitude = 0.1f;
// WDSP stamps a peak on its output side, a few blocks after the first one in.
constexpr int kMaxBlocksForOneSample = 16;

void feedOneBlock(hl2::Hl2RxDsp& dsp, double& phase)
{
    std::vector<std::complex<float>> block(kBlock);
    for (std::size_t n = 0; n < kBlock; ++n) {
        block[n] = kToneAmplitude * std::complex<float>(static_cast<float>(std::cos(phase)),
                                                        static_cast<float>(-std::sin(phase)));
        phase += 2.0 * kPi * 1000.0 / kFs;
    }
    dsp.processIqBlock(std::move(block));
}

const QString kFlagRow = QStringLiteral("adcOverload");
const QString kPeakRow = QStringLiteral("adcSlicePeakDbfs0");
const QString kAgeRow = QStringLiteral("adcSliceObservedAgoMs0");
const QString kPairingRow = QStringLiteral("adcPairing0");

// The pairing's other inputs, asserted on every read. The flag is the one a
// second clock decides: the link state reads a QElapsedTimer the test cannot
// set, and the flag is absent unless that reads Streaming.
void checkOtherInputs(const hl2::Hl2Backend& backend,
                      const IRadioBackend::HealthSnapshot& snap, std::int64_t stamp)
{
    check(snap.values.value(kFlagRow) == QVariant(false),
          "the snapshot carries the overload flag, clear: the link reads Streaming");
    check(snap.values.contains(kPeakRow), "the snapshot carries a real slice peak");
    check(Access::samplingGateOpen(backend), "the sampling gate is open");
    const hl2::Hl2RxDsp* dsp = Access::chain(backend);
    check(dsp && dsp->adcPeakObservedAtNs() == stamp, "the held peak's stamp has not moved");
    check(snap.order.contains(kPairingRow) && snap.labels.contains(kPairingRow),
          "the pairing row keeps its place and label");
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-hl2-adc-age-gate"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    hl2::Hl2Backend backend;
    hl2::Hl2RxDsp* dsp = Access::attachChain(backend);
    check(dsp != nullptr, "receiver 0 holds a configured chain");
    if (!dsp) {
        return 1;
    }
    check(dsp->adcPeakObservedAtNs() == 0, "control: no block processed, no stamp");

    double phase = 0.0;
    for (int i = 0; i < kMaxBlocksForOneSample && dsp->adcPeakObservedAtNs() == 0; ++i) {
        feedOneBlock(*dsp, phase);
    }
    const std::int64_t stamp = dsp->adcPeakObservedAtNs();
    const std::optional<double> peak = dsp->adcPeakDbfs();
    check(stamp != 0, "an unmuted block through the chain stamps a slice peak");
    check(peak && hl2::adcMeterReadingIsReal(*peak), "and the peak is a real reading");
    if (stamp == 0 || !peak) {
        return 1;
    }
    check(hl2::sliceHeadroomDb(*peak) > hl2::kSliceHotHeadroomDb,
          "the tone sits outside the hot band, so a clear flag pairs as BothClear");

    // No link yet: the flag is absent, so the pairing cannot be made at any age.
    {
        const auto snap = Access::snapshotAt(backend, stamp);
        check(!snap.values.contains(kFlagRow), "control: no link, no overload flag");
        check(!snap.values.contains(kPairingRow), "control: no flag, no pairing");
        check(snap.values.value(kAgeRow) == QVariant(qulonglong{0}),
              "the age row needs no link: 0 ms at the stamp's own instant");
    }

    Access::linkUp(backend);
    hl2::Hl2Telemetry t;
    t.adcOverload = false;
    Access::telemetry(backend, t);
    check(Access::chain(backend) == dsp, "the link edge kept receiver 0's chain");

    // The two instants differ by one nanosecond and nothing else is touched
    // between the reads: the last one for which the age truncates to
    // kSliceStaleMs, and the first for which it is kSliceStaleMs + 1.
    const std::int64_t lastCurrent = stamp + hl2::kSliceStaleMs * kNsPerMs + (kNsPerMs - 1);
    const std::int64_t firstStale = stamp + (hl2::kSliceStaleMs + 1) * kNsPerMs;
    check(firstStale - lastCurrent == 1, "the two instants are one nanosecond apart");

    const QString paired =
        QStringLiteral("agree — no converter overload, slice %1 dB below full scale")
            .arg(QString::number(hl2::sliceHeadroomDb(*peak), 'f', 1));

    const auto readCurrent = [&](const char* ageWhat, const char* pairingWhat) {
        const auto snap = Access::snapshotAt(backend, lastCurrent);
        checkOtherInputs(backend, snap, stamp);
        check(snap.values.value(kAgeRow)
                  == QVariant(static_cast<qulonglong>(hl2::kSliceStaleMs)),
              ageWhat);
        check(snap.values.value(kPairingRow) == QVariant(paired), pairingWhat);
    };

    readCurrent("at kSliceStaleMs and 999999 ns the age row reads kSliceStaleMs",
                "a peak exactly kSliceStaleMs old is paired with the live flag");
    {
        const auto snap = Access::snapshotAt(backend, firstStale);
        checkOtherInputs(backend, snap, stamp);
        check(snap.values.value(kAgeRow)
                  == QVariant(static_cast<qulonglong>(hl2::kSliceStaleMs + 1)),
              "one nanosecond later the age row reads kSliceStaleMs + 1");
        check(!snap.values.contains(kPairingRow),
              "a peak kSliceStaleMs + 1 old is not paired: the row reads not reported");
    }
    // Again after the stale read, so the stale verdict is the instant's doing
    // and not something that closed in between.
    readCurrent("back at the earlier instant the age row reads kSliceStaleMs again",
                "and the same peak pairs again");

    std::fprintf(stderr, "hl2_adc_age_gate_test: %s\n",
                 g_failures == 0 ? "all checks passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
