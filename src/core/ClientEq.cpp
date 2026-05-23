#include "ClientEq.h"

#include <algorithm>
#include <cmath>

namespace MasterSDR {

namespace {

constexpr float kPi    = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

// Minimum difference in smoothed params before we bother recomputing the
// biquad. Tuned so sub-audible parameter motion doesn't burn CPU on
// coefficient updates we can't hear.
constexpr float kFreqEpsHz = 0.05f;
constexpr float kGainEpsDb = 0.001f;
constexpr float kQEps      = 0.0005f;

// One-pole smoother time constant (samples to reach ~63% of target) —
// 15ms at 24 kHz is ~360 samples, computed in prepare().
constexpr float kSmoothTimeConstantSec = 0.015f;

// Chebyshev I passband ripple (dB) — classic 1 dB Chebyshev default.
constexpr float kChebyshevRippleDb = 1.0f;
// Elliptic passband ripple and stopband attenuation.
constexpr float kEllipticRippleDb  = 1.0f;
constexpr float kEllipticStopDb    = 60.0f;

float dbToAmp(float db) noexcept
{
    // A = 10^(dB / 40) — the sqrt() form used in RBJ peaking/shelf
    return std::pow(10.0f, db * 0.025f);
}

// Per-family Q values for each biquad section of an even-order cascade.
// numSections is the order / 2 (so order 4 → 2 sections, order 8 → 4).
// Section index 0 is the "broadest" (lowest Q); the highest index is the
// most resonant.  For Chebyshev/Elliptic we also return an (optional)
// per-section frequency scale relative to the nominal cutoff.
struct SectionDesign {
    float q;
    float freqScale;  // multiply nominal cutoff by this
};

// Butterworth: flat-maximal, Q_k = 1 / (2·cos((2k-1)·π / (2N)))
// All sections share the nominal cutoff (freqScale = 1).
SectionDesign butterworthSection(int k, int numSections)
{
    const int N = 2 * numSections;
    const float angle = static_cast<float>((2 * k + 1) * kPi) /
                        static_cast<float>(2 * N);
    return { 1.0f / (2.0f * std::sin(angle)), 1.0f };
    // Note: sin form of 1/(2·cos(θ)): we want angle from jω axis;
    // standard Butterworth pole angle measured from the negative real
    // axis is (2k-1)π/(2N) for k=1..N/2 — but using sin here rotates
    // into the correct quadrant.  Q = 1/(2·sin(θ_k)) with θ measured
    // from jω axis works out algebraically the same.
}

// Chebyshev Type I: poles on an ellipse.  Ripple in passband, flat
// stopband, steeper transition than Butterworth.
// Pole: p_k = -sinh(ν)·sin((2k-1)π/2N) + j·cosh(ν)·cos((2k-1)π/2N)
// where ν = asinh(1/ε)/N, ε = sqrt(10^(rippleDb/10) - 1)
// Natural frequency of section k:  ω_k = |p_k|
// Q_k = ω_k / (2·|Re(p_k)|)
SectionDesign chebyshevSection(int k, int numSections, float rippleDb)
{
    const int N = 2 * numSections;
    const float eps = std::sqrt(std::pow(10.0f, rippleDb / 10.0f) - 1.0f);
    const float nu  = std::asinh(1.0f / eps) / static_cast<float>(N);
    const float theta = static_cast<float>((2 * k + 1) * kPi) /
                        static_cast<float>(2 * N);
    const float re = -std::sinh(nu) * std::sin(theta);
    const float im =  std::cosh(nu) * std::cos(theta);
    const float mag = std::sqrt(re * re + im * im);
    const float q   = mag / (2.0f * std::fabs(re));
    return { q, mag };
}

// Bessel: maximally flat group delay.  Pole locations come from reverse
// Bessel polynomials — we use tabulated normalised values for orders up
// to 8 (which covers 48 dB/oct cap).  Frequencies are normalised so the
// -3 dB point sits at ω=1, so freqScale equals pole radius as-is.
SectionDesign besselSection(int k, int numSections)
{
    // Pole magnitudes and Qs for Bessel filters normalized to -3 dB.
    // Values from "Filter Design for Signal Processing" (Shpak) /
    // Analog Devices MT-224 app note, verified against scipy.signal.
    struct Table { float mag; float q; };
    // Indexed by [numSections-1][section_idx]
    static constexpr Table kBessel2[1] = { { 1.2736f, 0.5773f } };
    static constexpr Table kBessel4[2] = {
        { 1.4192f, 0.5219f },
        { 1.5912f, 0.8055f },
    };
    static constexpr Table kBessel6[3] = {
        { 1.6060f, 0.5103f },
        { 1.6913f, 0.6112f },
        { 1.9071f, 1.0234f },
    };
    static constexpr Table kBessel8[4] = {
        { 1.7837f, 0.5060f },
        { 1.8376f, 0.5596f },
        { 1.9591f, 0.6608f },
        { 2.1953f, 1.2257f },
    };
    const Table* t = nullptr;
    if      (numSections == 1) t = kBessel2;
    else if (numSections == 2) t = kBessel4;
    else if (numSections == 3) t = kBessel6;
    else                       t = kBessel8;
    const int idx = std::clamp(k, 0, numSections - 1);
    return { t[idx].q, t[idx].mag };
}

// Elliptic (Cauer): steepest rolloff for a given order, with both
// passband ripple and stopband ripple.  Real implementation requires
// Jacobi elliptic functions and is a project on its own.  For this
// pass we approximate with a Chebyshev-II pole geometry — sharp knee
// without full stopband zero placement — which gives a visibly steeper
// cutoff than Butterworth while keeping the audio path numerically
// stable.  Full elliptic (with zeros) can replace this in a follow-up.
SectionDesign ellipticSection(int k, int numSections)
{
    // Treat as a tighter Chebyshev for visual/audible distinction.
    SectionDesign c = chebyshevSection(k, numSections, 0.5f);
    c.q *= 1.15f;  // modest Q boost to mimic elliptic's steeper transition
    return c;
}

SectionDesign designSection(ClientEq::FilterFamily family,
                            int k, int numSections)
{
    using F = ClientEq::FilterFamily;
    switch (family) {
    case F::Butterworth: return butterworthSection(k, numSections);
    case F::Chebyshev:   return chebyshevSection(k, numSections,
                                                 kChebyshevRippleDb);
    case F::Bessel:      return besselSection(k, numSections);
    case F::Elliptic:    return ellipticSection(k, numSections);
    }
    return butterworthSection(k, numSections);
}

int slopeToSections(int slopeDbPerOct)
{
    // Quantise to supported steps: 12 / 24 / 36 / 48 dB/oct → 1 / 2 / 3 / 4
    // cascade sections.  Anything lower clamps to 1 section (12 dB/oct).
    if (slopeDbPerOct >= 48) return 4;
    if (slopeDbPerOct >= 36) return 3;
    if (slopeDbPerOct >= 24) return 2;
    return 1;
}

} // namespace

ClientEq::ClientEq()
{
    // Default: all bands initialised with reasonable peak-at-1kHz values
    // (already handled by AtomicBand default member initialisers). Just
    // mark runtime state as matching so we don't recompute on first run.
    for (int i = 0; i < kMaxBands; ++i) {
        m_runtime[i].current.freqHz = m_bands[i].freqHz.load(std::memory_order_relaxed);
        m_runtime[i].current.gainDb = m_bands[i].gainDb.load(std::memory_order_relaxed);
        m_runtime[i].current.q      = m_bands[i].q.load(std::memory_order_relaxed);
        m_runtime[i].cachedType     = static_cast<FilterType>(
            m_bands[i].type.load(std::memory_order_relaxed));
        m_runtime[i].cachedEnabled  = m_bands[i].enabled.load(std::memory_order_relaxed);
        m_runtime[i].cachedSlopeDbPerOct = m_bands[i].slopeDbPerOct.load(std::memory_order_relaxed);
        m_runtime[i].activeSections = 1;
    }
}

void ClientEq::setFilterFamily(FilterFamily f) noexcept
{
    m_filterFamily.store(static_cast<int>(f), std::memory_order_relaxed);
    // Bump every band's version so the audio thread recomputes cascade
    // coefficients on the next block.  Family change invalidates them all.
    for (int i = 0; i < kMaxBands; ++i) {
        m_bands[i].version.fetch_add(1, std::memory_order_release);
    }
}

ClientEq::FilterFamily ClientEq::filterFamily() const noexcept
{
    return static_cast<FilterFamily>(
        m_filterFamily.load(std::memory_order_relaxed));
}

void ClientEq::prepare(double sampleRate)
{
    m_sampleRate = sampleRate;
    // One-pole smoother coefficient: y += a * (target - y).
    // a = 1 - exp(-1 / (fs * τ)). Approximated via 1 / (fs * τ) for small τ.
    m_smoothCoeff = 1.0f - std::exp(-1.0f / static_cast<float>(sampleRate * kSmoothTimeConstantSec));

    // Force coefficient recompute on first process() call
    for (int i = 0; i < kMaxBands; ++i) {
        computeCoefficients(m_runtime[i]);
        for (auto& s : m_runtime[i].sections) {
            s.z1L = s.z2L = 0.0f;
            s.z1R = s.z2R = 0.0f;
        }
    }
}

void ClientEq::setEnabled(bool on) noexcept
{
    m_enabled.store(on, std::memory_order_release);
}

bool ClientEq::isEnabled() const noexcept
{
    return m_enabled.load(std::memory_order_acquire);
}

void ClientEq::setMasterGain(float linear) noexcept
{
    m_masterGain.store(std::clamp(linear, 0.0f, 4.0f), std::memory_order_relaxed);
}

float ClientEq::masterGain() const noexcept
{
    return m_masterGain.load(std::memory_order_relaxed);
}

void ClientEq::setBand(int idx, const BandParams& p) noexcept
{
    if (idx < 0 || idx >= kMaxBands) return;
    AtomicBand& b = m_bands[idx];
    b.freqHz.store(std::clamp(p.freqHz, 10.0f, 20000.0f), std::memory_order_relaxed);
    b.gainDb.store(std::clamp(p.gainDb, -24.0f, 24.0f),   std::memory_order_relaxed);
    b.q     .store(std::clamp(p.q,      0.1f,  18.0f),    std::memory_order_relaxed);
    b.type  .store(static_cast<int>(p.type),              std::memory_order_relaxed);
    b.enabled.store(p.enabled,                            std::memory_order_relaxed);
    b.slopeDbPerOct.store(std::clamp(p.slopeDbPerOct, 12, 48),
                          std::memory_order_relaxed);
    // Bump version last so audio thread sees a consistent snapshot
    b.version.fetch_add(1, std::memory_order_release);
}

ClientEq::BandParams ClientEq::band(int idx) const noexcept
{
    BandParams p;
    if (idx < 0 || idx >= kMaxBands) return p;
    const AtomicBand& b = m_bands[idx];
    p.freqHz  = b.freqHz.load(std::memory_order_relaxed);
    p.gainDb  = b.gainDb.load(std::memory_order_relaxed);
    p.q       = b.q.load(std::memory_order_relaxed);
    p.type    = static_cast<FilterType>(b.type.load(std::memory_order_relaxed));
    p.enabled = b.enabled.load(std::memory_order_relaxed);
    p.slopeDbPerOct = b.slopeDbPerOct.load(std::memory_order_relaxed);
    return p;
}

void ClientEq::setActiveBandCount(int n) noexcept
{
    m_activeBandCount.store(std::clamp(n, 0, kMaxBands), std::memory_order_release);
}

int ClientEq::activeBandCount() const noexcept
{
    return m_activeBandCount.load(std::memory_order_acquire);
}

void ClientEq::reset() noexcept
{
    for (int i = 0; i < kMaxBands; ++i) {
        for (auto& s : m_runtime[i].sections) {
            s.z1L = s.z2L = 0.0f;
            s.z1R = s.z2R = 0.0f;
        }
    }
}

namespace {

// Helper: analog-prototype magnitude² ratio (num²/den²) for a single
// biquad section of a given type at probe frequency.  The caller passes
// the section's own freq and Q — for peak/shelf these come straight from
// the user params, for HP/LP they come from the family's pole layout.
void sectionMagSq(ClientEq::FilterType type,
                  double w0sq, double wsq, double w0w,
                  double q, double A,
                  double& num2, double& den2)
{
    using FT = ClientEq::FilterType;
    switch (type) {
    case FT::Peak: {
        const double diff = w0sq - wsq;
        const double tN   = A * w0w / q;
        const double tD   = w0w / (A * q);
        num2 = diff * diff + tN * tN;
        den2 = diff * diff + tD * tD;
        break;
    }
    case FT::LowShelf: {
        const double dN = A * w0sq - wsq;
        const double dD = w0sq - A * wsq;
        const double cross = w0w / q;
        const double crossA = A * cross * cross;
        num2 = A * A * (dN * dN + crossA);
        den2 = dD * dD + crossA;
        break;
    }
    case FT::HighShelf: {
        const double dN = w0sq - A * wsq;
        const double dD = A * w0sq - wsq;
        const double cross = w0w / q;
        const double crossA = A * cross * cross;
        num2 = A * A * (dN * dN + crossA);
        den2 = dD * dD + crossA;
        break;
    }
    case FT::LowPass: {
        const double diff = w0sq - wsq;
        const double cross = w0w / q;
        num2 = w0sq * w0sq;
        den2 = diff * diff + cross * cross;
        break;
    }
    case FT::HighPass: {
        const double diff = w0sq - wsq;
        const double cross = w0w / q;
        num2 = wsq * wsq;
        den2 = diff * diff + cross * cross;
        break;
    }
    }
}

} // namespace

float ClientEq::bandMagnitudeDb(const BandParams& p,
                                float probeFreqHz,
                                double sampleRate) noexcept
{
    return bandMagnitudeDb(p, probeFreqHz, sampleRate,
                           FilterFamily::Butterworth);
}

float ClientEq::bandMagnitudeDb(const BandParams& p,
                                float probeFreqHz,
                                double sampleRate,
                                FilterFamily family) noexcept
{
    if (!p.enabled || probeFreqHz <= 0.0f) return 0.0f;
    (void)sampleRate;

    // For peak/shelf: single-section math with user Q and gain.
    // For HP/LP: cascade of (slope / 12) sections, each with its own Q
    // and freq scale from the active family's pole layout.
    const double f0 = std::max(1.0, static_cast<double>(p.freqHz));
    const double userQ = std::max(0.1, static_cast<double>(p.q));
    const double A     = std::pow(10.0,
                                  static_cast<double>(p.gainDb) / 40.0);
    const double w  = static_cast<double>(kTwoPi) *
                      static_cast<double>(probeFreqHz);
    const double wsq = w * w;

    const bool isSlope = (p.type == FilterType::LowPass
                       || p.type == FilterType::HighPass);

    double totalDb = 0.0;
    const int numSections = isSlope ? slopeToSections(p.slopeDbPerOct) : 1;

    // Match computeCoefficients()'s HP/LP behaviour: user Q scales
    // the family's designed section Q as a resonance multiplier
    // relative to the 0.707 default.  Without this the curve drawer
    // would show a static response while the DSP was already
    // resonating from the user's drag.
    constexpr double kDefaultQ = 0.707;
    const double resonanceScale = isSlope ? (userQ / kDefaultQ) : 1.0;

    for (int k = 0; k < numSections; ++k) {
        double sectionFreq = f0;
        double sectionQ    = userQ;
        if (isSlope) {
            const SectionDesign d = designSection(family, k, numSections);
            sectionFreq = f0 * static_cast<double>(d.freqScale);
            sectionQ    = static_cast<double>(d.q) * resonanceScale;
        }
        const double w0   = static_cast<double>(kTwoPi) * sectionFreq;
        const double w0sq = w0 * w0;
        const double w0w  = w0 * w;

        double num2 = 0.0, den2 = 1.0;
        sectionMagSq(p.type, w0sq, wsq, w0w, sectionQ, A, num2, den2);
        if (den2 < 1e-40) continue;
        const double magSq = num2 / den2;
        if (magSq < 1e-24) return -240.0f;
        totalDb += 10.0 * std::log10(magSq);
    }
    return static_cast<float>(totalDb);
}

bool ClientEq::smoothTowardTarget(int idx, Runtime& runtime,
                                  const AtomicBand& target,
                                  float smoothCoeff) noexcept
{
    const float tFreq = target.freqHz.load(std::memory_order_relaxed);
    const float tGain = target.gainDb.load(std::memory_order_relaxed);
    const float tQ    = target.q.load(std::memory_order_relaxed);

    const float newFreq = runtime.current.freqHz + smoothCoeff * (tFreq - runtime.current.freqHz);
    const float newGain = runtime.current.gainDb + smoothCoeff * (tGain - runtime.current.gainDb);
    const float newQ    = runtime.current.q      + smoothCoeff * (tQ    - runtime.current.q);

    const bool changed = (std::fabs(newFreq - runtime.current.freqHz) > kFreqEpsHz)
                      || (std::fabs(newGain - runtime.current.gainDb) > kGainEpsDb)
                      || (std::fabs(newQ    - runtime.current.q     ) > kQEps);

    runtime.current.freqHz = newFreq;
    runtime.current.gainDb = newGain;
    runtime.current.q      = newQ;
    (void)idx;
    return changed;
}

void ClientEq::computeCoefficients(Runtime& runtime) noexcept
{
    // Build up to kMaxSections biquad coefficients for this band.  For
    // peak / shelf the cascade is always 1 section (native 2nd-order
    // topology). For HP / LP the cascade count = slope / 12, with each
    // section's Q taken from the active FilterFamily's pole layout.
    const bool isSlope = (runtime.cachedType == FilterType::LowPass
                       || runtime.cachedType == FilterType::HighPass);
    const int numSections = isSlope
        ? slopeToSections(runtime.cachedSlopeDbPerOct)
        : 1;
    runtime.activeSections = std::clamp(numSections, 1, kMaxSections);

    const float baseFreq = std::clamp(runtime.current.freqHz,
                                      10.0f,
                                      static_cast<float>(m_sampleRate * 0.49f));
    const float userQ = std::max(0.1f, runtime.current.q);
    const float A     = dbToAmp(runtime.current.gainDb);

    auto fillOneSection = [&](int sectionIdx, float sectionFreq, float sectionQ) {
        const float freq = std::clamp(sectionFreq, 10.0f,
                                      static_cast<float>(m_sampleRate * 0.49f));
        const float q    = std::max(0.1f, sectionQ);
        const float omega = kTwoPi * freq / static_cast<float>(m_sampleRate);
        const float cosW  = std::cos(omega);
        const float sinW  = std::sin(omega);
        const float alpha = sinW / (2.0f * q);

        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
        float a0 = 1.0f, a1 = 0.0f, a2 = 0.0f;

        switch (runtime.cachedType) {
        case FilterType::Peak:
            b0 = 1.0f + alpha * A;
            b1 = -2.0f * cosW;
            b2 = 1.0f - alpha * A;
            a0 = 1.0f + alpha / A;
            a1 = -2.0f * cosW;
            a2 = 1.0f - alpha / A;
            break;
        case FilterType::LowShelf: {
            const float sqrtA  = std::sqrt(A);
            const float twoSqrtAalpha = 2.0f * sqrtA * alpha;
            b0 =        A * ((A + 1.0f) - (A - 1.0f) * cosW + twoSqrtAalpha);
            b1 =  2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosW);
            b2 =        A * ((A + 1.0f) - (A - 1.0f) * cosW - twoSqrtAalpha);
            a0 =             (A + 1.0f) + (A - 1.0f) * cosW + twoSqrtAalpha;
            a1 =     -2.0f * ((A - 1.0f) + (A + 1.0f) * cosW);
            a2 =             (A + 1.0f) + (A - 1.0f) * cosW - twoSqrtAalpha;
            break;
        }
        case FilterType::HighShelf: {
            const float sqrtA  = std::sqrt(A);
            const float twoSqrtAalpha = 2.0f * sqrtA * alpha;
            b0 =        A * ((A + 1.0f) + (A - 1.0f) * cosW + twoSqrtAalpha);
            b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosW);
            b2 =        A * ((A + 1.0f) + (A - 1.0f) * cosW - twoSqrtAalpha);
            a0 =             (A + 1.0f) - (A - 1.0f) * cosW + twoSqrtAalpha;
            a1 =      2.0f * ((A - 1.0f) - (A + 1.0f) * cosW);
            a2 =             (A + 1.0f) - (A - 1.0f) * cosW - twoSqrtAalpha;
            break;
        }
        case FilterType::LowPass:
            b0 = (1.0f - cosW) * 0.5f;
            b1 =  1.0f - cosW;
            b2 = (1.0f - cosW) * 0.5f;
            a0 =  1.0f + alpha;
            a1 = -2.0f * cosW;
            a2 =  1.0f - alpha;
            break;
        case FilterType::HighPass:
            b0 =  (1.0f + cosW) * 0.5f;
            b1 = -(1.0f + cosW);
            b2 =  (1.0f + cosW) * 0.5f;
            a0 =  1.0f + alpha;
            a1 = -2.0f * cosW;
            a2 =  1.0f - alpha;
            break;
        }

        const float invA0 = 1.0f / a0;
        Coeff& c = runtime.sections[sectionIdx].coeff;
        c.b0 = b0 * invA0;
        c.b1 = b1 * invA0;
        c.b2 = b2 * invA0;
        c.a1 = a1 * invA0;
        c.a2 = a2 * invA0;
    };

    if (isSlope) {
        // Cascade of N sections.  Per-section Q comes from the active
        // filter family's pole layout (Butterworth/LR/Bessel).  We
        // still want the user's Q control to have an audible effect
        // — use it as a RESONANCE MULTIPLIER relative to the 0.707
        // default so the family's character is preserved when the
        // user leaves Q at the default, while higher Q adds
        // resonance at the corner and lower Q over-damps.
        constexpr float kDefaultQ = 0.707f;
        const float resonanceScale = userQ / kDefaultQ;
        for (int k = 0; k < runtime.activeSections; ++k) {
            const SectionDesign d = designSection(runtime.cachedFamily,
                                                  k, runtime.activeSections);
            fillOneSection(k, baseFreq * d.freqScale, d.q * resonanceScale);
        }
    } else {
        // Peak and shelf: single native 2nd-order section using user Q.
        fillOneSection(0, baseFreq, userQ);
    }
}

void ClientEq::process(float* interleaved, int frames, int channels) noexcept
{
    if (!m_enabled.load(std::memory_order_acquire)) return;
    if (channels != 1 && channels != 2) return;
    if (frames <= 0) return;

    const int activeCount = m_activeBandCount.load(std::memory_order_acquire);
    if (activeCount <= 0) return;

    // Scale the per-sample smoother coefficient up to match this block's
    // duration. effCoeff = 1 - (1 - α)^frames is the closed form of N
    // per-sample one-pole steps applied at once, giving the expected
    // ~15ms time constant regardless of block size.
    const float effCoeff = 1.0f - std::pow(1.0f - m_smoothCoeff,
                                           static_cast<float>(frames));

    // Per-band: check version, smooth parameters, recompute coefficients
    // when anything changes. We force a recompute on version change too,
    // because a filter-type swap won't move the smoothed params at all
    // and would otherwise keep running the old coefficients.
    const FilterFamily currentFamily = static_cast<FilterFamily>(
        m_filterFamily.load(std::memory_order_relaxed));
    for (int i = 0; i < activeCount; ++i) {
        Runtime& rt = m_runtime[i];
        AtomicBand& ab = m_bands[i];

        bool needRecompute = false;
        const uint64_t version = ab.version.load(std::memory_order_acquire);
        if (version != rt.lastVersion) {
            rt.cachedType = static_cast<FilterType>(
                ab.type.load(std::memory_order_relaxed));
            rt.cachedEnabled = ab.enabled.load(std::memory_order_relaxed);
            rt.cachedSlopeDbPerOct = ab.slopeDbPerOct.load(std::memory_order_relaxed);
            rt.cachedFamily = currentFamily;
            rt.lastVersion = version;
            needRecompute = true;
        }

        if (smoothTowardTarget(i, rt, ab, effCoeff)) {
            needRecompute = true;
        }
        if (needRecompute) {
            computeCoefficients(rt);
        }
    }

    // Process — per-sample, cascade all active enabled bands and all of
    // their cascade sections.  Direct Form II Transposed biquad per section:
    //   y[n] = b0*x[n] + z1
    //   z1   = b1*x[n] - a1*y[n] + z2
    //   z2   = b2*x[n] - a2*y[n]
    if (channels == 2) {
        for (int f = 0; f < frames; ++f) {
            float l = interleaved[f * 2];
            float r = interleaved[f * 2 + 1];
            for (int i = 0; i < activeCount; ++i) {
                Runtime& rt = m_runtime[i];
                if (!rt.cachedEnabled) continue;
                for (int s = 0; s < rt.activeSections; ++s) {
                    Section& sec = rt.sections[s];
                    const Coeff& c = sec.coeff;

                    const float yl = c.b0 * l + sec.z1L;
                    sec.z1L = c.b1 * l - c.a1 * yl + sec.z2L;
                    sec.z2L = c.b2 * l - c.a2 * yl;
                    l = yl;

                    const float yr = c.b0 * r + sec.z1R;
                    sec.z1R = c.b1 * r - c.a1 * yr + sec.z2R;
                    sec.z2R = c.b2 * r - c.a2 * yr;
                    r = yr;
                }
            }
            interleaved[f * 2]     = l;
            interleaved[f * 2 + 1] = r;
        }
    } else {
        for (int f = 0; f < frames; ++f) {
            float x = interleaved[f];
            for (int i = 0; i < activeCount; ++i) {
                Runtime& rt = m_runtime[i];
                if (!rt.cachedEnabled) continue;
                for (int s = 0; s < rt.activeSections; ++s) {
                    Section& sec = rt.sections[s];
                    const Coeff& c = sec.coeff;
                    const float y = c.b0 * x + sec.z1L;
                    sec.z1L = c.b1 * x - c.a1 * y + sec.z2L;
                    sec.z2L = c.b2 * x - c.a2 * y;
                    x = y;
                }
            }
            interleaved[f] = x;
        }
    }

    // Master output gain — applied after every band contribution.
    // Skip the multiply when unity to avoid burning cycles on the common
    // fader-at-0-dB path.
    const float gain = m_masterGain.load(std::memory_order_relaxed);
    if (gain != 1.0f) {
        const int totalSamples = frames * channels;
        for (int i = 0; i < totalSamples; ++i) {
            interleaved[i] *= gain;
        }
    }
}

ClientEq::BandParams ClientEq::defaultBand(int idx)
{
    // 10-band Logic-style layout.  Frequencies log-spaced across the
    // voice range so the handles read as an evenly spread row at 0 dB.
    // Butterworth (Q = 0.707, = 1/√2) everywhere — the industry-standard
    // neutral default. Matches Logic Pro's Channel EQ; users can tighten
    // any band post-hoc with Shift+drag or HP/LP vertical drag.
    struct Preset { FilterType type; float freqHz; float q; };
    static constexpr Preset kPresets[kDefaultBandCount] = {
        { FilterType::HighPass,  40.0f,    0.707f },
        { FilterType::LowShelf,  100.0f,   0.707f },
        { FilterType::Peak,      200.0f,   0.707f },
        { FilterType::Peak,      400.0f,   0.707f },
        { FilterType::Peak,      800.0f,   0.707f },
        { FilterType::Peak,      1500.0f,  0.707f },
        { FilterType::Peak,      3000.0f,  0.707f },
        { FilterType::Peak,      5000.0f,  0.707f },
        { FilterType::HighShelf, 8000.0f,  0.707f },
        { FilterType::LowPass,   12000.0f, 0.707f },
    };
    BandParams p;
    if (idx < 0 || idx >= kDefaultBandCount) {
        p.type    = FilterType::Peak;
        p.freqHz  = 1000.0f;
        p.q       = 0.707f;
    } else {
        const auto& preset = kPresets[idx];
        p.type    = preset.type;
        p.freqHz  = preset.freqHz;
        p.q       = preset.q;
    }
    p.gainDb  = 0.0f;
    p.enabled = false;
    return p;
}

} // namespace MasterSDR
