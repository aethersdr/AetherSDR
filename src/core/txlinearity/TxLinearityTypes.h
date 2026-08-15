#pragma once

#include <complex>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// TX Linearity Analyzer — shared value types.
//
// Everything in `AetherSDR::txlin` is deliberately backend-agnostic: no Qt, no
// FlexRadio types, no radio-model dependency. The analyzer is the measurement
// half of a digital-predistortion loop with the correction half removed, and
// the HL2 backend will eventually need exactly this core with a different
// capture adapter bolted underneath. Coupling it to Flex specifics now would
// mean rewriting the DSP later, which is the one part that must not be
// rewritten — it is the part with the numbers in it.
//
// The one dependency the core does take is FFTW3, which is already a hard
// requirement of the tree (vendored WDSP needs it) — see SpectralNR and
// Hl2Spectrum for the existing precedent.
namespace AetherSDR::txlin {

// ── Reference conventions ───────────────────────────────────────────────────
//
// The single most common way to publish a wrong IMD number is to omit which
// reference it is against, so this type exists to make the choice unskippable
// at every boundary that carries a dBc value.
//
// For two equal-amplitude tones of complex-baseband amplitude A each:
//   - one tone's average power          = A^2
//   - envelope peak amplitude           = 2A
//   - PEP (peak envelope power)         = (2A)^2 = 4A^2
// so PEP sits exactly 10*log10(4) = 6.0206 dB above a single tone. Ham-radio
// convention usually quotes IMD relative to ONE TONE; ARRL lab measurements are
// referenced to PEP, which reads 6 dB better for the identical signal. Both are
// correct; a bare "IMD3 = -32 dB" is not.
enum class ImdReference {
    PerTone,  // dBc relative to one of the two tones
    Pep,      // dBc relative to peak envelope power (PerTone - 6.02 dB)
};

// The exact PEP-to-single-tone offset for a balanced two-tone, in dB.
// 10*log10(4). Named rather than spelled 6.02 inline because §9's reference
// convention test asserts on it and a literal would drift.
inline constexpr double kPepOverToneDb = 6.020599913279624;

// ── Capture ─────────────────────────────────────────────────────────────────

struct CaptureConfig {
    // Highest rate the pan bandwidth allows. For a two-tone with spacing df,
    // IMD3 sits at +/-df beyond the tones and IMD7 at +/-3df; at df = 2 kHz that
    // is +/-8 kHz of products, and we want considerably more than that so window
    // leakage from the capture band edges never reaches the measurement region.
    // 192 kHz is the top DAX IQ rate a FLEX offers, so it is the default.
    double sampleRateHz = 192000.0;

    // 16384 bins at 192 kHz is 11.7 Hz/bin — enough to resolve products with
    // 1 kHz tone spacing with many bins of guard either side.
    int frameSamples = 16384;

    double centerFreqHz = 0.0;

    // PA thermal and ALC settling after key-down, and the fall of the envelope
    // before key-up. Samples inside these guards are not steady-state and
    // measuring them reports a transient as distortion.
    int settleMsAfterKey = 50;
    int settleMsBeforeUnkey = 50;
};

// One block of aligned capture handed from the capture thread to the analyzer.
//
// `reference` is engaged only on a backend that can supply the radio's own
// ideal TX signal (HL2 Phase 3). On a FLEX it is always nullopt, because DAX IQ
// is unidirectional and there is no path by which the radio's modulator output
// reaches the host as IQ. Every consumer must degrade gracefully rather than
// fabricate one — see ITxCaptureSource::hasReferenceChannel().
struct CaptureFrame {
    std::vector<std::complex<float>> feedback;
    std::optional<std::vector<std::complex<float>>> reference;
    double sampleRateHz = 0.0;
    double centerFreqHz = 0.0;
    std::uint64_t timestampNs = 0;
    // ADC/path overload somewhere in this frame. Judged by magnitude-histogram
    // saturation across the frame, not by one sample touching full scale — a
    // single clipped sample in 16384 is a statistical certainty on a signal
    // whose peaks are the point, while a *population* piled against full scale
    // is the thing that ruins the measurement.
    bool clipped = false;
};

// ── Measurement results ─────────────────────────────────────────────────────

// One estimated spectral tone. `powerDb` is in dB relative to the analyzer's
// full-scale complex amplitude of 1.0, i.e. dBFS — the capture path delivers
// normalized IQ and the analyzer never learns the antenna-side absolute power,
// so every ratio below is a difference of these and is therefore absolute-scale
// independent, which is exactly what a linearity measurement wants.
struct TonePeak {
    bool valid = false;
    double freqHz = 0.0;
    double powerDb = 0.0;
};

struct ImdProduct {
    int order = 3;          // 3, 5 or 7
    bool upper = false;     // false: 2f1-f2 side. true: 2f2-f1 side.
    double freqHz = 0.0;
    double powerDb = 0.0;   // dBFS, same reference as TonePeak::powerDb

    // The two conventions, always both, always labelled. dbcPep is
    // dbcPerTone - kPepOverToneDb by construction for a balanced pair; it is
    // computed from the measured PEP rather than by subtracting the constant,
    // so an imbalanced pair reports the truth instead of the idealisation.
    double dbcPerTone = 0.0;
    double dbcPep = 0.0;

    // The product's peak is not far enough above the instrument's own noise
    // floor for the number to mean anything. A measurement that is actually
    // reading the analyzer's noise, or the feedback receiver's own IMD, is
    // worse than no measurement, so this flag exists to let the UI say so
    // instead of printing a confident wrong number.
    bool limitedByNoiseFloor = false;
};

// Adjacent-channel / shoulder measurement over an explicit band, so the caller
// can see exactly what was integrated rather than trusting an offset label.
struct BandPower {
    double lowHz = 0.0;   // relative to the two-tone centre
    double highHz = 0.0;
    double powerDb = 0.0;     // integrated power, dBFS
    double dbcPerTone = 0.0;
    double dbcPep = 0.0;
};

struct LinearityResult {
    bool valid = false;
    // Populated whenever valid == false. Human-readable and shown verbatim in
    // the UI; the analyzer refusing to answer is a first-class result.
    std::string invalidReason;

    double sampleRateHz = 0.0;
    double binHz = 0.0;
    int fftSize = 0;
    int averages = 0;

    TonePeak tone1;   // lower
    TonePeak tone2;   // upper
    double toneSpacingHz = 0.0;
    // tone2 - tone1 in dB. Beyond ~0.5 dB the IMD interpretation changes
    // (products are no longer symmetric and the PEP relation stops holding),
    // so the UI warns rather than silently reporting a number that assumes
    // balance.
    double toneImbalanceDb = 0.0;

    double pepDb = 0.0;  // measured peak envelope power, dBFS

    // Instrument noise floor, dBFS, PER ANALYSIS BIN — directly comparable
    // against TonePeak::powerDb and ImdProduct::powerDb, which is the whole
    // point: it is the level a product has to clear to mean anything.
    //
    // Note this includes the window's equivalent noise bandwidth. A bin's noise
    // power is 2*sigma^2 * ENBW / N, not 2*sigma^2 / N; for the default
    // Blackman-Harris 7 that is 4.2 dB higher than the naive figure. Reporting
    // the naive one would make every product look 4 dB more trustworthy than it
    // is.
    double noiseFloorDb = 0.0;

    std::vector<ImdProduct> products;
    std::optional<BandPower> shoulderLow;
    std::optional<BandPower> shoulderHigh;
    std::optional<BandPower> acprLow;
    std::optional<BandPower> acprHigh;

    bool clipped = false;

    // Welch-averaged magnitude spectrum in dBFS, DC-centred (DC at index
    // fftSize/2) to match the convention Hl2Spectrum already uses so the plot
    // code has one mental model, not two.
    std::vector<float> spectrumDb;
};

}  // namespace AetherSDR::txlin
