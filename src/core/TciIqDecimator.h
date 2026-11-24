#pragma once

#include <complex>
#include <vector>

namespace AetherSDR {

// Brings a backend's host IQ (HL2: the radio's own rate, which is also its
// pan's span) down to the rate a TCI client asked for. Every HL2 rate is
// 48 kHz times a power of two, and so is every TCI rate, so the work is a
// cascade of half-band decimate-by-two stages. Not thread-safe; one instance
// per tapped slice, fed from the thread that delivers that slice's IQ.
class TciIqDecimator final {
public:
    // The rate inputRate reaches by halving toward targetRate without going
    // below it. Never above inputRate: IQ is not invented.
    static int outputRate(int inputRate, int targetRate);

    // Sets the stage count for this pair of rates. A change retires all
    // filter history; the same pair keeps it, so the stream stays continuous.
    void configure(int inputRate, int targetRate);
    int inputRate() const { return m_inputRate; }
    int outputRate() const { return m_outputRate; }

    // Interleaved float32 I,Q pairs in, interleaved pairs out (replaced, not
    // appended). With no stages the input is copied through unchanged.
    void process(const float* iq, int pairs, std::vector<float>& out);

private:
    struct Stage {
        std::vector<std::complex<float>> history;   // last taps-1 inputs
        int phase = 0;                              // next output's offset
    };
    void runStage(Stage& stage, const std::vector<std::complex<float>>& in,
                  std::vector<std::complex<float>>& out) const;

    int m_inputRate = 0;
    int m_outputRate = 0;
    std::vector<Stage> m_stages;
    std::vector<std::complex<float>> m_a;
    std::vector<std::complex<float>> m_b;
};

} // namespace AetherSDR
