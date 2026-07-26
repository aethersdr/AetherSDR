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
// SIM ONLY, deliberately. The address is hardcoded to the simulator and this
// test keys the transmitter; pointing it at real hardware would radiate.
// If nothing answers there, the test SKIPS rather than fails, so a machine
// without the simulator running does not get a spurious red.

#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QUdpSocket>

#include <cmath>
#include <cstdio>
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

// Is the simulator actually there? A plain Metis discovery probe.
static bool simPresent(const QString& host)
{
    QUdpSocket s;
    if (!s.bind(QHostAddress(QHostAddress::AnyIPv4), 0))
        return false;
    const auto req = AetherSDR::hl2::discoveryRequest();
    s.writeDatagram(reinterpret_cast<const char*>(req.data()),
                    static_cast<qint64>(req.size()), QHostAddress(host),
                    AetherSDR::hl2::kMetisPort);
    return s.waitForReadyRead(1500);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<SliceDelta>();

    const QString simHost = QStringLiteral("192.168.1.12");
    if (!simPresent(simHost)) {
        std::fprintf(stderr,
            "hl2_tx_loopback_test: SKIPPED — no simulator answering at %s\n",
            qPrintable(simHost));
        return 0;
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
    backend.connectRadio(req);
    spin(2500);
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
        const double binHz = 48000.0 / n;
        // NEGATIVE offset: hpsdrsim feeds the TX IQ back in WIRE order, and the
        // wire is the conjugate of the standard analytic convention (see
        // Hl2TxDsp). On the air this same IQ becomes +5 kHz; here we are looking
        // at the wire, so it reads -5 kHz.
        const int expected = centre - static_cast<int>(std::lround(kToneOffsetHz / binHz));

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
            backend.submitTxAudio(pcm, kRate);
            spin(20);
            // Capture WHILE transmitting. Sampling after the loop would read
            // silence: the queue drains in well under a second once audio stops,
            // and unlike the built-in tone (synthesised per packet, so it never
            // stops) this path only carries what has been submitted.
            if (blk == 80)
                voice = lastSpectrum;
        }
        backend.setKeying(false);

        if (voice.size() == baseline.size() && !voice.empty()) {
            const int n = static_cast<int>(voice.size());
            const int centre = n / 2;
            const double binHz = 48000.0 / n;
            // Same wire-order reasoning as above: USB audio leaves the
            // modulator BELOW centre in wire order and is transmitted above the
            // carrier. Asserting the textbook sign here is precisely what let a
            // wrong-sideband transmitter pass its own tests — it took an
            // operator with a second receiver to catch it.
            const int expected = centre - static_cast<int>(std::lround(kAudioHz / binHz));
            const int mirrored = centre + static_cast<int>(std::lround(kAudioHz / binHz));

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
