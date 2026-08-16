#pragma once

#include <QByteArray>
#include <memory>
#include <vector>
#include <cstdint>

namespace r8b { class CDSPResampler24; }

namespace AetherSDR {

// High-quality sample rate converter using r8brain-free-src (MIT).
// Wraps r8b::CDSPResampler24 with float32 ↔ double conversion.
//
// Each instance handles one fixed rate ratio. Create separate instances
// for upsample and downsample paths.
//
// Thread safety: NOT thread-safe. Use one instance per thread/path.

class Resampler {
public:
    // maxBlockSamples: max mono samples per process() call
    // reqTransBand: transition band in percent of Nyquist (default 2.0 = ~950 taps; 45.0 = ~42 taps)
    Resampler(double srcRate, double dstRate, int maxBlockSamples = 4096, double reqTransBand = 2.0);
    ~Resampler();

    // Resample mono float32 PCM. Returns resampled mono float32.
    QByteArray process(const float* in, int numSamples);

    // Reuse caller-owned storage. The returned count is mono float samples.
    // Reserving output before the first call keeps steady-state conversion
    // allocation-free for blocks no larger than maxBlockSamples.
    int process(const float* in, int numSamples, QByteArray& output);

    // Convenience: stereo float32 → mono downsample → resampled mono float32
    QByteArray processStereoToMono(const float* stereoIn, int numStereoFrames);

    // Convenience: mono float32 → resampled → duplicated to stereo float32
    QByteArray processMonoToStereo(const float* monoIn, int numSamples);

    // Convenience: stereo float32 → downmix to mono → resample → duplicate to stereo float32
    QByteArray processStereoToStereo(const float* stereoIn, int numStereoFrames);

    // Discard streaming state and consume startup latency again. Call between
    // independent streams, never from the realtime process callback.
    void reset();

    double srcRate() const { return m_srcRate; }
    double dstRate() const { return m_dstRate; }

    // Linear-phase signal delay expressed in source-rate samples. prewarm()
    // consumes r8brain's no-output startup interval, but it does not remove
    // this acoustic delay from the converted waveform.
    int groupDelayInputFrames() const noexcept { return m_groupDelayInputFrames; }

private:
    void prewarm();

    double m_srcRate;
    double m_dstRate;
    int    m_maxBlockSamples;
    std::unique_ptr<r8b::CDSPResampler24> m_resampler;
    int m_groupDelayInputFrames{0};
    std::vector<double> m_inBuf;   // float32 → double conversion buffer
};

} // namespace AetherSDR
