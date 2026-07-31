// LIVE hardware probe — NOT part of ctest. Requires a real Hermes-Lite 2.
//
//   ./hl2_live_band_filter_probe <ip>
//
// Why this exists: the J16 filter byte rides the CONFIG register (0x00), the
// same register that carries the DDC sample rate and the receiver count. That
// makes it the one change in this work that can break receive outright — a
// misplaced bit lands in [25:24] (sample rate) or [6:3] (receiver count), and
// the failure mode is a radio that still streams, still looks framed, and
// delivers samples at the wrong rate or from an unassigned receiver.
//
// The unit test proves the ENCODER puts the bits where the register map says.
// It cannot prove the radio agrees, because it shares our reading of the map.
// This asks the hardware: with each band filter selected in turn, does EP6
// still arrive, at the expected packet rate, carrying non-trivial IQ?
//
// Sample rate is the independent check. It is not derived from anything we
// send back to ourselves: EP6 packets carry 126 samples each, so 48 kHz means
// 381 packets/second and 96 kHz means 762. If a filter bit leaked into
// DATA[25:24] the measured packet rate would move, and no amount of agreeing
// with ourselves about the register map would hide it.

#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QTimer>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace AetherSDR::hl2;

namespace {

int g_failures = 0;

void check(bool ok, const QString& what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    if (!ok)
        ++g_failures;
}

struct Observation {
    int    packets = 0;
    double rms = 0.0;
    Hl2Telemetry telemetry;
};

// Watch the stream for windowMs and report what arrived.
Observation observe(MetisClient& c, int windowMs)
{
    Observation o;
    double sumSq = 0.0;
    std::size_t n = 0;
    auto blockConn = QObject::connect(&c, &MetisClient::iqBlockReady,
        [&](const std::vector<std::complex<float>>& block) {
            ++o.packets;
            for (const auto& s : block) {
                sumSq += static_cast<double>(std::norm(s));
                ++n;
            }
        });
    auto telConn = QObject::connect(&c, &MetisClient::telemetryUpdated,
        [&](const Hl2Telemetry& t) { o.telemetry = t; });

    QEventLoop loop;
    QTimer::singleShot(windowMs, &loop, &QEventLoop::quit);
    loop.exec();

    QObject::disconnect(blockConn);
    QObject::disconnect(telConn);
    o.rms = n ? std::sqrt(sumSq / static_cast<double>(n)) : 0.0;
    return o;
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString host = argc > 1 ? QString::fromLatin1(argv[1])
                                  : QStringLiteral("192.168.1.21");

    MetisClient client;
    MetisClient::Params p;
    p.host = QHostAddress(host);
    p.sampleRate = SampleRate::R48k;
    p.rxFrequencyHz = 10'000'000;      // WWV — a signal that is reliably there
    p.lnaGainDb = 20;
    p.ocFilterByte = ocFilterByteForHz(10'000'000);

    std::printf("connecting to %s ...\n", qPrintable(host));
    if (!client.start(p)) {
        std::fprintf(stderr, "FATAL: could not open the UDP socket\n");
        return 2;
    }

    // Let the link come up before measuring anything.
    {
        QEventLoop loop;
        QTimer::singleShot(1500, &loop, &QEventLoop::quit);
        loop.exec();
    }
    if (!client.isRunning()) {
        std::fprintf(stderr, "FATAL: client stopped — radio unreachable or in use\n");
        return 2;
    }

    // Sweep every filter selection the band map can produce, at the frequency
    // that selects it. Each one rewrites the config register.
    struct Step { double mhz; const char* label; };
    const Step steps[] = {
        {  1.900, "160m" }, {  3.800, "80m"  }, {  7.200, "40m"  },
        { 10.000, "30m"  }, { 14.225, "20m"  }, { 18.130, "17m"  },
        { 24.950, "12m"  }, { 28.400, "10m"  }, {  0.600, "AM BC (no filter)" },
    };

    // 126 samples per EP6 packet at 48 kHz => 381/s. Allow generous slack: this
    // is a check that the rate did not CHANGE, not a jitter measurement.
    constexpr double kExpectedPacketsPerSec = 48000.0 / 126.0;
    constexpr int kWindowMs = 1200;

    for (const Step& s : steps) {
        const auto oc = ocFilterByteForHz(s.mhz * 1.0e6);
        client.setRxFrequencyHz(static_cast<std::uint32_t>(s.mhz * 1.0e6));
        client.setBandFilter(static_cast<int>(oc));

        const Observation o = observe(client, kWindowMs);
        const double rate = o.packets * 1000.0 / kWindowMs;
        const double ratio = rate / kExpectedPacketsPerSec;

        std::printf("  %-18s %.3f MHz  oc=0x%02X (%-20s) "
                    "%4d pkt  %6.1f pkt/s  rms=%.2e\n",
                    s.label, s.mhz, oc, ocFilterName(oc), o.packets, rate, o.rms);

        check(o.packets > 0,
              QStringLiteral("%1: stream still running after filter write").arg(s.label));
        // The load-bearing assertion. A filter bit that leaked into the sample
        // rate field would halve or double this.
        check(ratio > 0.85 && ratio < 1.15,
              QStringLiteral("%1: EP6 packet rate unchanged (%2x expected)")
                  .arg(s.label).arg(ratio, 0, 'f', 3));
        // A receiver-count bit landing in [6:3] leaves the DDC unassigned and
        // the payload becomes exactly zero — framed, paced, and silent.
        check(o.rms > 0.0,
              QStringLiteral("%1: IQ is not all-zero (receiver still assigned)")
                  .arg(s.label));
    }

    // ---- Does the filter board actually exist? ----
    //
    // Everything above proves we did not BREAK the config register. It cannot
    // prove a relay moved, because the gateware forwards this byte to I2C and
    // nothing answers — there is no readback anywhere in the protocol.
    //
    // So ask physics instead. Park on the AM broadcast band, where there is a
    // large signal by definition, and A/B the AM-blocking high-pass. With an
    // N2ADR board fitted, engaging it has to crush that signal by tens of dB.
    // With no board, the write is inert and the two readings are identical.
    //
    // This is the only measurement here that is outside our own convention: it
    // does not ask whether the radio agreed with our register map, it asks
    // whether the antenna path changed.
    {
        client.setRxFrequencyHz(600'000);
        client.setBandFilter(static_cast<int>(kOcNone));
        const double bypassed = observe(client, 1500).rms;
        client.setBandFilter(static_cast<int>(kOcHpfAmBc));
        const double filtered = observe(client, 1500).rms;
        client.setBandFilter(static_cast<int>(kOcNone));

        const double ratioDb = (bypassed > 0.0 && filtered > 0.0)
            ? 20.0 * std::log10(filtered / bypassed) : 0.0;
        std::printf("\n  AM BC 0.600 MHz: bypassed rms=%.3e  HPF-in rms=%.3e  "
                    "delta=%.1f dB\n", bypassed, filtered, ratioDb);
        if (ratioDb < -6.0) {
            std::printf("  -> companion filter board IS present and switching "
                        "(HPF attenuated the AM band)\n");
        } else {
            std::printf("  -> NO measurable change. Either no companion filter "
                        "board is fitted (the writes are inert, which is safe "
                        "and expected on a bare HL2) or the band is quiet. This "
                        "is NOT a failure — it is the limit of what can be "
                        "observed without one.\n");
        }
    }

    // ---- Does a LIVE LNA gain change reach the hardware? ----
    //
    // Same question as the filter, same kind of answer. lnaGainDb used to be
    // applied once at connect and never again, so the RF Gain slider had
    // nothing behind it. The fix routes it through MetisClient::setLnaGainDb at
    // any time — and the way to know that took is that the noise floor moves by
    // the amount we asked for.
    //
    // 20 dB apart, measured on the raw wire before any of our DSP or dB
    // referencing runs, so nothing in our own gain accounting can flatter the
    // result. Tolerance is wide: the AD9866's step is not exactly 1.000 dB and
    // an off-air noise floor is not stationary. What is being tested is that
    // the register moved AT ALL and in the right DIRECTION, not its linearity.
    {
        client.setRxFrequencyHz(10'000'000);
        client.setBandFilter(static_cast<int>(ocFilterByteForHz(10'000'000)));
        client.setLnaGainDb(10);
        const double lowGain = observe(client, 1500).rms;
        client.setLnaGainDb(30);
        const double highGain = observe(client, 1500).rms;
        client.setLnaGainDb(20);       // back to the default we came up on

        const double deltaDb = (lowGain > 0.0 && highGain > 0.0)
            ? 20.0 * std::log10(highGain / lowGain) : 0.0;
        std::printf("\n  LNA gain 10 dB -> 30 dB: rms %.3e -> %.3e  "
                    "measured delta=%.1f dB (commanded 20 dB)\n",
                    lowGain, highGain, deltaDb);
        check(deltaDb > 10.0,
              QStringLiteral("live LNA gain change reaches the AD9866 "
                             "(measured %1 dB for a commanded 20 dB)")
                  .arg(deltaDb, 0, 'f', 1));
    }

    // Telemetry the Radio Health dialog renders, read back from the real radio.
    {
        const Observation o = observe(client, 1000);
        const Hl2Telemetry& t = o.telemetry;
        std::printf("\n  telemetry: fw=%s adcOvl=%s txInh=%s fifo=%s tempRaw=%s "
                    "fwd=%s rev=%s bias=%s\n",
                    t.firmwareVersion ? qPrintable(QString::number(*t.firmwareVersion)) : "-",
                    t.adcOverload ? (*t.adcOverload ? "yes" : "no") : "-",
                    t.txInhibited ? (*t.txInhibited ? "yes" : "no") : "-",
                    t.txFifoCount ? qPrintable(QString::number(*t.txFifoCount)) : "-",
                    t.temperatureRaw ? qPrintable(QString::number(*t.temperatureRaw)) : "-",
                    t.forwardPowerRaw ? qPrintable(QString::number(*t.forwardPowerRaw)) : "-",
                    t.reversePowerRaw ? qPrintable(QString::number(*t.reversePowerRaw)) : "-",
                    t.biasCurrentRaw ? qPrintable(QString::number(*t.biasCurrentRaw)) : "-");
        check(t.firmwareVersion.has_value(),
              QStringLiteral("firmware version reported (Radio Health)"));
        check(t.temperatureRaw.has_value(),
              QStringLiteral("temperature reported (Radio Health)"));
        // Sanity, not calibration: the AD9866 die sits somewhere between a cold
        // room and its thermal limit. A raw count that scales outside this is a
        // decode error, not a hot radio.
        if (t.temperatureRaw) {
            const double c = (3.26 * (*t.temperatureRaw / 4096.0) - 0.5) / 0.01;
            std::printf("  decoded temperature: %.1f C\n", c);
            check(c > 0.0 && c < 100.0,
                  QStringLiteral("decoded temperature is physically plausible"));
        }
    }

    client.stop();
    std::printf("\n%s (%d failures)\n", g_failures ? "PROBE FAILED" : "PROBE PASSED",
                g_failures);
    return g_failures ? 1 : 0;
}
