// End-to-end transmit proof against hpsdrsim.
//
// hpsdrsim models the PureSignal feedback path: while PTT is asserted it feeds
// the TX IQ it received BACK into the RX stream, scaled by TX drive and with
// simulated IM3. So if we key, set a drive level and transmit a tone, that tone
// must reappear in our own spectrum at the offset we transmitted it on.
//
// That closes the loop the unit tests cannot: hl2_tx_gate_test proves the right
// BYTES leave, but only the simulator can show the radio actually received them
// and acted on them.
//
// SIM ONLY, deliberately — and now ENFORCED rather than asserted. This test keys
// the transmitter, so pointing it at real hardware would radiate. It used to
// trust a hardcoded LAN address to BE the simulator; findSimulator() below
// checks the responder's identity instead, and the default host is loopback.
// If nothing answers, the test SKIPS rather than fails, so a machine without the
// simulator running does not get a spurious red — exiting 77, which the
// SKIP_RETURN_CODE on its add_test turns into an honest ctest "Skipped" rather
// than a "Passed" that measured nothing.
//
// WHICH SIDE OF CENTRE A TONE LANDS ON. The loop conjugates TWICE. Hl2TxDsp
// conjugates the modulator output for the wire (the HPSDR wire has the opposite
// handedness to the analytic convention); the simulator feeds that IQ back
// verbatim; Hl2RxDsp conjugates the receive stream back before it reaches the
// panadapter FFT. Two conjugations cancel, so the spectrum this test reads is in
// the ANALYTIC convention: a tone transmitted 5 kHz above the carrier appears
// 5 kHz ABOVE centre.
//
// It did not always. The receive-side conjugation arrived in #4471, to fix a
// panadapter that drew every signal mirrored — on 40 m it put FT8 below a
// correctly-drawn DIGU cursor. Before that the display carried the raw wire and
// the fed-back tone read BELOW centre. This test was written against that older
// display in #4466 and went on asserting the negative offset afterwards. That
// staleness — not transmit — is the whole of the "fails on transmit-sideband
// checks" that HERMES.md carried as pre-existing.
//
// BUT cancelling conjugations mean this loopback ALONE cannot prove absolute
// sideband: a handedness error at BOTH ends still cancels. That is exactly how a
// wrong-sideband transmitter once passed its own tests, and it took an operator
// with a second receiver to catch it. So before believing anything the
// transmitter puts in the spectrum, we pin the RECEIVE end against a signal that
// never went through the transmitter — the simulator's own synthetic scene. See
// the scene anchor below.

#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/MetisProtocol.h"

#include "TestDspBuildWait.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QUdpSocket>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

using namespace AetherSDR;
using AetherSDR::hl2::Hl2Backend;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

static void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// ctest's skip convention, declared as SKIP_RETURN_CODE on the add_test. Exiting
// 0 on a skip is what let this test read as Passed to every automated consumer
// while having measured nothing — and with the loopback default below, the
// no-simulator case is the ordinary one, not the exception.
constexpr int kSkipExit = 77;

enum class Probe { NoReply, Unreadable, NotSimulator, Simulator };

// hpsdrsim builds its discovery reply with a synthetic MAC written byte by byte
// in its source: AA BB CC DD <radio-type> FF. No HPSDR board ships an
// AA:BB:CC:DD OUI, so this is a dependable "simulator, not a radio" fingerprint
// — and it is the gate that keeps a test which KEYS A TRANSMITTER off real
// hardware.
//
// It replaces a bare "did anything answer at 192.168.1.12". That address is not
// the simulator's by construction; it is only where one happened to run once. On
// another network it is somebody's actual radio. It was also, on the author's
// LAN, silently reaching a simulator on a DIFFERENT machine — which is where the
// "fails non-deterministically" reputation came from, since the answers depended
// on what that other machine was doing at the time.
//
// Which hpsdrsim: the g0orx/pihpsdr build HERMES.md §7 pins as the fixture. It
// writes those bytes at hpsdrsim.c:628-633. Current upstream (dl1ycf/pihpsdr)
// uses a different synthetic MAC, so a simulator built from that one reads as
// NotSimulator and this test skips rather than running against it.
static Probe findSimulator(const QString& host, AetherSDR::hl2::DiscoveryReply* out)
{
    const QHostAddress target(host);
    QUdpSocket s;
    if (!s.bind(QHostAddress(QHostAddress::AnyIPv4), 0))
        return Probe::NoReply;
    const auto req = AetherSDR::hl2::discoveryRequest();
    s.writeDatagram(reinterpret_cast<const char*>(req.data()),
                    static_cast<qint64>(req.size()), target,
                    AetherSDR::hl2::kMetisPort);

    // Read until the deadline instead of believing the first datagram, and
    // ignore anything that did not come from the host we probed. Taking the
    // first packet is a narrow version of this test's own root cause: one stray
    // sender consumes the window, and the answer that decides whether we may key
    // never gets looked at.
    QElapsedTimer clock;
    clock.start();
    bool answeredUnparseable = false;
    while (true) {
        const qint64 remaining = 1500 - clock.elapsed();
        if (remaining <= 0 || !s.waitForReadyRead(static_cast<int>(remaining)))
            break;
        while (s.hasPendingDatagrams()) {
            QByteArray dg(2048, 0);
            QHostAddress from;
            const qint64 got = s.readDatagram(dg.data(), dg.size(), &from);
            if (got <= 0)
                continue;
            if (!from.isEqual(target, QHostAddress::TolerantConversion))
                continue;
            const auto reply = AetherSDR::hl2::parseDiscoveryReply(
                std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(dg.constData()),
                    static_cast<std::size_t>(got)));
            // Answered, but not as a discovery reply. Kept distinct from the
            // MAC mismatch below so the skip message can say which it was —
            // reporting an all-zero MAC for a packet that never parsed sends
            // whoever reads it looking for a device that does not exist.
            if (!reply) {
                answeredUnparseable = true;
                continue;
            }
            if (out)
                *out = *reply;

            const auto& mac = reply->mac;
            const bool synthetic = mac[0] == 0xAA && mac[1] == 0xBB && mac[2] == 0xCC
                                && mac[3] == 0xDD && mac[5] == 0xFF;
            return synthetic ? Probe::Simulator : Probe::NotSimulator;
        }
    }
    return answeredUnparseable ? Probe::Unreadable : Probe::NoReply;
}

// The IQ sample rate this test runs the receiver at, and therefore the span the
// spectrum frames cover. Declared once because every expected-bin computation
// derives from it.
constexpr int kIqRateHz = 48000;

// hpsdrsim's synthetic receive scene: two tones it generates itself into
// toneItab/toneQtab, at offsets fixed relative to the ADC rather than the NCO,
// present whether or not we are keyed.
//
// It builds them as I = sin(theta), Q = cos(theta) — so I + jQ = j*exp(-j*theta),
// a NEGATIVE frequency on the wire, and a receive path that conjugates correctly
// draws them ABOVE centre. That is the anchor: these tones reach the spectrum
// without passing through the transmitter, so their side of centre pins the
// RECEIVE end's handedness on its own. With it pinned, the transmit assertions
// below can no longer be satisfied by a matched pair of errors.
constexpr double kSceneToneLowHz = 800.0;
constexpr double kSceneToneHighHz = 4000.0;

// Bin of a baseband offset in the displayed spectrum, which is fftshifted (DC at
// the centre bin) and in the analytic convention (above the carrier is above
// centre). The ONE place that sign convention is written down.
static int binForOffset(double offsetHz, int n)
{
    return n / 2 + static_cast<int>(
        std::lround(offsetHz * n / static_cast<double>(kIqRateHz)));
}

// Strongest level within +/-halfWidth bins of centreBin. Offsets rarely land on
// an exact bin — 800 Hz is 17.07 bins at this rate — so a bare lookup reads the
// shoulder of the tone rather than its peak.
static float peakNear(const std::vector<float>& spec, int centreBin, int halfWidth)
{
    const int n = static_cast<int>(spec.size());
    const int lo = std::max(0, centreBin - halfWidth);
    const int hi = std::min(n - 1, centreBin + halfWidth);
    float best = -300.0f;
    for (int i = lo; i <= hi; ++i)
        best = std::max(best, spec[static_cast<std::size_t>(i)]);
    return best;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<SliceDelta>();

    // Loopback by default: a simulator on this machine is the normal case, and
    // 127.0.0.1 cannot be somebody's radio. Override to reach a simulator on
    // another box — the fingerprint check still applies there.
    const QString simHost = qEnvironmentVariableIsSet("AETHER_HL2_SIM_HOST")
        ? qEnvironmentVariable("AETHER_HL2_SIM_HOST")
        : QStringLiteral("127.0.0.1");
    // QHostAddress parses IP literals only — it does not resolve names. A
    // hostname silently becomes a null address, the probe goes nowhere, and the
    // result is a skip that reads exactly like "the simulator is down". Say so
    // instead of leaving somebody to debug a simulator that was running fine.
    if (QHostAddress(simHost).isNull()) {
        std::fprintf(stderr,
            "hl2_tx_loopback_test: SKIPPED — AETHER_HL2_SIM_HOST=\"%s\" is not an "
            "IP literal. Give the simulator's address; names are not resolved.\n",
            qPrintable(simHost));
        return kSkipExit;
    }

    AetherSDR::hl2::DiscoveryReply reply;
    switch (findSimulator(simHost, &reply)) {
    case Probe::NoReply:
        std::fprintf(stderr,
            "hl2_tx_loopback_test: SKIPPED — nothing answering discovery at %s. "
            "Start hpsdrsim (-hermeslite2 -P1), or set AETHER_HL2_SIM_HOST.\n",
            qPrintable(simHost));
        return kSkipExit;
    case Probe::Unreadable:
        std::fprintf(stderr,
            "hl2_tx_loopback_test: SKIPPED — %s answered the discovery probe, but "
            "the reply did not parse as a Metis discovery response. This test keys "
            "the transmitter, so it refuses to run against an unidentified "
            "responder.\n",
            qPrintable(simHost));
        return kSkipExit;
    case Probe::NotSimulator:
        // Deliberately a SKIP and not a failure: the responder is somebody's
        // radio or another board, and the correct outcome is to leave it alone,
        // not to key it and not to redden a suite over it.
        std::fprintf(stderr,
            "hl2_tx_loopback_test: SKIPPED — %s answered with MAC "
            "%02X:%02X:%02X:%02X:%02X:%02X, which is not hpsdrsim's synthetic "
            "AA:BB:CC:DD:xx:FF. This test keys the transmitter, so it refuses to "
            "run against anything that might be a real radio.\n",
            qPrintable(simHost), reply.mac[0], reply.mac[1], reply.mac[2],
            reply.mac[3], reply.mac[4], reply.mac[5]);
        return kSkipExit;
    case Probe::Simulator:
        // Somebody else's session. hpsdrsim serves one client: connecting tears
        // down its EP6 handler and re-points it at us, and this test then keys
        // PTT. Leaving it alone is the same call as the NotSimulator arm — and
        // it is the collision HERMES.md used to warn about, now that the local
        // simulator is the default target rather than an accident.
        if (reply.streaming) {
            std::fprintf(stderr,
                "hl2_tx_loopback_test: SKIPPED — the simulator at %s reports it is "
                "already streaming to another client (discovery status 0x03). "
                "Taking its session over would break that client, and this test "
                "then keys PTT.\n",
                qPrintable(simHost));
            return kSkipExit;
        }
        break;
    }

    // Transmit must be permitted for this to mean anything. This process sets no
    // AETHER_AUTOMATION, so it is an interactive run and the gate is open.
    qunsetenv("AETHER_AUTOMATION");

    Hl2Backend backend;
    check(backend.capabilities().canTransmit,
          "transmit available (interactive run)");

    std::vector<float> lastSpectrum;
    QObject::connect(&backend, &IRadioBackend::spectrumFrameReady, &backend,
                     [&](int, const QByteArray& ba) {
        lastSpectrum.assign(reinterpret_cast<const float*>(ba.constData()),
                            reinterpret_cast<const float*>(ba.constData())
                                + ba.size() / sizeof(float));
    });

    RadioConnectRequest req;
    req.host = simHost;
    // PIN the IQ rate rather than inheriting the backend's default. Every bin
    // arithmetic below scales with it, and this test used to hardcode 48000 while
    // silently depending on the default happening to be 48 kHz — so raising that
    // default to the widest span the hardware offers moved every expected bin and
    // three assertions failed for a reason that had nothing to do with transmit.
    //
    // 48 kHz specifically: it gives the finest bin spacing of the four rates, so
    // the sideband and floor margins this test measures stay as tight as they
    // were. kIqRateHz is the ONE place the rate appears.
    req.params[QStringLiteral("sampleRateHz")] = kIqRateHz;
    backend.connectRadio(req);
    AetherSDR::test::awaitDspBuild("hl2_tx_loopback_test",
                                  [&] { return backend.isConnected(); });
    spin(2500);   // unchanged settle budget; only the connect wait moved out of it
    check(backend.isConnected(), "connected to the simulator");
    if (!backend.isConnected()) {
        std::fprintf(stderr, "hl2_tx_loopback_test: cannot continue unconnected\n");
        return 1;
    }

    backend.setSliceFrequency(0, 14'200'000.0);
    spin(500);

    // Drive must be non-zero: the simulator scales its feedback by TX drive, so
    // at drive 0 a perfect transmission is indistinguishable from none.
    backend.setTxDriveLevel(200);

    // Baseline: what the spectrum looks like unkeyed.
    spin(1200);
    const std::vector<float> baseline = lastSpectrum;
    check(!baseline.empty(), "receiving spectrum before keying");

    // ---- scene anchor: pin the RECEIVE end before trusting the transmit end ----
    //
    // Unkeyed, so nothing here has been through the modulator. If these fail, the
    // receive/display handedness is wrong and EVERY transmit assertion below is
    // measuring through a broken ruler — fix this first and do not read the
    // transmit results as a sideband verdict.
    if (!baseline.empty()) {
        const int n = static_cast<int>(baseline.size());
        const float lowUp = peakNear(baseline, binForOffset(kSceneToneLowHz, n), 2);
        const float lowDown = peakNear(baseline, binForOffset(-kSceneToneLowHz, n), 2);
        const float highUp = peakNear(baseline, binForOffset(kSceneToneHighHz, n), 2);
        const float highDown = peakNear(baseline, binForOffset(-kSceneToneHighHz, n), 2);

        std::fprintf(stderr,
            "scene anchor: %.0f Hz above %.1f dB / below %.1f dB, "
            "%.0f Hz above %.1f dB / below %.1f dB\n",
            kSceneToneLowHz, lowUp, lowDown, kSceneToneHighHz, highUp, highDown);

        check(lowUp - lowDown > 20.0f,
              "receive handedness: the simulator's 800 Hz scene tone is ABOVE centre");
        check(highUp - highDown > 20.0f,
              "receive handedness: the simulator's 4 kHz scene tone is ABOVE centre");
    }

    // ---- transmit a tone ----
    constexpr double kToneOffsetHz = 5000.0;
    backend.setTxTestTone(kToneOffsetHz, 0.5);
    backend.setKeying(true);
    spin(2500);
    const std::vector<float> keyed = lastSpectrum;
    backend.setKeying(false);
    backend.setTxTestTone(0.0, 0.0);

    check(keyed.size() == baseline.size() && !keyed.empty(),
          "spectrum still flowing while keyed");

    if (!keyed.empty() && keyed.size() == baseline.size()) {
        // The spectrum is fftshifted, so DC sits at the centre bin. A +5 kHz
        // tone lands that many bins above it.
        const int n = static_cast<int>(keyed.size());
        const int centre = n / 2;
        const double binHz = static_cast<double>(kIqRateHz) / n;
        // POSITIVE offset, and the scene anchor above is what earns the right to
        // say so. Hl2TxDsp conjugates for the wire and Hl2RxDsp conjugates back
        // for the panadapter, so the spectrum reads in the analytic convention
        // and a tone sent 5 kHz up comes back 5 kHz up. The old negative
        // expectation was correct against the pre-#4471 display, which showed
        // the raw wire; it has been stale since.
        const int expected = binForOffset(kToneOffsetHz, n);

        // Find the strongest bin while keyed.
        int peak = 0;
        for (int i = 1; i < n; ++i)
            if (keyed[static_cast<std::size_t>(i)] > keyed[static_cast<std::size_t>(peak)])
                peak = i;

        const double peakHz = (peak - centre) * binHz;
        std::fprintf(stderr,
            "loopback: peak bin %d (%.0f Hz), expected ~%d (%.0f Hz), level %.1f dB "
            "(baseline at that bin %.1f dB)\n",
            peak, peakHz, expected, kToneOffsetHz,
            keyed[static_cast<std::size_t>(peak)],
            baseline[static_cast<std::size_t>(expected)]);

        // Within a few bins of where we transmitted it.
        check(std::abs(peak - expected) <= 4,
              "the transmitted tone comes back at the frequency it was sent on");
        // And it must actually stand above where the floor was before keying.
        check(keyed[static_cast<std::size_t>(expected)]
                  - baseline[static_cast<std::size_t>(expected)] > 20.0f,
              "the tone is >20 dB above the unkeyed floor at that bin");
    }

    // ---- the REAL audio path: submitTxAudio -> modulator -> EP2 ----
    //
    // The tone above is generated inside MetisClient, so it proves the wire but
    // not the chain an operator actually uses. This drives the same entry point
    // AudioEngine feeds: processed int16 stereo at 24 kHz, which must arrive on
    // the air as a single sideband at the audio frequency.
    {
        backend.setSliceMode(0, QStringLiteral("USB"));
        spin(300);
        backend.setKeying(true);

        std::vector<float> voice;
        constexpr double kAudioHz = 1500.0;
        constexpr int kRate = 24000;
        // ~2 s of audio in 20 ms blocks, the cadence AudioEngine delivers.
        for (int blk = 0; blk < 100; ++blk) {
            const int frames = kRate / 50;
            QByteArray pcm(frames * 2 * static_cast<int>(sizeof(qint16)), 0);
            auto* out = reinterpret_cast<qint16*>(pcm.data());
            for (int n = 0; n < frames; ++n) {
                const double t = static_cast<double>(blk * frames + n) / kRate;
                const auto v = static_cast<qint16>(
                    0.5 * 32767.0 * std::sin(2.0 * M_PI * kAudioHz * t));
                out[2 * n] = v;
                out[2 * n + 1] = v;      // AudioEngine duplicates across channels
            }
            backend.submitTxAudio(pcm, kRate, /*clientLeveled=*/false);
            spin(20);
            // Capture WHILE transmitting. Sampling after the loop would read
            // silence: the queue drains in well under a second once audio stops,
            // and unlike the built-in tone (synthesised per packet, so it never
            // stops) this path only carries what has been submitted.
            if (blk == 80)
                voice = lastSpectrum;
        }
        backend.setKeying(false);

        // Assert the capture happened. Without this the three checks below are
        // skipped in silence when `voice` never arrived, and the test still
        // prints "all checks passed" — the same silent-green shape as a skip
        // that exits 0, one level down.
        check(voice.size() == baseline.size() && !voice.empty(),
              "spectrum still flowing during the mic-path transmission");

        if (voice.size() == baseline.size() && !voice.empty()) {
            const int n = static_cast<int>(voice.size());
            const int centre = n / 2;
            const double binHz = static_cast<double>(kIqRateHz) / n;
            // USB: 1.5 kHz of audio is transmitted 1.5 kHz ABOVE the carrier and,
            // through the twice-conjugating loop, reads 1.5 kHz above centre.
            //
            // Asserting the textbook sign is what let a wrong-sideband
            // transmitter pass its own tests once — but the defence against that
            // is the scene anchor, which fixes the receive end independently, not
            // a sign flip here. Flipping the sign only made this fail always,
            // which is not the same as having teeth.
            const int expected = binForOffset(kAudioHz, n);
            const int mirrored = binForOffset(-kAudioHz, n);

            int peak = 0;
            for (int i = 1; i < n; ++i)
                if (voice[static_cast<std::size_t>(i)] > voice[static_cast<std::size_t>(peak)])
                    peak = i;

            std::fprintf(stderr,
                "mic path: peak bin %d (%.0f Hz), expected %d; wanted %.1f dB, "
                "mirror %.1f dB, floor %.1f dB\n",
                peak, (peak - centre) * binHz, expected,
                voice[static_cast<std::size_t>(expected)],
                voice[static_cast<std::size_t>(mirrored)],
                baseline[static_cast<std::size_t>(expected)]);

            check(std::abs(peak - expected) <= 4,
                  "submitTxAudio: 1.5 kHz audio transmits at +1.5 kHz (USB)");
            check(voice[static_cast<std::size_t>(expected)]
                      - baseline[static_cast<std::size_t>(expected)] > 20.0f,
                  "submitTxAudio: the tone is well above the unkeyed floor");
            // The whole point of SSB: nothing on the other side of the carrier.
            check(voice[static_cast<std::size_t>(expected)]
                      - voice[static_cast<std::size_t>(mirrored)] > 20.0f,
                  "submitTxAudio: opposite sideband suppressed on the air");
        }
    }

    backend.disconnectRadio();
    spin(500);

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_tx_loopback_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
