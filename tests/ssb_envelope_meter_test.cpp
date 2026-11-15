// Pins the offline SSB envelope meter the transmit-dynamics work is measured
// with (RFC #4836, Stage 0). Socket-free and keys nothing: every input is a
// synthetic signal whose envelope is known in closed form.

#include "core/dsp/SsbEnvelopeMeter.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

using AetherSDR::SsbEnvelopeReport;
using AetherSDR::SsbFilterPhase;
using AetherSDR::SsbPassband;
using AetherSDR::designSsbFilter;
using AetherSDR::measureSsbEnvelope;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 24000.0;  // the Flex transport rate
int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-64s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) ++g_failed;
}

std::string fmt(const char* format, double a, double b = 0.0)
{
    char buf[128];
    std::snprintf(buf, sizeof buf, format, a, b);
    return buf;
}

std::vector<float> tones(const std::vector<std::pair<double, double>>& hzAndAmplitude,
                         double seconds, bool useSine = false)
{
    std::vector<float> out(static_cast<std::size_t>(seconds * kRate));
    for (std::size_t n = 0; n < out.size(); ++n) {
        double v = 0.0;
        for (const auto& [hz, amp] : hzAndAmplitude) {
            const double phase = 2.0 * kPi * hz * static_cast<double>(n) / kRate;
            v += amp * (useSine ? std::sin(phase) : std::cos(phase));
        }
        out[n] = static_cast<float>(v);
    }
    return out;
}

SsbEnvelopeReport measure(const std::vector<float>& x,
                          SsbPassband band = {},
                          SsbFilterPhase phase = SsbFilterPhase::Linear)
{
    return measureSsbEnvelope(x.data(), x.size(), kRate, band, phase);
}

double magnitudeAt(const std::vector<std::complex<double>>& h, double hz)
{
    std::complex<double> acc(0.0, 0.0);
    for (std::size_t n = 0; n < h.size(); ++n)
        acc += h[n] * std::polar(1.0, -2.0 * kPi * hz * static_cast<double>(n) / kRate);
    return std::abs(acc);
}

}  // namespace

int main()
{
    // A single in-band tone has a constant envelope equal to its amplitude.
    {
        const auto r = measure(tones({{1000.0, 0.5}}, 1.0));
        report("single tone: envelope equals amplitude",
               std::fabs(r.envelopePeak - 0.5) < 0.005, fmt("peak %.4f", r.envelopePeak));
        report("single tone: peak-to-average ratio is 0 dB",
               std::fabs(r.paprDb) < 0.05, fmt("%.3f dB", r.paprDb));
    }

    // Two equal tones: the envelope swings 0..2A with RMS sqrt(2)A, 3.01 dB.
    // 700 and 1900 Hz beat at 1200 Hz, 20 samples at 24 kHz, so samples land
    // on the envelope peaks exactly.
    const auto twoTone = tones({{700.0, 0.25}, {1900.0, 0.25}}, 1.0);
    const auto usb = measure(twoTone);
    {
        report("two tone: envelope peak is 2A",
               std::fabs(usb.envelopePeak - 0.5) < 0.005, fmt("peak %.4f", usb.envelopePeak));
        report("two tone: peak-to-average ratio is 3.01 dB",
               std::fabs(usb.paprDb - 3.0103) < 0.05, fmt("%.3f dB", usb.paprDb));
        report("two tone: no envelope overshoot over the audio peak",
               std::fabs(usb.envelopeOverAudioPeakDb) < 0.05,
               fmt("%.3f dB", usb.envelopeOverAudioPeakDb));
    }

    // Either sideband gives the same envelope: |x + jH(x)| == |x - jH(x)|.
    {
        const auto lsb = measure(twoTone, {-2700.0, -300.0});
        report("lower sideband envelope matches the upper sideband",
               std::fabs(lsb.paprDb - usb.paprDb) < 0.01
                   && std::fabs(lsb.envelopePeak - usb.envelopePeak) < 0.001,
               fmt("LSB %.4f dB, USB %.4f dB", lsb.paprDb, usb.paprDb));
    }

    // The reason a client sample-peak limiter cannot cap PEP: a square wave
    // band-limited to its 500/1500/2500 Hz harmonics peaks near 1.18 as audio,
    // but its envelope peaks at (4/pi)(1 + 1/3 + 1/5) = 1.952.
    {
        const auto square = tones({{500.0, 4.0 / kPi},
                                   {1500.0, 4.0 / (3.0 * kPi)},
                                   {2500.0, 4.0 / (5.0 * kPi)}}, 1.0, true);
        const auto r = measure(square);
        const double expected = 4.0 / kPi * (1.0 + 1.0 / 3.0 + 1.0 / 5.0);
        report("square wave: envelope peak is the harmonic sum",
               std::fabs(r.envelopePeak - expected) < 0.02 * expected,
               fmt("%.4f vs %.4f", r.envelopePeak, expected));
        report("square wave: envelope overshoots the audio peak by > 3 dB",
               r.envelopeOverAudioPeakDb > 3.0, fmt("%.2f dB", r.envelopeOverAudioPeakDb));
    }

    // Out-of-band energy: a 6 kHz tone 20 dB under a 1 kHz tone, with a
    // 300..2700 Hz passband, reads -20 dBc.
    {
        const auto r = measure(tones({{1000.0, 0.5}, {6000.0, 0.05}}, 1.0));
        report("out-of-band tone 20 dB down reads -20 dBc",
               std::fabs(r.outOfBandDbc + 20.0) < 0.3, fmt("%.2f dBc", r.outOfBandDbc));
        const auto clean = measure(tones({{1000.0, 0.5}}, 1.0));
        report("in-band tone alone has no out-of-band energy",
               clean.outOfBandDbc < -60.0, fmt("%.1f dBc", clean.outOfBandDbc));
    }

    // The minimum-phase model keeps the magnitude response and moves the
    // energy to the front of the filter.
    {
        const auto lin = designSsbFilter(kRate, {}, 1023, SsbFilterPhase::Linear);
        const auto min = designSsbFilter(kRate, {}, 1023, SsbFilterPhase::Minimum);
        bool sameMagnitude = true;
        double worst = 0.0;
        for (double hz : {500.0, 1000.0, 1500.0, 2000.0, 2500.0}) {
            const double d = std::fabs(20.0 * std::log10(magnitudeAt(min, hz) / magnitudeAt(lin, hz)));
            worst = std::max(worst, d);
            sameMagnitude = sameMagnitude && d < 0.05;
        }
        report("minimum phase keeps the passband magnitude", sameMagnitude,
               fmt("worst %.4f dB", worst));

        auto frontEnergy = [](const std::vector<std::complex<double>>& h) {
            double front = 0.0, total = 0.0;
            for (std::size_t n = 0; n < h.size(); ++n) {
                const double e = std::norm(h[n]);
                total += e;
                if (n < h.size() / 4) front += e;
            }
            return front / total;
        };
        report("minimum phase puts > 90% of its energy in the first quarter",
               frontEnergy(min) > 0.9 && frontEnergy(lin) < 0.5,
               fmt("min %.3f, linear %.3f", frontEnergy(min), frontEnergy(lin)));

        const auto r = measure(twoTone, {}, SsbFilterPhase::Minimum);
        report("minimum phase: two-tone ratio is still 3.01 dB",
               std::fabs(r.paprDb - 3.0103) < 0.05, fmt("%.3f dB", r.paprDb));
    }

    // Pauses must not count as low average power. The blocks straddling the
    // burst's end (filter tail inside the 30 dB gate) still count as active,
    // which costs ~0.16 dB here; a whole-signal figure would be off by 3 dB.
    {
        auto burst = tones({{700.0, 0.25}, {1900.0, 0.25}}, 2.0);
        for (std::size_t n = burst.size() / 2; n < burst.size(); ++n) burst[n] = 0.0f;
        const auto r = measure(burst);
        report("active ratio ignores silence",
               std::fabs(r.activePaprDb - 3.0103) < 0.3 && r.paprDb > 5.5,
               fmt("active %.3f dB, whole %.3f dB", r.activePaprDb, r.paprDb));
    }

    // Too short to fill the filter: nothing is analysed, nothing is invented.
    {
        const std::vector<float> tiny(100, 0.5f);
        const auto r = measure(tiny);
        report("input shorter than the filter analyses nothing", r.samplesAnalysed == 0);
    }

    std::printf("%s\n", g_failed ? "FAILED" : "PASSED");
    return g_failed ? 1 : 0;
}
