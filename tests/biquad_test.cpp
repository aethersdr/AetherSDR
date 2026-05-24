// Standalone test harness for Biquad / StereoBiquad.
// CMake target `biquad_test`.  Exit 0 = pass.
//
// Acceptance per #3042:
//   - Coefficient computation matches Audio EQ Cookbook reference values
//     for each filter type (spot-checks at fc with Q=0.707).
//   - reset() zeros state without touching coefficients (verify by
//     running a signal, resetting, running again — outputs must match).
//   - Block process() is bit-identical to N successive per-sample
//     process(x) calls (pins DF-II-T behaviour for future SIMD work).
//   - Each Biquad::Type produces a frequency response with the
//     expected shape at probe points (high attenuation in stopband,
//     near-0 dB in passband, peak at boost frequency, etc.).
//   - StereoBiquad is bit-identical to two independent Biquads with
//     the same coefficients fed the same input.

#include "core/Biquad.h"
#include "core/StereoBiquad.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using AetherSDR::Biquad;
using AetherSDR::StereoBiquad;

namespace {

constexpr double kSampleRate = 48000.0;

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-60s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail.c_str());
    if (!ok) ++g_failed;
}

bool approxEq(double a, double b, double tol)
{
    return std::fabs(a - b) <= tol;
}

// Reference cookbook coefficients computed directly from the
// Audio EQ Cookbook formulas at fs=48000, fc=1000, Q=0.707, gain=6 dB
// for the gain-bearing types.  Pre-normalised by a0 — these are the
// exact bit-for-bit values setCoefficients() must produce when given
// the same inputs.  Using double precision math for the reference so
// the comparison tolerance is tight.
struct ExpectedCoeffs { double b0, b1, b2, a1, a2; };

ExpectedCoeffs cookbookReference(Biquad::Type type, double fs, double fc,
                                 double Q, double gainDb)
{
    constexpr double kPi = 3.14159265358979323846;
    const double w0    = 2.0 * kPi * fc / fs;
    const double cosw  = std::cos(w0);
    const double sinw  = std::sin(w0);
    const double alpha = sinw / (2.0 * Q);
    const double A     = std::pow(10.0, gainDb / 40.0);
    double b0=1, b1=0, b2=0, a0=1, a1=0, a2=0;
    switch (type) {
    case Biquad::Type::LowPass:
        b0 = (1 - cosw) * 0.5; b1 = 1 - cosw; b2 = (1 - cosw) * 0.5;
        a0 = 1 + alpha; a1 = -2 * cosw; a2 = 1 - alpha;
        break;
    case Biquad::Type::HighPass:
        b0 = (1 + cosw) * 0.5; b1 = -(1 + cosw); b2 = (1 + cosw) * 0.5;
        a0 = 1 + alpha; a1 = -2 * cosw; a2 = 1 - alpha;
        break;
    case Biquad::Type::BandPassConstQ:
        b0 = Q * alpha; b1 = 0; b2 = -Q * alpha;
        a0 = 1 + alpha; a1 = -2 * cosw; a2 = 1 - alpha;
        break;
    case Biquad::Type::BandPassPeak:
        b0 = alpha; b1 = 0; b2 = -alpha;
        a0 = 1 + alpha; a1 = -2 * cosw; a2 = 1 - alpha;
        break;
    case Biquad::Type::Notch:
        b0 = 1; b1 = -2 * cosw; b2 = 1;
        a0 = 1 + alpha; a1 = -2 * cosw; a2 = 1 - alpha;
        break;
    case Biquad::Type::AllPass:
        b0 = 1 - alpha; b1 = -2 * cosw; b2 = 1 + alpha;
        a0 = 1 + alpha; a1 = -2 * cosw; a2 = 1 - alpha;
        break;
    case Biquad::Type::PeakingEq:
        b0 = 1 + alpha * A; b1 = -2 * cosw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cosw; a2 = 1 - alpha / A;
        break;
    case Biquad::Type::LowShelf: {
        const double sa = std::sqrt(A), k = 2 * sa * alpha;
        b0 =       A * ((A+1) - (A-1)*cosw + k);
        b1 =  2 * A * ((A-1) - (A+1)*cosw);
        b2 =       A * ((A+1) - (A-1)*cosw - k);
        a0 =           (A+1) + (A-1)*cosw + k;
        a1 =     -2 * ((A-1) + (A+1)*cosw);
        a2 =           (A+1) + (A-1)*cosw - k;
        break;
    }
    case Biquad::Type::HighShelf: {
        const double sa = std::sqrt(A), k = 2 * sa * alpha;
        b0 =       A * ((A+1) + (A-1)*cosw + k);
        b1 = -2 * A * ((A-1) + (A+1)*cosw);
        b2 =       A * ((A+1) + (A-1)*cosw - k);
        a0 =           (A+1) - (A-1)*cosw + k;
        a1 =      2 * ((A-1) - (A+1)*cosw);
        a2 =           (A+1) - (A-1)*cosw - k;
        break;
    }
    }
    return { b0/a0, b1/a0, b2/a0, a1/a0, a2/a0 };
}

// Driver helper: run a chirp through a Biquad and return its output.
std::vector<float> runImpulse(Biquad& f, int n)
{
    std::vector<float> out(n, 0.0f);
    std::vector<float> in(n, 0.0f);
    in[0] = 1.0f;
    f.process(in.data(), out.data(), n);
    return out;
}

// Helper used by the magnitude-response shape tests.  Sweep a
// sine through the filter for many cycles, then measure peak amplitude
// after warmup; convert to dB.  Coarse but reliable — the cookbook
// response shapes have tens-of-dB separations that survive coarse
// measurement.
double measureMagnitudeDb(Biquad& f, double probeHz, double sr)
{
    constexpr int kFrames = 4800;       // 100 ms at 48 kHz
    constexpr int kWarmup = 2400;
    f.reset();
    constexpr double kPi = 3.14159265358979323846;
    const double w = 2.0 * kPi * probeHz / sr;
    double peak = 0.0;
    for (int i = 0; i < kFrames; ++i) {
        const float x = static_cast<float>(std::sin(w * i));
        const float y = f.process(x);
        if (i >= kWarmup) {
            const double a = std::fabs(static_cast<double>(y));
            if (a > peak) peak = a;
        }
    }
    return 20.0 * std::log10(std::max(peak, 1e-10));
}

// ── Tests ────────────────────────────────────────────────────────────

void testCookbookCoefficients()
{
    struct Case {
        const char* name;
        Biquad::Type type;
        double fc;
        double Q;
        double gainDb;
    };
    const Case cases[] = {
        {"LowPass    fc=1k Q=0.707",    Biquad::Type::LowPass,         1000.0, 0.707, 0.0},
        {"HighPass   fc=300 Q=0.707",   Biquad::Type::HighPass,         300.0, 0.707, 0.0},
        {"BPF constQ fc=2k Q=2",        Biquad::Type::BandPassConstQ,  2000.0, 2.0,   0.0},
        {"BPF peak   fc=5k Q=4",        Biquad::Type::BandPassPeak,    5000.0, 4.0,   0.0},
        {"Notch      fc=1k Q=10",       Biquad::Type::Notch,           1000.0, 10.0,  0.0},
        {"AllPass    fc=1k Q=0.707",    Biquad::Type::AllPass,         1000.0, 0.707, 0.0},
        {"PeakingEq  fc=1k Q=1   +6dB", Biquad::Type::PeakingEq,       1000.0, 1.0,   6.0},
        {"PeakingEq  fc=1k Q=1   -6dB", Biquad::Type::PeakingEq,       1000.0, 1.0,  -6.0},
        {"LowShelf   fc=200 Q=0.707 +6", Biquad::Type::LowShelf,        200.0, 0.707, 6.0},
        {"HighShelf  fc=8k  Q=0.707 -6", Biquad::Type::HighShelf,      8000.0, 0.707,-6.0},
    };
    for (const auto& c : cases) {
        Biquad b;
        b.setCoefficients(c.type, kSampleRate, c.fc, c.Q, c.gainDb);
        const ExpectedCoeffs ref = cookbookReference(c.type, kSampleRate,
                                                    c.fc, c.Q, c.gainDb);
        // Drive an impulse through Biquad and the reference DF-II-T form;
        // bit-identical samples imply bit-identical coefficients within
        // float precision.
        constexpr int kN = 32;
        std::vector<float> ref_out(kN, 0.0f);
        {
            const float b0 = static_cast<float>(ref.b0);
            const float b1 = static_cast<float>(ref.b1);
            const float b2 = static_cast<float>(ref.b2);
            const float a1 = static_cast<float>(ref.a1);
            const float a2 = static_cast<float>(ref.a2);
            float z1 = 0, z2 = 0;
            for (int i = 0; i < kN; ++i) {
                const float x = (i == 0) ? 1.0f : 0.0f;
                const float y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b2 * x - a2 * y;
                ref_out[i] = y;
            }
        }
        std::vector<float> bq_out = runImpulse(b, kN);
        double maxDiff = 0.0;
        for (int i = 0; i < kN; ++i) {
            maxDiff = std::max(maxDiff,
                std::fabs(static_cast<double>(ref_out[i]) - bq_out[i]));
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf), "max |Δ| = %.3e", maxDiff);
        report(c.name, maxDiff < 1e-6, buf);
    }
}

void testResetPreservesCoefficients()
{
    Biquad b;
    b.setCoefficients(Biquad::Type::LowPass, kSampleRate, 1000.0, 0.707);
    // Push some energy through the filter to dirty state.
    std::vector<float> warmup(256, 0.5f);
    std::vector<float> dummy(256, 0.0f);
    b.process(warmup.data(), dummy.data(), 256);
    // Run an impulse → record response.
    std::vector<float> imp1 = runImpulse(b, 64);
    // Dirty state again.
    b.process(warmup.data(), dummy.data(), 256);
    // Reset (should zero state, preserve coefficients) → run impulse.
    b.reset();
    std::vector<float> imp2 = runImpulse(b, 64);
    // The first impulse run was from a non-zero state, so it WON'T
    // match imp2.  We need a clean baseline: a fresh Biquad
    // configured the same way, with no prior data run through it.
    Biquad fresh;
    fresh.setCoefficients(Biquad::Type::LowPass, kSampleRate, 1000.0, 0.707);
    std::vector<float> baseline = runImpulse(fresh, 64);
    double maxDiff = 0.0;
    for (int i = 0; i < 64; ++i) {
        maxDiff = std::max(maxDiff,
            std::fabs(static_cast<double>(baseline[i]) - imp2[i]));
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf),
        "post-reset impulse matches fresh (max |Δ| = %.3e)", maxDiff);
    report("reset() zeros state, preserves coefficients",
           maxDiff == 0.0, buf);
}

void testBlockMatchesPerSample()
{
    // Bit-identical: every Biquad::Type, run a 256-sample noise burst.
    const Biquad::Type all[] = {
        Biquad::Type::LowPass,        Biquad::Type::HighPass,
        Biquad::Type::BandPassConstQ, Biquad::Type::BandPassPeak,
        Biquad::Type::Notch,          Biquad::Type::AllPass,
        Biquad::Type::PeakingEq,      Biquad::Type::LowShelf,
        Biquad::Type::HighShelf,
    };
    constexpr int kN = 256;
    // Deterministic pseudo-random burst.
    std::vector<float> burst(kN);
    uint32_t lfsr = 0xACE1u;
    for (int i = 0; i < kN; ++i) {
        lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xB400u);
        const float r = (static_cast<int32_t>(lfsr) & 0xFFFF) / 65535.0f - 0.5f;
        burst[i] = r;
    }
    for (auto t : all) {
        Biquad blockBiquad, sampleBiquad;
        blockBiquad .setCoefficients(t, kSampleRate, 1000.0, 0.707, 3.0);
        sampleBiquad.setCoefficients(t, kSampleRate, 1000.0, 0.707, 3.0);
        std::vector<float> blockOut(kN, 0.0f), sampleOut(kN, 0.0f);
        blockBiquad.process(burst.data(), blockOut.data(), kN);
        for (int i = 0; i < kN; ++i) sampleOut[i] = sampleBiquad.process(burst[i]);
        bool bitIdentical = true;
        for (int i = 0; i < kN; ++i) {
            if (blockOut[i] != sampleOut[i]) { bitIdentical = false; break; }
        }
        char name[80];
        std::snprintf(name, sizeof(name),
            "block process == per-sample process (type=%d)",
            static_cast<int>(t));
        report(name, bitIdentical);
    }
}

void testFrequencyResponseShape()
{
    // Lowpass: ~0 dB at 100 Hz, < -20 dB at 10 kHz with fc=1kHz Q=0.707.
    {
        Biquad b;
        b.setCoefficients(Biquad::Type::LowPass, kSampleRate, 1000.0, 0.707);
        const double pass = measureMagnitudeDb(b, 100.0,   kSampleRate);
        const double stop = measureMagnitudeDb(b, 10000.0, kSampleRate);
        char detail[96];
        std::snprintf(detail, sizeof(detail),
            "100Hz=%+.2fdB  10kHz=%+.2fdB", pass, stop);
        report("LowPass shape", pass > -1.0 && stop < -20.0, detail);
    }
    // Highpass: < -20 dB at 30 Hz, ~0 dB at 5 kHz with fc=300Hz.
    {
        Biquad b;
        b.setCoefficients(Biquad::Type::HighPass, kSampleRate, 300.0, 0.707);
        const double stop = measureMagnitudeDb(b, 30.0,   kSampleRate);
        const double pass = measureMagnitudeDb(b, 5000.0, kSampleRate);
        char detail[96];
        std::snprintf(detail, sizeof(detail),
            "30Hz=%+.2fdB  5kHz=%+.2fdB", stop, pass);
        report("HighPass shape", stop < -20.0 && pass > -1.0, detail);
    }
    // Notch: deep null at fc, near 0 dB an octave away.
    {
        Biquad b;
        b.setCoefficients(Biquad::Type::Notch, kSampleRate, 1000.0, 10.0);
        const double at  = measureMagnitudeDb(b, 1000.0, kSampleRate);
        const double off = measureMagnitudeDb(b, 200.0,  kSampleRate);
        char detail[96];
        std::snprintf(detail, sizeof(detail),
            "1kHz=%+.2fdB  200Hz=%+.2fdB", at, off);
        report("Notch shape", at < -20.0 && off > -1.0, detail);
    }
    // AllPass: |H| ≈ 1 everywhere.
    {
        Biquad b;
        b.setCoefficients(Biquad::Type::AllPass, kSampleRate, 1000.0, 0.707);
        const double m1 = measureMagnitudeDb(b, 200.0,  kSampleRate);
        const double m2 = measureMagnitudeDb(b, 1000.0, kSampleRate);
        const double m3 = measureMagnitudeDb(b, 8000.0, kSampleRate);
        char detail[96];
        std::snprintf(detail, sizeof(detail),
            "200Hz=%+.2fdB 1kHz=%+.2fdB 8kHz=%+.2fdB", m1, m2, m3);
        report("AllPass shape (flat magnitude)",
               std::fabs(m1) < 0.5 && std::fabs(m2) < 0.5 && std::fabs(m3) < 0.5,
               detail);
    }
    // PeakingEq +6 dB at 1 kHz, Q=2: peak ~+6 dB at fc, ~0 dB far away.
    {
        Biquad b;
        b.setCoefficients(Biquad::Type::PeakingEq, kSampleRate,
                          1000.0, 2.0, 6.0);
        const double peak = measureMagnitudeDb(b, 1000.0, kSampleRate);
        const double far  = measureMagnitudeDb(b,  100.0, kSampleRate);
        char detail[96];
        std::snprintf(detail, sizeof(detail),
            "1kHz=%+.2fdB  100Hz=%+.2fdB", peak, far);
        report("PeakingEq +6 dB at fc",
               approxEq(peak, 6.0, 0.5) && std::fabs(far) < 1.0, detail);
    }
    // LowShelf +6 dB: shelf below corner sits ~+6 dB, above ~0 dB.
    {
        Biquad b;
        b.setCoefficients(Biquad::Type::LowShelf, kSampleRate,
                          500.0, 0.707, 6.0);
        const double low  = measureMagnitudeDb(b,    50.0, kSampleRate);
        const double high = measureMagnitudeDb(b, 10000.0, kSampleRate);
        char detail[96];
        std::snprintf(detail, sizeof(detail),
            "50Hz=%+.2fdB  10kHz=%+.2fdB", low, high);
        report("LowShelf +6 dB",
               approxEq(low, 6.0, 0.5) && std::fabs(high) < 1.0, detail);
    }
    // HighShelf -6 dB: above corner ~-6 dB, below ~0 dB.
    {
        Biquad b;
        b.setCoefficients(Biquad::Type::HighShelf, kSampleRate,
                          5000.0, 0.707, -6.0);
        const double low  = measureMagnitudeDb(b,   100.0, kSampleRate);
        const double high = measureMagnitudeDb(b, 15000.0, kSampleRate);
        char detail[96];
        std::snprintf(detail, sizeof(detail),
            "100Hz=%+.2fdB  15kHz=%+.2fdB", low, high);
        report("HighShelf -6 dB",
               std::fabs(low) < 1.0 && approxEq(high, -6.0, 0.5), detail);
    }
}

void testMagnitudeHelper()
{
    // Static helper agrees with measured magnitude (within 0.5 dB,
    // which is the measurement floor of the coarse sine-sweep above).
    {
        const double mdbStatic = Biquad::magnitudeDbAt(
            Biquad::Type::LowPass, kSampleRate, 1000.0, 0.707, 0.0, 100.0);
        Biquad b;
        b.setCoefficients(Biquad::Type::LowPass, kSampleRate, 1000.0, 0.707);
        const double mdbMeasured = measureMagnitudeDb(b, 100.0, kSampleRate);
        char detail[96];
        std::snprintf(detail, sizeof(detail),
            "static=%+.2fdB measured=%+.2fdB", mdbStatic, mdbMeasured);
        report("magnitudeDbAt (static) matches measured",
               std::fabs(mdbStatic - mdbMeasured) < 0.5, detail);
    }
    // Member helper agrees with static helper exactly (same math).
    {
        Biquad b;
        b.setCoefficients(Biquad::Type::PeakingEq, kSampleRate,
                          1000.0, 2.0, 6.0);
        const double member = b.magnitudeDbAt(1000.0, kSampleRate);
        const double stat   = Biquad::magnitudeDbAt(
            Biquad::Type::PeakingEq, kSampleRate, 1000.0, 2.0, 6.0, 1000.0);
        char detail[96];
        std::snprintf(detail, sizeof(detail),
            "member=%+.4fdB static=%+.4fdB", member, stat);
        report("magnitudeDbAt member == static",
               std::fabs(member - stat) < 1e-9, detail);
    }
}

void testStereoBiquad()
{
    // Bit-identical: StereoBiquad on interleaved L/R produces the
    // same output as two independent Biquads each processing one
    // channel's samples.
    StereoBiquad sb;
    sb.setCoefficients(Biquad::Type::PeakingEq, kSampleRate,
                       1000.0, 1.0, 3.0);
    Biquad lhs, rhs;
    lhs.setCoefficients(Biquad::Type::PeakingEq, kSampleRate, 1000.0, 1.0, 3.0);
    rhs.setCoefficients(Biquad::Type::PeakingEq, kSampleRate, 1000.0, 1.0, 3.0);

    constexpr int kN = 512;
    std::vector<float> stereoBuf(kN * 2);
    std::vector<float> monoL(kN), monoR(kN);
    uint32_t lfsr = 0xBEEFu;
    for (int i = 0; i < kN; ++i) {
        lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xB400u);
        const float l = (static_cast<int32_t>(lfsr) & 0xFFFF) / 65535.0f - 0.5f;
        lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xB400u);
        const float r = (static_cast<int32_t>(lfsr) & 0xFFFF) / 65535.0f - 0.5f;
        stereoBuf[i * 2]     = l;
        stereoBuf[i * 2 + 1] = r;
        monoL[i] = l;
        monoR[i] = r;
    }
    sb.processInterleaved(stereoBuf.data(), kN);
    for (int i = 0; i < kN; ++i) {
        monoL[i] = lhs.process(monoL[i]);
        monoR[i] = rhs.process(monoR[i]);
    }
    bool match = true;
    for (int i = 0; i < kN; ++i) {
        if (stereoBuf[i * 2]     != monoL[i] ||
            stereoBuf[i * 2 + 1] != monoR[i]) {
            match = false;
            break;
        }
    }
    report("StereoBiquad == two independent Biquads (bit-identical)", match);

    // reset() zeros both channels.
    sb.reset();
    StereoBiquad fresh;
    fresh.setCoefficients(Biquad::Type::PeakingEq, kSampleRate,
                          1000.0, 1.0, 3.0);
    std::vector<float> impulse(kN * 2, 0.0f);
    impulse[0] = 1.0f;
    impulse[1] = 1.0f;
    std::vector<float> impulse2 = impulse;
    sb.processInterleaved(impulse.data(), kN);
    fresh.processInterleaved(impulse2.data(), kN);
    bool resetMatches = true;
    for (int i = 0; i < kN * 2; ++i) {
        if (impulse[i] != impulse2[i]) { resetMatches = false; break; }
    }
    report("StereoBiquad::reset() restores fresh-instance behaviour",
           resetMatches);
}

void testBlockProcessMatchesPerSampleSecondCall()
{
    // Sanity: also verify the block helper specifically agrees with
    // calling process(x) one sample at a time, even after the state
    // has been pre-loaded by a prior block call.  Protects against a
    // future SIMD vectorisation that handles the prologue wrong.
    Biquad a, b;
    a.setCoefficients(Biquad::Type::LowPass, kSampleRate, 2000.0, 0.7);
    b.setCoefficients(Biquad::Type::LowPass, kSampleRate, 2000.0, 0.7);

    constexpr int kN = 200;
    std::vector<float> in(kN);
    for (int i = 0; i < kN; ++i) {
        in[i] = static_cast<float>(std::sin(0.1 * i));
    }
    std::vector<float> blockOut(kN, 0.0f);
    a.process(in.data(), blockOut.data(), kN);
    std::vector<float> psOut(kN, 0.0f);
    for (int i = 0; i < kN; ++i) psOut[i] = b.process(in[i]);
    // Now load both with a second batch (state already dirty).
    std::vector<float> in2(kN);
    for (int i = 0; i < kN; ++i) {
        in2[i] = static_cast<float>(std::cos(0.05 * i));
    }
    std::vector<float> blockOut2(kN, 0.0f);
    a.process(in2.data(), blockOut2.data(), kN);
    std::vector<float> psOut2(kN, 0.0f);
    for (int i = 0; i < kN; ++i) psOut2[i] = b.process(in2[i]);
    bool match = true;
    for (int i = 0; i < kN; ++i) {
        if (blockOut[i]  != psOut[i])  { match = false; break; }
        if (blockOut2[i] != psOut2[i]) { match = false; break; }
    }
    report("block process matches per-sample across two batches", match);
}

} // namespace

int main()
{
    testCookbookCoefficients();
    testResetPreservesCoefficients();
    testBlockMatchesPerSample();
    testFrequencyResponseShape();
    testMagnitudeHelper();
    testStereoBiquad();
    testBlockProcessMatchesPerSampleSecondCall();
    std::printf("\n%s — %d failure%s\n",
                g_failed == 0 ? "ALL PASS" : "FAILED",
                g_failed, g_failed == 1 ? "" : "s");
    return g_failed == 0 ? 0 : 1;
}
