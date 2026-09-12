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
#include "core/backends/hl2/Hl2TxLevelPolicy.h"

#include <QCoreApplication>
#include <QObject>

#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

using namespace AetherSDR::hl2;
using AetherSDR::TxAudioSource;

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
                                                 bool alc = false,
                                                 const double* passband = nullptr,
                                                 TxAudioSource source =
                                                     TxAudioSource::Microphone)
{
    Hl2TxDsp tx;
    Hl2TxDsp::Config cfg;
    cfg.mode = mode;
    // Optional explicit passband. Hl2Backend pushes a sign-correct, mode-derived
    // one; the struct default is a positive 300..2700 for every mode, so without
    // this the sideband/passband interaction is never exercised at all.
    if (passband) {
        cfg.filterLowHz  = passband[0];
        cfg.filterHighHz = passband[1];
    }
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
                                                audio.begin() + static_cast<std::ptrdiff_t>(off + n)),
                             source);
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

    // ---- DIGU/DIGL transmit on the same sidebands as USB/LSB ----
    //
    // WSJT-X transmits in DIGU. These modes were never covered here, and the
    // sideband is the whole question: an FT8 signal on the wrong sideband is
    // both un-decodable and 3 kHz away from where the operator believes it is.
    //
    // Each is checked with the exact passband Hl2Backend now pushes for that
    // mode, which is the combination that actually ships -- the assertions above
    // only ever ran against the struct default, so the passband's effect on the
    // sideband was untested until this block existed.
    {
        // POSITIVE for every mode. This is the TX convention, and it is the
        // opposite of RX: here the MODE carries the sideband and the bandpass is
        // an audio-domain magnitude. Feeding TX the RX table's signed pairs put
        // LSB and DIGL on the upper sideband -- caught by this very block before
        // it shipped, which is why the two tables are separate in Hl2Backend.
        const double diguBand[2] = {150.0, 3000.0};
        const double diglBand[2] = {150.0, 3000.0};
        const double usbBand[2]  = {300.0, 2700.0};
        const double lsbBand[2]  = {300.0, 2700.0};

        struct Case { const char* name; WdspChannel::Mode mode; const double* band;
                      bool wireUpper; };
        // wireUpper mirrors the USB/LSB assertions above: the wire order is
        // conjugated, so a USB-family mode lands on the LOWER wire bin.
        const Case cases[] = {
            {"USB",  WdspChannel::Mode::Usb,  usbBand,  false},
            {"DIGU", WdspChannel::Mode::Digu, diguBand, false},
            {"LSB",  WdspChannel::Mode::Lsb,  lsbBand,  true},
            {"DIGL", WdspChannel::Mode::Digl, diglBand, true},
        };

        for (const Case& c : cases) {
            const auto iq = modulate(c.mode, kTone, 0.5, 1.0, 1.0,
                                     nullptr, false, c.band);
            if (iq.empty()) {
                check(false, "modulation produced IQ for this mode");
                continue;
            }
            const double upper = binPower(iq, +kTone, kFsOut);
            const double lower = binPower(iq, -kTone, kFsOut);
            const double wanted = c.wireUpper ? upper : lower;
            const double other  = c.wireUpper ? lower : upper;
            const double suppDb = 20.0 * std::log10((wanted + 1e-12) / (other + 1e-12));
            std::fprintf(stderr,
                         "%-4s (passband %.0f..%.0f): +1kHz %.6f, -1kHz %.6f, "
                         "suppression %.1f dB\n",
                         c.name, c.band[0], c.band[1], upper, lower, suppDb);
            check(wanted > other, "sideband is correct for this mode");
            check(suppDb > 30.0, "opposite sideband suppressed by >30 dB");
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

    // ---- The mic slider lifts speech-level audio to something that modulates -
    //
    // This is the gap that made voice inaudible on the air: measured on the
    // radio, audio at -10 dBFS produced 1226 counts of forward power and audio
    // at -30 dBFS produced 47, while ordinary speech sits near -32 dBFS. That
    // measurement is why the gap must be closed by SOMETHING, and it has not
    // changed.
    //
    // WHAT CLOSES IT HAS. This case used to assert that the ALC lifted a
    // -34 dBFS tone toward full modulation on its own, at unity mic gain — up
    // to 40 dB of makeup, applied on an absolute threshold, which is what lifted
    // room noise level with speech (20.5 dB of separation in, 0.33 dB out) and
    // is the defect this stage's ceiling now forbids. The ALC only reduces.
    // The operator's mic slider closes the gap instead, which is why it reaches
    // +40 dB, so this case is re-pointed at the slider rather than deleted: the
    // same tone, the same destination, a different instrument.
    //
    // Two claims, and they are the pair: at the top of the slider the tone still
    // reaches full modulation, and at unity it now passes straight through
    // instead of being normalized.
    {
        constexpr double kQuiet = 0.02;         // about -34 dBFS, speech-ish
        const double kSlider100Linear = micSliderToLinear(100);
        const auto plain = modulate(WdspChannel::Mode::Usb, kTone, kQuiet, 1.0, 1.5,
                                    nullptr, false);
        const auto alced = modulate(WdspChannel::Mode::Usb, kTone, kQuiet, 1.0, 1.5,
                                    nullptr, true);
        const auto driven = modulate(WdspChannel::Mode::Usb, kTone, kQuiet,
                                     kSlider100Linear, 1.5, nullptr, true);
        if (!plain.empty() && !alced.empty() && !driven.empty()) {
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
            const double c = tailPeak(driven);
            std::fprintf(stderr,
                         "mic slider: quiet input peak %.4f -> ALC at unity %.4f "
                         "(%+.1f dB) -> slider 100 %.4f (%+.1f dB)\n",
                         a, b, 20.0 * std::log10((b + 1e-12) / (a + 1e-12)),
                         c, 20.0 * std::log10((c + 1e-12) / (a + 1e-12)));
            // The ALC adds nothing of its own. Anything but ~0 dB here is the
            // makeup half coming back.
            check(std::fabs(20.0 * std::log10((b + 1e-12) / (a + 1e-12))) < 1.0,
                  "at unity mic gain the ALC passes speech-level audio through "
                  "unchanged");
            // The same bound the old assertion used, now asked of the control
            // that is actually meant to close the gap.
            check(c > 0.5, "the mic slider at 100 brings quiet audio near full "
                           "modulation");
            // And the stage must never overshoot into clipping — an ALC that
            // overshoots transmits splatter rather than merely clipping our own
            // audio. This is measured on the DRIVEN run, which is the only one
            // that now reaches the ALC's target at all, and it is the whole of
            // what the stage still promises.
            double mx = 0.0;
            for (const auto& v : driven) mx = std::max(mx, static_cast<double>(std::abs(v)));
            check(mx <= 1.001, "ALC output never exceeds full scale");
        }
    }

    // ── The mic path preserves the separation between speech and the room ───
    //
    // THE REGRESSION TEST FOR THE REPORTED FAULT, and the direct successor of a
    // case that asserted the opposite. This block used to assert that the ALC
    // HELD its gain below -45 dBFS and lifted above it — makeup gain with an
    // absolute threshold, which sounds like a noise policy and is not one. The
    // threshold sat below a real shack's noise floor, so between words the loop
    // went on lifting until the room reached the same target peak as the voice:
    // measured on the air, 20.5 dB of speech-to-floor separation went in and
    // 0.33 dB came out. A stage that erases 20 dB of contrast is not protecting
    // anything.
    //
    // Rewritten rather than adjusted, because merely relaxing the old numbers
    // would leave "the ALC held" being asserted by a stage that no longer holds
    // anything — the first of its three assertions would still pass, for the
    // wrong reason, measuring "nothing is ever lifted".
    //
    // The property now under test is the one the report is about: the ALC adds
    // no gain at either level, and the DIFFERENCE between them survives the
    // stage intact. 20 dB in, 20 dB out.
    {
        struct Settled { double gainDb; double outPeak; };
        auto settled = [](double amplitude) -> Settled {
            Hl2TxDsp tx;
            Hl2TxDsp::Config cfg;
            cfg.mode = WdspChannel::Mode::Usb;
            cfg.alcEnabled = true;
            std::string err;
            if (!tx.configure(cfg, &err)) {
                std::fprintf(stderr, "FAIL: ALC separation configure: %s\n",
                             err.c_str());
                ++g_failures;
                return {0.0, 0.0};
            }
            double lastGainDb = 0.0;
            QObject::connect(&tx, &Hl2TxDsp::alcGain, &tx,
                             [&lastGainDb](float db) { lastGainDb = db; });
            std::vector<std::complex<float>> out;
            QObject::connect(&tx, &Hl2TxDsp::iqReady, &tx,
                             [&out](const std::vector<std::complex<float>>& iq) {
                out.insert(out.end(), iq.begin(), iq.end());
            });

            const int fs = cfg.inputSampleRateHz;
            const int total = fs * 3;   // long enough for the slow release to settle
            constexpr std::size_t kChunk = 240;
            std::vector<float> chunk(kChunk);
            for (int off = 0; off < total; off += static_cast<int>(kChunk)) {
                for (std::size_t n = 0; n < kChunk; ++n) {
                    chunk[n] = static_cast<float>(
                        amplitude * std::sin(2.0 * M_PI * 1000.0
                                             * (off + static_cast<int>(n)) / fs));
                }
                tx.processAudioBlock(chunk, TxAudioSource::Microphone);
            }
            // Settled tail only, so the opening blocks are not representative.
            double mx = 0.0;
            for (std::size_t i = out.size() / 2; i < out.size(); ++i)
                mx = std::max(mx, static_cast<double>(std::abs(out[i])));
            return {lastGainDb, mx};
        };

        // -54 dBFS: this is "the room" — the fan, the hiss, the pause between
        // words. It sat below the old -45 dBFS hold threshold on purpose, and
        // still sits below it in spirit: it is the leg that used to be hauled up.
        const Settled room = settled(0.002);
        // -34 dBFS: about where real speech sits, per the measurements quoted in
        // Hl2TxDsp::Config. Exactly 20 dB above the room leg.
        const Settled speech = settled(0.02);

        const double outSeparationDb =
            20.0 * std::log10((speech.outPeak + 1e-12) / (room.outPeak + 1e-12));
        std::fprintf(stderr,
                     "separation: room gain %.2f dB (peak %.6f), speech gain %.2f dB "
                     "(peak %.6f), 20.0 dB in -> %.2f dB out\n",
                     room.gainDb, room.outPeak, speech.gainDb, speech.outPeak,
                     outSeparationDb);
        // Neither leg is lifted. The ceiling is unity, so on any input below the
        // target the ALC's answer is 0 dB — for the room AND for the speech.
        check(std::fabs(room.gainDb) < 1.0,
              "the ALC adds no gain to room-level audio");
        check(std::fabs(speech.gainDb) < 1.0,
              "the ALC adds no gain to speech-level audio either");
        // AND THE ONE THAT WOULD HAVE CAUGHT THE FAULT. Both legs at 0 dB is
        // necessary but not sufficient: what the operator hears is the contrast,
        // and the reported failure was 20.5 dB in arriving as 0.33 dB out.
        check(outSeparationDb > 19.0 && outSeparationDb < 21.0,
              "20 dB of input separation arrives as 20 dB of output separation");
    }

    // ── #4796: client-leveled audio bypasses the ALC entirely ──────────────
    //
    // A TCI/DAX client's level control is a digital attenuator on the audio it
    // streams (WSJT-X's Pwr slider). Through the ALC's makeup half that control
    // was either normalized away (above the then-current -45 dBFS hold
    // threshold) or frozen into a path-dependent gain (below it). Output must
    // simply be proportional to input — and the quiet leg is 5 dB below where
    // that threshold used to sit, which is where the ALC used to freeze. That
    // ceiling is now unity on every path rather than only this one, so these
    // assertions read the same as they did; they are unchanged on purpose.
    {
        // -50 dBFS and -30 dBFS, both with the ALC configured ON, both marked
        // client-leveled. 20 dB apart in, 20 dB apart out.
        const auto quiet = modulate(WdspChannel::Mode::Usb, kTone, 0.00316, 1.0,
                                    1.0, nullptr, true, nullptr,
                                    TxAudioSource::ClientLeveled);
        const auto loud  = modulate(WdspChannel::Mode::Usb, kTone, 0.0316, 1.0,
                                    1.0, nullptr, true, nullptr,
                                    TxAudioSource::ClientLeveled);
        if (!quiet.empty() && !loud.empty()) {
            const double a = binPower(quiet, kTone, kFsOut);
            const double b = binPower(loud, kTone, kFsOut);
            const double ratioDb = 20.0 * std::log10((b + 1e-12) / (a + 1e-12));
            std::fprintf(stderr,
                         "client-leveled: -50 dBFS %.6f, -30 dBFS %.6f, ratio %.1f dB\n",
                         a, b, ratioDb);
            check(ratioDb > 18.0 && ratioDb < 22.0,
                  "client-leveled output is proportional to input (no ALC)");
            // And the quiet tone was NOT hauled up toward the ALC target.
            double mx = 0.0;
            for (const auto& v : quiet) mx = std::max(mx, static_cast<double>(std::abs(v)));
            check(mx < 0.05,
                  "a -50 dBFS client tone stays near -50 dBFS on the wire");
        }
    }

    // ── #4796: the reported hysteresis, as an assertion ─────────────────────
    //
    // One continuous transmission, level swept down-up-down across the hold
    // threshold: -50 dBFS -> -30 dBFS -> -50 dBFS. This is the report's "slide
    // the Pwr slider up, RF appears; slide it back, RF stays" — with the ALC in
    // the path, the third segment came out ~26 dB hotter than the first,
    // because the gain wound up during the loud segment was frozen (not
    // reducing, input below hold) instead of released. Client-leveled audio
    // must be path-independent: equal inputs, equal outputs, whatever came
    // between.
    {
        Hl2TxDsp tx;
        Hl2TxDsp::Config cfg;
        cfg.mode = WdspChannel::Mode::Usb;
        cfg.alcEnabled = true;   // configured ON — the bypass is per-block
        std::string err;
        if (!tx.configure(cfg, &err)) {
            std::fprintf(stderr, "FAIL: hysteresis configure: %s\n", err.c_str());
            ++g_failures;
        } else {
            std::vector<std::complex<float>> out;
            QObject::connect(&tx, &Hl2TxDsp::iqReady, &tx,
                             [&](const std::vector<std::complex<float>>& iq) {
                out.insert(out.end(), iq.begin(), iq.end());
            });

            const int fs = cfg.inputSampleRateHz;
            const double levels[] = {0.00316, 0.0316, 0.00316};
            std::size_t marks[4] = {0, 0, 0, 0};
            constexpr std::size_t kChunk = 240;
            std::vector<float> chunk(kChunk);
            int sample = 0;
            for (int stage = 0; stage < 3; ++stage) {
                for (int off = 0; off < fs; off += static_cast<int>(kChunk)) {
                    for (std::size_t n = 0; n < kChunk; ++n, ++sample) {
                        chunk[n] = static_cast<float>(
                            levels[stage]
                            * std::sin(2.0 * M_PI * 1000.0 * sample / fs));
                    }
                    tx.processAudioBlock(chunk, TxAudioSource::ClientLeveled);
                }
                marks[stage + 1] = out.size();
            }

            // Compare the settled tail of each quiet segment (its second half).
            auto tailPeak = [&](std::size_t from, std::size_t to) {
                double mx = 0.0;
                for (std::size_t i = from + (to - from) / 2; i < to; ++i)
                    mx = std::max(mx, static_cast<double>(std::abs(out[i])));
                return mx;
            };
            const double firstQuiet = tailPeak(marks[0], marks[1]);
            const double loud       = tailPeak(marks[1], marks[2]);
            const double lastQuiet  = tailPeak(marks[2], marks[3]);
            const double reGainDb =
                20.0 * std::log10((lastQuiet + 1e-12) / (firstQuiet + 1e-12));
            std::fprintf(stderr,
                         "hysteresis: quiet %.6f -> loud %.6f -> quiet %.6f "
                         "(re-gain %.2f dB)\n",
                         firstQuiet, loud, lastQuiet, reGainDb);
            check(std::fabs(reGainDb) < 1.0,
                  "equal client levels produce equal output before and after a "
                  "loud excursion (no ALC hysteresis)");
            check(loud > firstQuiet * 5.0,
                  "the loud segment is genuinely louder (sweep is real)");
        }
    }

    // ── #4796 review: the bypass is ONE-SIDED — an over-level client is ────
    //    limited, not clipped.
    //
    // Removing the ALC's makeup half is the #4796 fix. Removing its REDUCTION
    // half would leave the modulator's hard clamp as the only thing between a
    // hot client and the band: m_micGain now reaches 100x (+40 dB — the TX gain
    // slider at 100, Hl2TxLevelPolicy.h), and even the 10x used below puts a
    // full-scale client ~17 dB into that clamp. Flat-topping an SSB modulator
    // input is a splatter generator: the one failure mode here that harms other
    // operators rather than the operator who caused it.
    //
    // Distortion is measured as the CREST FACTOR of the analytic magnitude,
    // not as a harmonic bin. For a single tone |IQ| is constant, so peak/mean
    // is 1.0 however the tone is scaled; clipping ripples it. A DFT bin ratio
    // was tried first and rejected: binPower's absolute output is dominated by
    // frequency-dependent cancellation, so it read the same -5.8 dBc on a tone
    // that provably could not be clipping. It is a same-frequency comparator
    // (which is all the proportionality case above asks of it), not a
    // distortion meter. The clean control below stays in the test so the
    // instrument is validated on every run rather than on the day it was
    // written.
    {
        // A raw linear +20 dB, NOT a slider position. This was written as
        // micSliderToLinear(100) back when the slider topped out at +20 dB; the
        // slider now reaches +40 dB (100x) and the name would be a lie. The
        // value is what the case needs — enough to drive a full-scale client
        // well into limiting — so it is kept and renamed rather than moved.
        constexpr double kHotMicGain = 10.0;   // +20 dB, linear
        constexpr double kHarmTone  = 700.0;
        auto crest = [](const std::vector<std::complex<float>>& v) {
            double mx = 0.0, sum = 0.0;
            for (const auto& x : v) {
                const double m = std::abs(x);
                mx = std::max(mx, m);
                sum += m;
            }
            return mx / (sum / static_cast<double>(v.size()) + 1e-12);
        };
        auto settledTail = [](const std::vector<std::complex<float>>& v) {
            return std::vector<std::complex<float>>(
                v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
        };

        const auto hot = modulate(WdspChannel::Mode::Usb, kHarmTone, 1.0,
                                  kHotMicGain, 1.5, nullptr, true, nullptr,
                                  TxAudioSource::ClientLeveled);
        // 24 dB below the ALC target at unity mic gain: the limiter cannot
        // engage, so this is what "undistorted" reads on this instrument.
        const auto clean = modulate(WdspChannel::Mode::Usb, kHarmTone, 0.05,
                                    1.0, 1.5, nullptr, true, nullptr,
                                    TxAudioSource::ClientLeveled);
        if (!hot.empty() && !clean.empty()) {
            const auto tail  = settledTail(hot);     // skips the 5 ms attack
            const auto ctail = settledTail(clean);
            double mx = 0.0;
            for (const auto& v : tail)
                mx = std::max(mx, static_cast<double>(std::abs(v)));
            const double hotCrest   = crest(tail);
            const double cleanCrest = crest(ctail);
            std::fprintf(stderr,
                         "over-level client: settled |IQ| peak %.3f, crest %.4f "
                         "(undistorted control %.4f)\n",
                         mx, hotCrest, cleanCrest);
            check(cleanCrest < 1.05,
                  "crest-factor instrument reads ~1.0 on an undistorted tone");
            check(mx < 0.95,
                  "a full-scale client at +20 dB of mic gain is limited below "
                  "the clamp, not flat-topped by it");
            check(mx > 0.5,
                  "the limited transmission is still on the air (case is real)");
            check(hotCrest < 1.05,
                  "an over-level client is limited cleanly, with no clipping "
                  "distortion on the wire");
        }
    }

    // ── #4796 review: the reduction half must RELEASE — it may not latch ────
    //
    // Gating only the ALC's INCREASE branch (leaving `reducing` free to act) is
    // the smaller edit and is wrong: once a loud block pulls the gain down,
    // nothing can raise it again within the over, so a client sliding back down
    // stays attenuated at whatever the loudest block called for. That is
    // path-dependent gain — #4796's own defect class, mirrored. Expressing the
    // bypass as a unity CEILING instead leaves the gain free to release.
    //
    // This case is the discriminator between those two shapes. It passes on a
    // total bypass and on the ceiling; it fails ~21 dB wide on an
    // increase-gated ALC.
    //
    // HALF OF THAT RATIONALE HAS RETIRED, and the half that has not is the
    // reason this case is untouched by the unity-ceiling change.
    //
    // It used to be the discriminator for a second thing as well: the hold being
    // made mic-path-only (`!clientLeveled` in processAudioBlock's `held`). There
    // is no hold any more — it was deleted outright with the ALC's makeup half,
    // because under a unity ceiling a quiet block wants target = 1.0, so after
    // any reduction `reducing` is false and a hold would strand the gain exactly
    // as described above. So that discriminator has nothing left to discriminate
    // between, and the paragraph is kept as history rather than as a live claim.
    //
    // WHAT SURVIVES IS THE PROOF THAT THE UNITY CEILING CHANGED NOTHING HERE. On
    // the client path the ceiling was already 1.0 and `held` was already false,
    // so every assertion below reads the same before and after — which is what
    // makes this case the evidence that a change to the mic path is a no-op on
    // TCI/DAX.
    //
    // THE QUIET LEG IS STILL DELIBERATELY WHERE IT IS: -70 dBFS times this
    // case's 10x mic gain is -50 dBFS at the ALC's measurement point, which was
    // under the old -45 dBFS hold threshold. Do not raise it. It is what keeps
    // the case able to catch a hold reappearing, on either path, by any route.
    //
    // One continuous transmission: full scale, then -70 dBFS held for four
    // release time constants. The tail must match the same level keyed fresh.
    {
        // A raw linear +20 dB, not a slider position — see the note in the case
        // above. The slider reaches +40 dB now; this value does not need to.
        constexpr double kHotMicGain = 10.0;
        const auto fresh = modulate(WdspChannel::Mode::Usb, kTone, 0.000316,
                                    kHotMicGain, 1.5, nullptr, true, nullptr,
                                    TxAudioSource::ClientLeveled);

        Hl2TxDsp tx;
        Hl2TxDsp::Config cfg;
        cfg.mode = WdspChannel::Mode::Usb;
        cfg.alcEnabled = true;
        std::string err;
        if (!tx.configure(cfg, &err)) {
            std::fprintf(stderr, "FAIL: limiter release configure: %s\n",
                         err.c_str());
            ++g_failures;
        } else if (!fresh.empty()) {
            tx.setMicGain(kHotMicGain);
            std::vector<std::complex<float>> out;
            QObject::connect(&tx, &Hl2TxDsp::iqReady, &tx,
                             [&](const std::vector<std::complex<float>>& iq) {
                out.insert(out.end(), iq.begin(), iq.end());
            });
            double lastGainDb = 0.0;
            QObject::connect(&tx, &Hl2TxDsp::alcGain, &tx,
                             [&lastGainDb](float db) { lastGainDb = db; });

            const int fs = cfg.inputSampleRateHz;
            constexpr std::size_t kChunk = 240;
            std::vector<float> chunk(kChunk);
            const double levels[]  = {1.0, 0.000316};
            const int    stageSec[] = {1, 2};
            int sample = 0;
            std::size_t afterLoud = 0;
            for (int stage = 0; stage < 2; ++stage) {
                for (int off = 0; off < fs * stageSec[stage];
                     off += static_cast<int>(kChunk)) {
                    for (std::size_t n = 0; n < kChunk; ++n, ++sample) {
                        chunk[n] = static_cast<float>(
                            levels[stage]
                            * std::sin(2.0 * M_PI * 1000.0 * sample / fs));
                    }
                    tx.processAudioBlock(chunk, TxAudioSource::ClientLeveled);
                }
                if (stage == 0)
                    afterLoud = out.size();
            }

            double swept = 0.0;
            for (std::size_t i = afterLoud + (out.size() - afterLoud) / 2;
                 i < out.size(); ++i)
                swept = std::max(swept, static_cast<double>(std::abs(out[i])));
            double ref = 0.0;
            for (std::size_t i = fresh.size() / 2; i < fresh.size(); ++i)
                ref = std::max(ref, static_cast<double>(std::abs(fresh[i])));
            const double deltaDb =
                20.0 * std::log10((swept + 1e-12) / (ref + 1e-12));
            std::fprintf(stderr,
                         "limiter release: swept %.6f vs fresh-key %.6f "
                         "(delta %.2f dB, settled ALC %.2f dB)\n",
                         swept, ref, deltaDb, lastGainDb);
            check(std::fabs(deltaDb) < 1.0,
                  "the limiter releases to unity after an over-level excursion "
                  "(reduction does not latch)");
            check(std::fabs(lastGainDb) < 1.0,
                  "ALC gain is back at 0 dB once the client is quiet again");
        }
    }

    // ── The engine's own generated audio keeps the level it was generated at ──
    //
    // WSPR, the AX.25 modem and the RADE waveform reach the modulator as
    // TxAudioSource::EngineGenerated. Two properties, and the second is the one
    // that was actually broken on the air.
    //
    // 1. THE MIC SLIDER DOES NOT REACH IT. A slider set for a voice is not a
    //    control over an unattended beacon. Before the source enum, engine
    //    audio was indistinguishable from mic audio and moved with it.
    // 2. THE LEVEL SURVIVES. A generator emitting -20 dBFS transmits -20 dBFS.
    //
    // Both were invisible while the ALC carried 40 dB of makeup, because it
    // normalised every generated level onto its target. The bench measured the
    // consequence once the makeup went: 18.58 dB of unattended shortfall
    // (bench-runner runs/wspr-unattended-ab, both binaries, same stimulus).
    {
        constexpr double kGenerated = 0.1;      // -20.000 dBFS, the WSPR default
        float mp = 0.0f;
        auto peak = [](const std::vector<std::complex<float>>& iq) {
            double m = 0.0;
            for (const auto& v : iq) m = std::max(m, static_cast<double>(std::abs(v)));
            return m;
        };

        const double pUnity = peak(modulate(WdspChannel::Mode::Usb, kTone,
            kGenerated, 1.0, 1.0, &mp, true, nullptr, TxAudioSource::EngineGenerated));
        const double pUp    = peak(modulate(WdspChannel::Mode::Usb, kTone,
            kGenerated, 100.0, 1.0, &mp, true, nullptr, TxAudioSource::EngineGenerated));
        const double pDown  = peak(modulate(WdspChannel::Mode::Usb, kTone,
            kGenerated, 0.1, 1.0, &mp, true, nullptr, TxAudioSource::EngineGenerated));

        const double spreadDb = 20.0 * std::log10(
            std::max(pUp, pDown) / std::max(1e-12, std::min(pUp, pDown)));
        std::fprintf(stderr,
            "engine-generated: mic 1x %.6f, 100x %.6f, 0.1x %.6f -> spread %.2f dB\n",
            pUnity, pUp, pDown, spreadDb);
        check(spreadDb < 0.5,
              "the mic slider does not reach engine-generated audio");

        // And the mic path is untouched by all of it. This one is also the
        // CONTROL for the level assertion below.
        const double micUnity = peak(modulate(WdspChannel::Mode::Usb, kTone,
            kGenerated, 1.0, 1.0, &mp, true));

        // Consistency alone is not enough -- three identical WRONG answers
        // would pass the spread check. The level must be RIGHT, and "right" is
        // measured rather than asserted against a constant: the modulator has
        // its own scale factor (an amplitude of 0.5 leaves as |IQ| 0.52, see
        // the diag line above), so dividing by a guessed number would test the
        // guess. The microphone path at UNITY mic gain applies no gain either,
        // so it IS the reference -- and the claim becomes exactly what it
        // should be: engine-generated audio comes out where mic audio would
        // with the slider at unity, whatever the slider actually says.
        const double vsControlDb =
            20.0 * std::log10(std::max(1e-12, pUnity / micUnity));
        std::fprintf(stderr,
            "engine-generated vs mic-at-unity control: %+.3f dB\n", vsControlDb);
        check(std::fabs(vsControlDb) < 0.1,
              "engine-generated audio transmits at the level it was generated at");
        const double micHalf  = peak(modulate(WdspChannel::Mode::Usb, kTone,
            kGenerated, 0.5, 1.0, &mp, true));
        const double micDropDb =
            20.0 * std::log10(std::max(1e-12, micHalf / micUnity));
        std::fprintf(stderr,
            "mic path still follows the slider: %.2f dB for a 2:1 cut\n", micDropDb);
        check(micDropDb < -3.0, "the mic slider still moves microphone audio");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_txdsp_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
