// Socket-free regression test for the two RADE V1 receive phase accumulators
// (#6398). Each is advanced by repeated complex multiplication, so float32
// rounding shrinks its magnitude without bound unless it is renormalized:
//
//   rade_bpf.c  rade_bpf_process()  mixer phase, once per sample
//   rade_rx.c   rade_rx_process()   frequency-offset phase, once per frame
//
// The BPF mixes down and back up with the same phase, so a phase of magnitude
// p scales its output by p^2. Real drift takes hours to matter, so the test
// seeds the accumulator off the unit circle (standing in for those hours) and
// asserts that one call brings it back. Remove either renormalization and the
// matching case below fails.
//
// The RX case injects RADE_STATE_SYNC and an off-unit phase directly into the
// receiver state. It proves the normalization, not acquisition or SNR
// convergence. No radio, socket, device or peer process is involved.
#include "rade_bpf.h"
#include "rade_dsp.h"
#include "rade_rx.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <vector>

namespace {
int failures = 0;
int checks = 0;

void check(bool passed, const char* name)
{
    ++checks;
    if (!passed) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", name);
    }
}

// Same shape as the production receive filter (rade_rx_init): centred on the
// carrier group, 8 kHz, one modem frame per call.
constexpr float kFs = 8000.0f;
constexpr float kCentreHz = 1475.0f;
constexpr float kBandwidthHz = 1740.0f;
constexpr int kBlock = RADE_NMF;
constexpr float kUnitTolerance = 1e-5f;

// A tone on the filter centre, phase-continuous across blocks.
void fillTone(std::vector<RADE_COMP>& block, long firstSample)
{
    for (std::size_t i = 0; i < block.size(); ++i) {
        const double phase = 2.0 * std::numbers::pi * kCentreHz
                             * static_cast<double>(firstSample + static_cast<long>(i)) / kFs;
        block[i] = {static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase))};
    }
}

float rms(const std::vector<RADE_COMP>& block)
{
    double sum = 0.0;
    for (const RADE_COMP& s : block) {
        sum += static_cast<double>(s.real) * s.real + static_cast<double>(s.imag) * s.imag;
    }
    return static_cast<float>(std::sqrt(sum / static_cast<double>(block.size())));
}

// Output level of the block after `warmup` blocks, for a filter whose mixer
// phase starts at `seed`.
float settledLevel(RADE_COMP seed, int warmup, float* phaseMagAfterFirstBlock)
{
    rade_bpf bpf;
    rade_bpf_init(&bpf, RADE_BPF_NTAP, kFs, kBandwidthHz, kCentreHz, kBlock);
    bpf.phase = seed;

    std::vector<RADE_COMP> in(kBlock), out(kBlock);
    long next = 0;
    for (int b = 0; b <= warmup; ++b) {
        fillTone(in, next);
        next += kBlock;
        rade_bpf_process(&bpf, out.data(), in.data(), kBlock);
        if (b == 0 && phaseMagAfterFirstBlock) {
            *phaseMagAfterFirstBlock = rade_cabs(bpf.phase);
        }
    }
    return rms(out);
}

void bpfRenormalizesMixerPhase()
{
    // One sample is enough to expose the missing normalization: the saved
    // phase is whatever the multiply left it, scaled by nothing.
    rade_bpf bpf;
    rade_bpf_init(&bpf, RADE_BPF_NTAP, kFs, kBandwidthHz, kCentreHz, kBlock);
    bpf.phase = {0.5f, 0.0f};
    RADE_COMP in = rade_cone();
    RADE_COMP out{};
    rade_bpf_process(&bpf, &out, &in, 1);
    check(std::fabs(rade_cabs(bpf.phase) - 1.0f) < kUnitTolerance,
          "BPF saves a unit-magnitude mixer phase after a call that started at 0.5");

    // A healthy phase stays on the circle across many calls.
    rade_bpf_reset(&bpf);
    std::vector<RADE_COMP> block(kBlock), filtered(kBlock);
    long next = 0;
    for (int b = 0; b < 50; ++b) {
        fillTone(block, next);
        next += kBlock;
        rade_bpf_process(&bpf, filtered.data(), block.data(), kBlock);
    }
    check(std::fabs(rade_cabs(bpf.phase) - 1.0f) < kUnitTolerance,
          "BPF mixer phase stays unit magnitude over 50 modem frames");

    // Zero-length calls leave a unit phase unit.
    rade_bpf_process(&bpf, filtered.data(), block.data(), 0);
    check(std::fabs(rade_cabs(bpf.phase) - 1.0f) < kUnitTolerance,
          "an empty BPF call leaves the mixer phase unit magnitude");
}

void bpfOutputLevelRecoversFromDrift()
{
    // The symptom, not just the state: the BPF output level. A drifted phase
    // attenuates the passband tone by |phase|^2; the first block after the
    // seed still carries that attenuation, every later block must not.
    float seededMag = 0.0f;
    const float reference = settledLevel(rade_cone(), 2, nullptr);
    const float recovered = settledLevel({0.5f, 0.0f}, 2, &seededMag);

    check(reference > 0.5f, "reference tone passes the filter at close to unity");
    check(std::fabs(seededMag - 1.0f) < kUnitTolerance,
          "phase seeded at 0.5 is unit magnitude after the first block");
    check(std::fabs(recovered / reference - 1.0f) < 0.01f,
          "BPF output level two blocks after a 0.5 seed matches an undrifted filter");
}

// Deterministic pseudo-noise in the receive buffer keeps the timing and
// frequency refinement finite without any real signal.
void fillNoise(std::vector<RADE_COMP>& block)
{
    unsigned state = 12345u;
    auto next = [&state] {
        state = state * 1664525u + 1013904223u;
        return (static_cast<float>(state >> 8) / static_cast<float>(1u << 24) - 0.5f) * 0.2f;
    };
    for (RADE_COMP& s : block) {
        s = {next(), next()};
    }
}

float rxPhaseMagnitudeAfterOneFrame(float seedMagnitude, bool* finite)
{
    // The receiver state is large; keep it off the stack.
    auto* rx = static_cast<rade_rx_state*>(std::calloc(1, sizeof(rade_rx_state)));
    // Production configuration (RADEEngine): bottleneck 3, auxdata on. The
    // BPF is off here so the only phase this call can move is rx_phase.
    if (rade_rx_init(rx, nullptr, 3, 1, 0) != 0) {
        std::free(rx);
        *finite = false;
        return 0.0f;
    }

    rx->verbose = 0;
    rx->state = RADE_STATE_SYNC;
    rx->nin = RADE_NMF;
    rx->tmax = RADE_NMF / 2;   // inside [M, Nmf - M): no timing-slip branch
    rx->fmax = 0.0f;
    rx->rx_phase = {seedMagnitude, 0.0f};

    std::vector<RADE_COMP> in(RADE_NMF + RADE_M);
    fillNoise(in);
    std::vector<float> features(static_cast<std::size_t>(rade_rx_n_features_out(rx)));
    std::vector<float> eoo(static_cast<std::size_t>(rade_rx_n_eoo_bits(rx)));
    rade_rx_process(rx, features.data(), eoo.data(), in.data());

    const float magnitude = rade_cabs(rx->rx_phase);
    *finite = std::isfinite(magnitude);
    std::free(rx);
    return magnitude;
}

void rxRenormalizesFrequencyOffsetPhase()
{
    bool finite = false;
    const float drifted = rxPhaseMagnitudeAfterOneFrame(0.5f, &finite);
    check(finite, "RX processing a drifted phase yields a finite magnitude");
    check(std::fabs(drifted - 1.0f) < kUnitTolerance,
          "RX saves a unit-magnitude frequency-offset phase after a frame that started at 0.5");

    const float healthy = rxPhaseMagnitudeAfterOneFrame(1.0f, &finite);
    check(finite && std::fabs(healthy - 1.0f) < kUnitTolerance,
          "RX leaves a unit-magnitude frequency-offset phase unit magnitude");
}
} // namespace

int main()
{
    bpfRenormalizesMixerPhase();
    bpfOutputLevelRecoversFromDrift();
    rxRenormalizesFrequencyOffsetPhase();
    std::printf("rade_phase_renorm_test: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
