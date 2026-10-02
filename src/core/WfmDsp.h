#pragma once

#include <array>
#include <memory>
#include <vector>

namespace AetherSDR {

class Resampler;

// Pure DSP core of the WFM data demodulator (tests/wfm_dsp_test.cpp),
// replicating SkyRoof's Slicer (VE3NEA):
//   IQ @ native device rate
//     → [1] NCO mix-down   (phase-continuous offset/Doppler correction)
//     → [2] twin linear-phase resamplers → exactly 48 kHz
//     → [3] phase-difference FM discriminator (atan2, amplitude-invariant)
//     → [4] FIR low-pass, 95 taps, fc = 20 kHz, Hamming, linear phase
//     → mono float audio @ 48 kHz
// NCO: the pan stays fixed while Doppler software steps the slice; an offset
// Δf gives a DC term 2·Δf·kGain/fs that clips beyond ≈8 kHz at kGain = 3.
// Mixing down first removes it, and changing only NCO frequency keeps steps
// click-free. Native-rate input: forcing 48 kHz lets the OS mixer resample
// (a 24 k stream has nothing above ±12 kHz); r8brain resamples flat to
// 0.95·Nyquist instead. No de-emphasis, squelch, AGC or EQ: data modems
// (G3RUH 9600 bd) need a flat linear-phase response; the f² noise rise is
// normal. Single-threaded: all calls, including setFreqOffsetHz(), from one
// thread.
class WfmDsp
{
public:
    static constexpr int   kAudioRate  = 48000;  // output rate, always exact
    static constexpr int   kLpCutoffHz = 20000;  // post-demod FIR fc
    static constexpr float kGain       = 3.0f;   // G3RUH ±3 kHz dev → ≈±0.4

    static constexpr int kFirOrder = 94;             // even → odd taps → Type-I
    static constexpr int kFirTaps  = kFirOrder + 1;  // 95

    explicit WfmDsp(int iqRateHz);
    ~WfmDsp();

    int iqRateHz() const { return m_iqRate; }

    // Largest |signal − IQ centre| the chain can absorb: the resampler
    // passband edge (0.95 · min(iqRate, 48 k)/2) minus an 8 kHz guard for
    // the signal's own width (G3RUH Carson bandwidth ≈ ±8 kHz).
    float maxFreqOffsetHz() const;

    // offsetHz = signal frequency − IQ centre frequency. Phase-continuous:
    // only the NCO step changes, the accumulated phase never jumps.
    void setFreqOffsetHz(float offsetHz);

    // Demodulate `frames` interleaved IQ pairs at iqRateHz(). Replaces
    // `audioOut` with mono 48 kHz audio. Output is unclamped, nominal
    // ±2·dev·kGain/48000 — the caller applies volume/limiting.
    void process(const float* iqInterleaved, int frames, std::vector<float>& audioOut);

private:
    const int m_iqRate;

    // [1] NCO (SkyRoof FirstMixer): θ advances by m_ncoStep per input sample
    double m_ncoPhase{0.0};
    double m_ncoStep{0.0};

    // [2] identical twin resamplers keep I and Q phase-matched (same
    // deterministic linear-phase filter, same latency, same output length);
    // null when iqRate == 48 kHz
    std::unique_ptr<Resampler> m_resampleI;
    std::unique_ptr<Resampler> m_resampleQ;
    std::vector<float> m_workI, m_workQ;

    // [3] discriminator state
    float m_prevI{0.0f};
    float m_prevQ{0.0f};

    // [4] FIR delay line (circular)
    std::array<float, kFirTaps> m_firBuf{};
    int m_firIdx{0};
};

} // namespace AetherSDR
