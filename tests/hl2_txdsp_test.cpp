// SSB modulator correctness for the Hermes-Lite 2 transmit chain.
//
// The assertion that matters on the air: for USB, a 1 kHz audio tone must leave
// this modulator at -1 kHz of the IQ it hands to the wire, and for LSB at
// +1 kHz.
//
// THAT SIGN LOOKS BACKWARDS AND IS NOT. The HPSDR wire order has the opposite
// handedness to the standard analytic convention, which is why the receive path
// conjugates with -imag() before WDSP sees anything. Transmit carries the same
// correction, so the wire-facing sign is inverted from the textbook one.
//
// This was originally asserted the textbook way, and the test passed while every
// transmission went out on the WRONG SIDEBAND — invisible from inside the
// application, because the panadapter reads the same wire order and therefore
// agreed with it. It took an operator with a second receiver to catch it.
//
// It also pins the rate conversion (24 kHz audio in, 48 kHz IQ out) and the
// mic-gain path, both of which are easy to get subtly wrong in ways that only
// show up as "my audio is quiet" or "my signal is 2 kHz wide".

#include "core/backends/hl2/Hl2TxDsp.h"

#include <QCoreApplication>
#include <QObject>

#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

// Goertzel-style complex bin: correlate the IQ against exp(-j*2*pi*f*n/fs).
// Positive f probes the upper sideband, negative the lower.
static double binPower(const std::vector<std::complex<float>>& iq, double hz, double fs)
{
    std::complex<double> acc{0.0, 0.0};
    const double w = -2.0 * M_PI * hz / fs;
    for (std::size_t n = 0; n < iq.size(); ++n) {
        const double ph = w * static_cast<double>(n);
        acc += std::complex<double>(iq[n].real(), iq[n].imag())
             * std::complex<double>(std::cos(ph), std::sin(ph));
    }
    return std::abs(acc) / static_cast<double>(iq.size());
}

// Run `seconds` of a pure audio tone through the modulator and collect the IQ.
static std::vector<std::complex<float>> modulate(WdspChannel::Mode mode,
                                                 double toneHz, double amplitude,
                                                 double micGain, double seconds,
                                                 float* lastMicPeak = nullptr,
                                                 bool alc = false)
{
    Hl2TxDsp tx;
    Hl2TxDsp::Config cfg;
    cfg.mode = mode;
    // ALC OFF by default in these tests. Every assertion below except the ALC's
    // own measures a fixed relationship between input and output, and an ALC
    // exists precisely to break fixed relationships.
    cfg.alcEnabled = alc;
    std::string err;
    if (!tx.configure(cfg, &err)) {
        std::fprintf(stderr, "FAIL: configure: %s\n", err.c_str());
        ++g_failures;
        return {};
    }
    tx.setMicGain(micGain);

    std::vector<std::complex<float>> out;
    QObject::connect(&tx, &Hl2TxDsp::iqReady, &tx,
                     [&](const std::vector<std::complex<float>>& iq) {
        out.insert(out.end(), iq.begin(), iq.end());
    });
    if (lastMicPeak) {
        QObject::connect(&tx, &Hl2TxDsp::micPeak, &tx,
                         [lastMicPeak](float db) { *lastMicPeak = db; });
    }

    const int fs = cfg.inputSampleRateHz;
    const int total = static_cast<int>(seconds * fs);
    std::vector<float> audio(static_cast<std::size_t>(total));
    for (int n = 0; n < total; ++n)
        audio[static_cast<std::size_t>(n)] =
            static_cast<float>(amplitude * std::sin(2.0 * M_PI * toneHz * n / fs));

    // Feed in realistic chunks rather than one giant block.
    constexpr std::size_t kChunk = 240;
    for (std::size_t off = 0; off < audio.size(); off += kChunk) {
        const std::size_t n = std::min(kChunk, audio.size() - off);
        tx.processAudioBlock(std::vector<float>(audio.begin() + static_cast<std::ptrdiff_t>(off),
                                                audio.begin() + static_cast<std::ptrdiff_t>(off + n)));
    }
    return out;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    constexpr double kFsOut = 48000.0;
    constexpr double kTone = 1000.0;

    // ---- rate conversion ----
    {
        const auto iq = modulate(WdspChannel::Mode::Usb, kTone, 0.5, 1.0, 0.5);
        check(!iq.empty(), "USB modulation produces IQ");
        double mx = 0.0;
        for (const auto& v : iq) mx = std::max(mx, static_cast<double>(std::abs(v)));
        std::fprintf(stderr, "diag: %zu IQ samples, max |IQ| = %.9f\n", iq.size(), mx);
        // 0.5 s of 24 kHz audio -> ~0.5 s of 48 kHz IQ. WDSP's filter latency
        // eats a little, so allow a generous margin; what matters is the 2:1.
        check(iq.size() > 18000 && iq.size() < 26000,
              "24 kHz audio in -> ~48 kHz IQ out (2:1 rate conversion)");
    }

    // ---- USB puts the tone ABOVE the carrier ----
    {
        const auto iq = modulate(WdspChannel::Mode::Usb, kTone, 0.5, 1.0, 1.0);
        if (!iq.empty()) {
            const double upper = binPower(iq, +kTone, kFsOut);
            const double lower = binPower(iq, -kTone, kFsOut);
            const double ratioDb = 20.0 * std::log10((upper + 1e-12) / (lower + 1e-12));
            std::fprintf(stderr, "USB: +1kHz %.6f, -1kHz %.6f, suppression %.1f dB\n",
                         upper, lower, ratioDb);
            check(lower > upper,
                  "USB: wire-facing IQ puts energy on the LOWER side (conjugated)");
            check(-ratioDb > 30.0, "USB: opposite sideband suppressed by >30 dB");
        }
    }

    // ---- LSB puts it BELOW ----
    {
        const auto iq = modulate(WdspChannel::Mode::Lsb, kTone, 0.5, 1.0, 1.0);
        if (!iq.empty()) {
            const double upper = binPower(iq, +kTone, kFsOut);
            const double lower = binPower(iq, -kTone, kFsOut);
            const double ratioDb = 20.0 * std::log10((lower + 1e-12) / (upper + 1e-12));
            std::fprintf(stderr, "LSB: +1kHz %.6f, -1kHz %.6f, suppression %.1f dB\n",
                         upper, lower, ratioDb);
            check(upper > lower,
                  "LSB: wire-facing IQ puts energy on the UPPER side (conjugated)");
            check(-ratioDb > 30.0, "LSB: opposite sideband suppressed by >30 dB");
        }
    }

    // ---- audio outside the passband does not get transmitted ----
    {
        // 5 kHz is well above the 2700 Hz TX filter.
        const auto iq = modulate(WdspChannel::Mode::Usb, 5000.0, 0.5, 1.0, 1.0);
        const auto ref = modulate(WdspChannel::Mode::Usb, kTone, 0.5, 1.0, 1.0);
        if (!iq.empty() && !ref.empty()) {
            const double out = binPower(iq, 5000.0, kFsOut);
            const double in  = binPower(ref, kTone, kFsOut);
            const double rejDb = 20.0 * std::log10((in + 1e-12) / (out + 1e-12));
            double mxOut = 0.0, mxRef = 0.0;
            for (const auto& v : iq)  mxOut = std::max(mxOut, static_cast<double>(std::abs(v)));
            for (const auto& v : ref) mxRef = std::max(mxRef, static_cast<double>(std::abs(v)));
            // Probe where the 5 kHz energy actually went.
            std::fprintf(stderr, "passband: 1 kHz %.6f vs 5 kHz %.6f, rejection %.1f dB "
                                 "| max|IQ| ref %.4f out %.4f | out@-5k %.6f out@19k %.6f out@1k %.6f\n",
                         in, out, rejDb, mxRef, mxOut,
                         binPower(iq, -5000.0, kFsOut), binPower(iq, 19000.0, kFsOut),
                         binPower(iq, 1000.0, kFsOut));
            check(rejDb > 30.0, "audio above the TX passband is filtered out");
        }
    }

    // ---- mic gain scales the drive, and the peak meter follows it ----
    // Runs with the ALC OFF: with it on, compensating for exactly this change is
    // the ALC's purpose, and the 1:1 relationship correctly disappears.
    {
        float peakUnity = -300.0f, peakHalf = -300.0f;
        const auto loud  = modulate(WdspChannel::Mode::Usb, kTone, 0.5, 1.0, 0.5, &peakUnity);
        const auto quiet = modulate(WdspChannel::Mode::Usb, kTone, 0.5, 0.5, 0.5, &peakHalf);
        if (!loud.empty() && !quiet.empty()) {
            const double a = binPower(loud, kTone, kFsOut);
            const double b = binPower(quiet, kTone, kFsOut);
            const double dropDb = 20.0 * std::log10((a + 1e-12) / (b + 1e-12));
            std::fprintf(stderr, "mic gain: unity %.6f, half %.6f, drop %.1f dB; "
                                 "peak %.1f -> %.1f dBFS\n",
                         a, b, dropDb, peakUnity, peakHalf);
            check(dropDb > 4.0 && dropDb < 8.0,
                  "halving mic gain drops the transmitted level by ~6 dB");
            check(peakUnity - peakHalf > 4.0 && peakUnity - peakHalf < 8.0,
                  "the mic peak meter follows mic gain by the same ~6 dB");
        }
    }

    // ---- ALC lifts speech-level audio to something that modulates ----
    //
    // This is the gap that made voice inaudible on the air: measured on the
    // radio, audio at -10 dBFS produced 1226 counts of forward power and audio
    // at -30 dBFS produced 47, while ordinary speech sits near -32 dBFS. The
    // ALC's job is to close that.
    {
        constexpr double kQuiet = 0.02;         // about -34 dBFS, speech-ish
        const auto plain = modulate(WdspChannel::Mode::Usb, kTone, kQuiet, 1.0, 1.5,
                                    nullptr, false);
        const auto alced = modulate(WdspChannel::Mode::Usb, kTone, kQuiet, 1.0, 1.5,
                                    nullptr, true);
        if (!plain.empty() && !alced.empty()) {
            // Compare the settled tail: the ALC ramps in, so the opening block
            // is deliberately not representative.
            auto tailPeak = [](const std::vector<std::complex<float>>& v) {
                double mx = 0.0;
                for (std::size_t i = v.size() / 2; i < v.size(); ++i)
                    mx = std::max(mx, static_cast<double>(std::abs(v[i])));
                return mx;
            };
            const double a = tailPeak(plain);
            const double b = tailPeak(alced);
            std::fprintf(stderr, "ALC: quiet input peak %.4f -> %.4f (+%.1f dB)\n",
                         a, b, 20.0 * std::log10((b + 1e-12) / (a + 1e-12)));
            check(b > a * 5.0, "ALC lifts quiet audio substantially");
            check(b > 0.5, "ALC brings quiet audio near full modulation");
            // And it must never overshoot into clipping — an ALC that overshoots
            // transmits splatter rather than merely clipping our own audio.
            double mx = 0.0;
            for (const auto& v : alced) mx = std::max(mx, static_cast<double>(std::abs(v)));
            check(mx <= 1.001, "ALC output never exceeds full scale");
        }
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_txdsp_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
