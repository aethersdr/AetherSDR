// HL2 host-side CW audio peaking filter (APF) and AGC-off level.
//
// Both controls used to be Flex wire text and nothing else, so on an HL2 —
// whose receive DSP is WDSP on this host — the APF button, the APF level
// slider and the AGC-T slider with AGC Off moved and changed nothing. These
// checks run a real Hl2RxDsp (a real WDSP channel) and MEASURE the audio, so a
// verb that stores its value and never reaches WDSP fails here rather than
// passing on a mirrored field.
//
// Pinned:
//   1. The two level maps (AGC-off level -> fixed gain dB, APF level ->
//      bandwidth), including that the slice model's defaults land on WDSP's
//      own construction values where that was the design intent.
//   2. WdspChannel refuses an APF design WDSP cannot build, and a transmit
//      channel has neither stage.
//   3. AGC-off level: 50 units apart is 30 dB apart in the audio with AGC
//      Off, and makes NO difference with AGC on — WDSP applies the fixed gain
//      only in its mode 0.
//   4. APF: in CW, a tone 200 Hz off the pitch is attenuated relative to one
//      on the pitch when the filter is on; in USB the same request is held but
//      the stage does not run.
//   5. Both survive a configure() (the rebuild a sample-rate change does),
//      and the channel read-back reports what WDSP accepted.
//   6. THE BACKEND HALF. Hl2Backend::setSliceApf / setSliceAgcOffLevel reach
//      the receiver's chain on the I/O thread, publish what the receiver
//      holds through sliceChanged, and dspChains() reports what WDSP took; a
//      CW pitch change moves the APF centre; leaving CW stops the stage and
//      keeps the slice's request. No socket: receiver 0 is handed a configured
//      chain directly, as hl2_unkey_hold_test does with an unconfigured one.

#include "TestSettingsProfile.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisProtocol.h"   // kEp6BlockSamples
#include "core/dsp/FftwPlannerLock.h"
#include "core/dsp/WdspChannel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace AetherSDR::hl2 {

// Give receiver 0 a CONFIGURED chain on the backend's real I/O thread, so every
// verb below travels the queued path it takes in production. Configured on this
// thread before the move, then published to the EP6 fan-out's list, which is
// what dspChains() gathers from.
struct Hl2ApfAgcOffTestAccess {
    static bool attachChain(Hl2Backend& backend, const Hl2RxDsp::Config& cfg)
    {
        auto* dsp = new Hl2RxDsp();
        std::string err;
        if (!dsp->configure(cfg, &err)) {
            std::printf("  configure failed: %s\n", err.c_str());
            delete dsp;
            return false;
        }
        dsp->moveToThread(backend.m_ioThread);
        backend.m_rx[0].dsp = dsp;
        backend.publishIoDsps();
        return true;
    }
    static void tearDown(Hl2Backend& backend) { backend.tearDownReceivers(); }
};

}  // namespace AetherSDR::hl2

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond)
        ++g_failures;
}

static constexpr double kPi = 3.14159265358979323846;
static constexpr int kFs = 48000;
static constexpr double kPitchHz = 600.0;

namespace {

// A tone at +toneHz IN WIRE ORDER (the HPSDR wire is the conjugate of the
// analytic convention — see Hl2RxDsp::processIqBlock and the NB test).
std::vector<std::complex<float>> makeTone(double toneHz, double amp, int samples)
{
    std::vector<std::complex<float>> out(static_cast<std::size_t>(samples));
    for (int n = 0; n < samples; ++n) {
        const double ph = 2.0 * kPi * toneHz * n / kFs;
        out[static_cast<std::size_t>(n)] = std::complex<float>(
            static_cast<float>(amp * std::cos(ph)), static_cast<float>(-amp * std::sin(ph)));
    }
    return out;
}

void feed(Hl2RxDsp& dsp, const std::vector<std::complex<float>>& stream)
{
    for (std::size_t off = 0; off < stream.size(); off += kEp6BlockSamples) {
        const std::size_t n = std::min<std::size_t>(kEp6BlockSamples, stream.size() - off);
        dsp.processIqBlock(std::vector<std::complex<float>>(
            stream.begin() + static_cast<std::ptrdiff_t>(off),
            stream.begin() + static_cast<std::ptrdiff_t>(off + n)));
    }
}

Hl2RxDsp::Config baseConfig(WdspChannel::Mode mode, int agcMode)
{
    Hl2RxDsp::Config cfg;
    cfg.inputSampleRateHz = kFs;
    cfg.audioSampleRateHz = kFs;
    cfg.dspBlockSize = 1024;
    cfg.fftSize = 256;
    cfg.mode = mode;
    // An audio window wide enough to pass both test tones (600 and 800 Hz)
    // in every mode used here, so the only thing that can separate them is
    // the APF.
    cfg.filterLowHz = 300.0;
    cfg.filterHighHz = 1500.0;
    cfg.agcMode = agcMode;
    cfg.blockForOutput = true;   // deterministic for an offline feed
    return cfg;
}

// RMS of the audio a fresh chain produces for one tone, after a settle.
// `prepare` runs between configure() and the feed, which is where a verb under
// test is applied. A fresh object per measurement so no run inherits another's
// filter or AGC state.
template <typename Prepare>
double measureRms(WdspChannel::Mode mode, int agcMode, double toneHz, double amp,
                  Prepare prepare)
{
    Hl2RxDsp dsp;
    std::string err;
    if (!dsp.configure(baseConfig(mode, agcMode), &err)) {
        std::printf("  configure failed: %s\n", err.c_str());
        return std::numeric_limits<double>::quiet_NaN();
    }
    prepare(dsp);
    double sumSquares = 0.0;
    std::size_t count = 0;
    bool collecting = false;
    QObject::connect(&dsp, &Hl2RxDsp::audioReady, &dsp,
                     [&](const std::vector<float>& pcm) {
        if (!collecting)
            return;
        for (float s : pcm) {
            sumSquares += static_cast<double>(s) * s;
            ++count;
        }
    });
    feed(dsp, makeTone(toneHz, amp, kFs / 2));   // settle: filters, AGC
    collecting = true;
    feed(dsp, makeTone(toneHz, amp, kFs));
    return count > 0 ? std::sqrt(sumSquares / static_cast<double>(count)) : 0.0;
}

double db(double ratio) { return 20.0 * std::log10(ratio); }

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("hl2-apf-agc-off"));
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "settings are isolated");
    std::printf("\n  HL2 APF and AGC-off level — host WDSP stages, measured\n\n");

    // ── 1. The maps ──────────────────────────────────────────────────────
    {
        // The default is pinned EXACTLY (==, not a tolerance): before this
        // route AGC Off ran at WdspChannel::Config's 10 dB whatever the
        // slider said, and the default slider must not change what it sounds
        // like.
        check(Hl2RxDsp::agcFixedGainDbForOffLevel(Hl2RxDsp::kDefaultAgcOffLevel)
                      == WdspChannel::Config{}.agcFixedGainDb
                  && WdspChannel::Config{}.agcFixedGainDb == 10.0,
              "the default AGC-off level gives exactly the 10 dB AGC Off always had");
        check(std::abs(Hl2RxDsp::agcFixedGainDbForOffLevel(0) - 4.0) < 1e-9
                  && std::abs(Hl2RxDsp::agcFixedGainDbForOffLevel(100) - 64.0) < 1e-9,
              "AGC-off level spans 4..64 dB at the AGC-T threshold's 0.6 dB/unit");
        check(std::abs(Hl2RxDsp::agcFixedGainDbForOffLevel(-5) - 4.0) < 1e-9
                  && std::abs(Hl2RxDsp::agcFixedGainDbForOffLevel(400) - 64.0) < 1e-9,
              "an out-of-range off level clamps rather than extrapolating");
        check(std::abs(Hl2RxDsp::apfBandwidthHzForLevel(0) - 200.0) < 1e-9
                  && std::abs(Hl2RxDsp::apfBandwidthHzForLevel(50) - 100.0) < 1e-9
                  && std::abs(Hl2RxDsp::apfBandwidthHzForLevel(100) - 50.0) < 1e-9,
              "APF level 0/50/100 -> 200/100/50 Hz; the default lands on RXA.c's 100 Hz");
        bool narrowing = true;
        for (int level = 1; level <= 100; ++level) {
            if (Hl2RxDsp::apfBandwidthHzForLevel(level)
                >= Hl2RxDsp::apfBandwidthHzForLevel(level - 1))
                narrowing = false;
        }
        check(narrowing, "a higher APF level is always a NARROWER peak, as the "
                         "slider's \"APF bandwidth\" tooltip promises");
        // The defaults the receiver starts on must be the ones the slice model
        // starts on, or the first published state disagrees with the control.
        AetherSDR::SliceModel slice(0);
        check(slice.apfLevel() == Hl2RxDsp::kDefaultApfLevel,
              "Hl2RxDsp's default APF level is SliceModel's");
        check(slice.agcOffLevel() == Hl2RxDsp::kDefaultAgcOffLevel,
              "Hl2RxDsp's default AGC-off level is SliceModel's");
    }

    // ── 2. Refusals ──────────────────────────────────────────────────────
    {
        check(!WdspChannel::apfParametersValid(0.0, 100.0, 2.0)
                  && !WdspChannel::apfParametersValid(600.0, 0.0, 2.0)
                  && !WdspChannel::apfParametersValid(600.0, 100.0, 0.0)
                  && !WdspChannel::apfParametersValid(
                      std::numeric_limits<double>::quiet_NaN(), 100.0, 2.0),
              "a zero or non-finite APF centre, bandwidth or gain is refused");
        WdspChannel::Config tx;
        tx.direction = WdspChannel::Direction::Transmit;
        tx.inputSampleRate = kFs;
        tx.dspSampleRate = kFs;
        tx.outputSampleRate = kFs;
        std::string err;
        auto channel = WdspChannel::create(tx, &err);
        check(channel != nullptr, "transmit channel opens");
        if (channel) {
            check(!channel->setApf(true, 600.0, 100.0, 2.0),
                  "setApf is refused on a transmit channel");
            check(!channel->setAgcFixedGain(20.0),
                  "setAgcFixedGain is refused on a transmit channel");
        }
        WdspChannel::Config bad;
        bad.apfBandwidthHz = 0.0;
        check(WdspChannel::create(bad, &err) == nullptr,
              "create() refuses an APF design open() could not push");
    }

    // ── 2b. A no-op setApf takes no lock ─────────────────────────────────
    //
    // The planner lock is process-global and setApf runs on the I/O thread
    // that paces EP2. A call that changes nothing must not wait behind someone
    // else's planning, so it is made HERE while this thread holds that lock:
    // with the early-out it returns at once; without it, it blocks until the
    // lock is released below, and the bounded wait reports that as a failure
    // instead of hanging.
    {
        WdspChannel::Config rxc;   // Receive, 48 kHz, valid as it stands
        rxc.mode = WdspChannel::Mode::Cwu;
        std::string err;
        auto channel = WdspChannel::create(rxc, &err);
        check(channel != nullptr, "receive channel opens");
        if (channel) {
            const double bw = Hl2RxDsp::apfBandwidthHzForLevel(Hl2RxDsp::kDefaultApfLevel);
            check(channel->setApf(true, kPitchHz, bw, 2.0), "APF design accepted");
            std::future<bool> same;
            bool returnedUnderLock = false;
            {
                std::unique_lock<std::mutex> held(AetherSDR::fftwPlannerMutex());
                same = std::async(std::launch::async, [&channel, bw] {
                    return channel->setApf(true, kPitchHz, bw, 2.0);
                });
                returnedUnderLock = same.wait_for(std::chrono::seconds(2))
                                    == std::future_status::ready;
            }
            const bool sameOk = same.get();
            check(returnedUnderLock && sameOk,
                  "an unchanged setApf returns true without taking the planner lock");
            check(channel->setApf(true, kPitchHz + 50.0, bw, 2.0)
                      && channel->config().apfCenterHz == kPitchHz + 50.0,
                  "a changed setApf still reaches WDSP");
        }
    }

    // ── 3. AGC-off level, measured ───────────────────────────────────────
    //
    // Small amplitude so 64 dB of fixed gain does not clip: 1e-4 * 10^3.2 = 0.16.
    constexpr double kAmp = 1e-4;
    {
        const auto atLevel = [&](int agcMode, int level) {
            return measureRms(WdspChannel::Mode::Usb, agcMode, 1000.0, kAmp,
                              [level](Hl2RxDsp& d) { d.setAgcOffLevel(level); });
        };
        const double off10 = atLevel(0, 10);
        const double off60 = atLevel(0, 60);
        const double step = db(off60 / off10);
        std::printf("  AGC off: level 10 -> %.6f, level 60 -> %.6f (%.2f dB)\n",
                    off10, off60, step);
        check(off10 > 0.0 && std::abs(step - 30.0) < 0.5,
              "with AGC Off, 50 units of off level is 30 dB of audio");

        const double med10 = atLevel(3, 10);
        const double med60 = atLevel(3, 60);
        std::printf("  AGC med: level 10 -> %.6f, level 60 -> %.6f (%.2f dB)\n",
                    med10, med60, db(med60 / med10));
        check(med10 > 0.0 && std::abs(db(med60 / med10)) < 0.1,
              "with AGC on, the off level changes nothing — it applies exactly "
              "when AGC is Off");
    }

    // ── 4. APF, measured ─────────────────────────────────────────────────
    //
    // AGC Off so the output is a scaled copy of the IF and the ratio of two
    // tones means something; the AGC would otherwise level them both.
    {
        const auto ratioDb = [&](WdspChannel::Mode mode, bool apfOn) {
            const auto prep = [apfOn](Hl2RxDsp& d) {
                d.setAgcOffLevel(50);   // 34 dB
                d.setApf(apfOn, 50, kPitchHz);
            };
            const double onPitch = measureRms(mode, 0, kPitchHz, 1e-3, prep);
            const double offPitch = measureRms(mode, 0, kPitchHz + 200.0, 1e-3, prep);
            return db(offPitch / onPitch);
        };
        const double cwOff = ratioDb(WdspChannel::Mode::Cwu, false);
        const double cwOn = ratioDb(WdspChannel::Mode::Cwu, true);
        std::printf("  CWU: 800 Hz vs 600 Hz, APF off %.2f dB, on %.2f dB\n", cwOff, cwOn);
        check(cwOn < cwOff - 10.0,
              "in CW the APF lifts the pitch over a tone 200 Hz away by > 10 dB");

        const double usbOff = ratioDb(WdspChannel::Mode::Usb, false);
        const double usbOn = ratioDb(WdspChannel::Mode::Usb, true);
        std::printf("  USB: 800 Hz vs 600 Hz, APF off %.2f dB, requested %.2f dB\n",
                    usbOff, usbOn);
        check(std::abs(usbOn - usbOff) < 0.5,
              "outside CW the APF request is held but the stage does not run");

        // CWL comes out at +pitch in the audio too (the BFO leans the other
        // way on RF, not in the audio), so the same positive centre serves it.
        // The tone is placed on the lower side, where CWL listens.
        Hl2RxDsp dsp;
        std::string err;
        Hl2RxDsp::Config cfg = baseConfig(WdspChannel::Mode::Cwl, 0);
        cfg.filterLowHz = -1500.0;
        cfg.filterHighHz = -300.0;
        check(dsp.configure(cfg, &err), "CWL chain configures");
        dsp.setApf(true, 50, kPitchHz);
        const WdspChannel::Config* c = dsp.channelConfig();
        check(c && c->apfEnabled && std::abs(c->apfCenterHz - kPitchHz) < 1e-9,
              "CWL runs the APF on the positive pitch");
    }

    // ── 5. Survives a rebuild; the read-back reports what WDSP took ──────
    {
        Hl2RxDsp dsp;
        std::string err;
        check(dsp.configure(baseConfig(WdspChannel::Mode::Cwu, 0), &err),
              "CWU chain configures");
        dsp.setApf(true, 80, 700.0);
        dsp.setAgcOffLevel(40);
        const WdspChannel::Config* c = dsp.channelConfig();
        check(c && c->apfEnabled && std::abs(c->apfCenterHz - 700.0) < 1e-9
                  && std::abs(c->apfBandwidthHz - Hl2RxDsp::apfBandwidthHzForLevel(80)) < 1e-9
                  && std::abs(c->apfGain - Hl2RxDsp::kApfGain) < 1e-9,
              "the channel read-back reports the APF design WDSP accepted");
        check(c && std::abs(c->agcFixedGainDb - 28.0) < 1e-9,
              "the channel read-back reports the fixed gain WDSP accepted");

        // What a sample-rate change does: configure() with a caller's fresh
        // Config, which carries neither value.
        Hl2RxDsp::Config rebuilt = baseConfig(WdspChannel::Mode::Cwu, 0);
        rebuilt.inputSampleRateHz = 96000;
        check(dsp.configure(rebuilt, &err), "the chain rebuilds at 96 kHz");
        c = dsp.channelConfig();
        check(c && c->inputSampleRate == 96000, "the rebuilt channel is the new one");
        check(c && c->apfEnabled && std::abs(c->apfCenterHz - 700.0) < 1e-9
                  && std::abs(c->apfBandwidthHz - Hl2RxDsp::apfBandwidthHzForLevel(80)) < 1e-9,
              "the APF survives the rebuild");
        check(c && std::abs(c->agcFixedGainDb - 28.0) < 1e-9,
              "the AGC-off level survives the rebuild");

        // Leaving CW takes the stage out of circuit without forgetting it.
        dsp.setMode(WdspChannel::Mode::Usb);
        c = dsp.channelConfig();
        check(c && !c->apfEnabled && dsp.apfRequested(),
              "leaving CW stops the stage and keeps the request");
        dsp.setMode(WdspChannel::Mode::Cwu);
        c = dsp.channelConfig();
        check(c && c->apfEnabled, "and coming back to CW runs it again");

        // A nonsense pitch keeps the last good centre.
        dsp.setApf(true, 80, 0.0);
        c = dsp.channelConfig();
        check(c && std::abs(c->apfCenterHz - 700.0) < 1e-9,
              "a non-positive centre is not handed to WDSP");
    }

    // ── 6. Through Hl2Backend: seam verb -> receiver -> WDSP, and back ────
    //
    // Read back from dspChains(), which reports what the WDSP channel ACCEPTED
    // rather than what was asked, so a verb that stored its value and never
    // reached the chain fails here. dspChains() blocks on the I/O thread,
    // behind the queued pushes these verbs made.
    {
        using AetherSDR::IRadioBackend;
        using AetherSDR::SliceDelta;
        Hl2Backend backend;
        check(Hl2ApfAgcOffTestAccess::attachChain(
                  backend, baseConfig(WdspChannel::Mode::Usb, 0)),
              "receiver 0 has a configured chain on the I/O thread");
        const auto rx0 = [&backend]() {
            for (const QVariant& v : backend.dspChains()) {
                const QVariantMap m = v.toMap();
                if (m.value(QStringLiteral("chain")).toString() == QLatin1String("rx-wdsp")
                    && m.value(QStringLiteral("receiver")).toInt() == 0)
                    return m;
            }
            return QVariantMap{};
        };
        const auto near = [](const QVariant& v, double want) {
            return v.isValid() && std::abs(v.toDouble() - want) < 1e-6;
        };
        std::optional<bool> pubApf;
        std::optional<int> pubApfLevel, pubOffLevel;
        auto capture = QObject::connect(&backend, &IRadioBackend::sliceChanged, &backend,
                                        [&](int, const SliceDelta& d) {
            if (d.apf) pubApf = *d.apf;
            if (d.apfLevel) pubApfLevel = *d.apfLevel;
            if (d.agcOffLevel) pubOffLevel = *d.agcOffLevel;
        });
        // Stated rather than inherited: Hl2Backend starts at 600 Hz, and the
        // assertions this section replaced assumed 700 -- they never ran, so
        // nothing caught it. The centre checks below read against this value.
        backend.setCwPitch(700);
        backend.setSliceMode(0, QStringLiteral("CW"));
        check(pubApf == false && pubApfLevel == Hl2RxDsp::kDefaultApfLevel
                  && pubOffLevel == Hl2RxDsp::kDefaultAgcOffLevel,
              "a fresh receiver publishes the APF and off-level defaults its chain runs");
        QVariantMap c = rx0();
        check(!c.isEmpty() && c.value(QStringLiteral("apfRun")).toBool() == false
                  && c.value(QStringLiteral("agcFixedGainDb")).toDouble() == 10.0,
              "the chain reports the APF off and AGC Off at exactly the 10 dB "
              "origin/main always ran");
        // Read HERE, before any setSliceApf: the pitch above was set while the
        // receiver was still in USB, and the next setSliceApf would push the
        // pitch itself and hide a stale centre (PR #6050 review).
        check(near(c.value(QStringLiteral("apfCenterHz")), 700.0),
              "a pitch set outside CW is the APF centre on entering CW");

        backend.setSliceApf(0, true, 80);
        backend.setSliceAgcOffLevel(0, 40);
        check(pubApf == true && pubApfLevel == 80 && pubOffLevel == 40,
              "setSliceApf / setSliceAgcOffLevel publish what the receiver now holds");
        c = rx0();
        check(c.value(QStringLiteral("apfRun")).toBool()
                  && near(c.value(QStringLiteral("apfCenterHz")), 700.0)
                  && near(c.value(QStringLiteral("apfBandwidthHz")),
                          Hl2RxDsp::apfBandwidthHzForLevel(80)),
              "in CW the APF runs, centred on the CW pitch, at the level's bandwidth");
        check(near(c.value(QStringLiteral("agcFixedGainDb")), 28.0),
              "the AGC-off level reached WDSP's fixed gain (40 units = 28 dB)");

        backend.setCwPitch(650);
        c = rx0();
        check(near(c.value(QStringLiteral("apfCenterHz")), 650.0),
              "a CW pitch change moves the APF centre with it");

        backend.setSliceMode(0, QStringLiteral("LSB"));
        c = rx0();
        check(!c.value(QStringLiteral("apfRun")).toBool() && pubApf == true,
              "outside CW the stage stops while the slice keeps its APF request");
        // The review's sequence with the APF ON: the pitch moves while the
        // slice is out of CW, and nothing but the mode change follows.
        backend.setCwPitch(750);
        backend.setSliceMode(0, QStringLiteral("CW"));
        c = rx0();
        check(c.value(QStringLiteral("apfRun")).toBool(), "and back in CW it runs again");
        check(near(c.value(QStringLiteral("apfCenterHz")), 750.0),
              "centred on the pitch moved while the slice was out of CW, not the "
              "one it left CW with");
        QObject::disconnect(capture);
        Hl2ApfAgcOffTestAccess::tearDown(backend);
    }

    std::printf("\n  %s — %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
