#pragma once

// Measures what an SSB transmitter's RF envelope would do with a block of
// transmit audio, offline, without keying anything (RFC #4836, Stage 0).
//
// The client hands the radio real audio; the radio filters it into one
// sideband and the RF envelope is the magnitude of that complex signal. The
// envelope peak is not the audio sample peak: band-limited clipped audio can
// have an envelope several dB above its highest sample, which is why a
// sample-peak limiter in the client cannot cap PEP. This meter models the
// radio's sideband filter (linear or minimum phase) and reports the envelope
// figures the transmit-dynamics stages are judged by.

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace AetherSDR {

struct SsbPassband {
    // Signed audio edges in hertz. Positive edges select the upper sideband,
    // negative edges the lower one ({-2700, -300} is LSB).
    double lowHz{300.0};
    double highHz{2700.0};
};

enum class SsbFilterPhase {
    Linear,  // symmetric FIR, constant group delay
    Minimum, // same magnitude response, energy at the front of the filter
};

struct SsbEnvelopeReport {
    std::size_t samplesAnalysed{0};
    double audioPeak{0.0};               // highest |audio sample| (linear, 1.0 = full scale)
    double envelopePeak{0.0};            // highest envelope sample
    double envelopeP999{0.0};            // 99.9th percentile of the envelope
    double envelopeRms{0.0};             // over every analysed sample
    double activeEnvelopeRms{0.0};       // over 20 ms blocks within 30 dB of the loudest
    double paprDb{0.0};                  // envelopePeak / envelopeRms
    double activePaprDb{0.0};            // envelopePeak / activeEnvelopeRms
    double activeP999PaprDb{0.0};        // envelopeP999 / activeEnvelopeRms
    double envelopeOverAudioPeakDb{0.0}; // envelopePeak / audioPeak
    double outOfBandDbc{0.0};            // audio energy outside the passband vs inside
};

// Complex FIR taps for one sideband of `passband` at `sampleRate`. Gain is 2
// in the passband, so a real tone of amplitude A has an envelope of A.
// `taps` is forced odd.
std::vector<std::complex<double>> designSsbFilter(double sampleRate, SsbPassband passband, int taps,
                                                  SsbFilterPhase phase);

SsbEnvelopeReport measureSsbEnvelope(const float* audio, std::size_t frames, double sampleRate,
                                     SsbPassband passband, SsbFilterPhase phase, int taps = 1023,
                                     const std::vector<std::uint8_t>* activeBlocksIn = nullptr,
                                     std::vector<std::uint8_t>* activeBlocksOut = nullptr);

// The same envelope figures for IQ a modulator has already produced (the HL2
// modulator's output, for one): the envelope is |iq|, no filter is modeled,
// and audioPeak, envelopeOverAudioPeakDb and outOfBandDbc stay 0.
SsbEnvelopeReport measureIqEnvelope(const std::complex<float>* iq, std::size_t count,
                                    double sampleRate,
                                    const std::vector<std::uint8_t>* activeBlocksIn = nullptr,
                                    std::vector<std::uint8_t>* activeBlocksOut = nullptr);

// The active-speech mask is one byte per 20 ms block of analysed output.
// `activeBlocksOut` receives the mask a measurement chose; `activeBlocksIn`
// scores the active RMS over a mask chosen elsewhere. Comparing a processed
// run against a bypassed one must use the bypassed run's mask: compression
// lifts breaths and room tone over a per-run gate, and those quiet blocks
// would then drag the processed RMS down and hide the gain being measured.

} // namespace AetherSDR
