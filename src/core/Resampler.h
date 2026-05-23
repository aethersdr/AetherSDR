#pragma once

#include <QByteArray>
#include <memory>
#include <vector>
#include <cstdint>

namespace r8b { class CDSPResampler24; }

namespace MasterSDR {

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
    Resampler(double srcRate, double dstRate, int maxBlockSamples = 4096);
    ~Resampler();

    // Resample mono float32 PCM. Returns resampled mono float32.
    QByteArray process(const float* in, int numSamples);

    // Convenience: stereo float32 → mono downsample → resampled mono float32
    QByteArray processStereoToMono(const float* stereoIn, int numStereoFrames);

    // Convenience: mono float32 → resampled → duplicated to stereo float32
    QByteArray processMonoToStereo(const float* monoIn, int numSamples);

    // Convenience: stereo float32 → downmix to mono → resample → duplicate to stereo float32
    QByteArray processStereoToStereo(const float* stereoIn, int numStereoFrames);

    double srcRate() const { return m_srcRate; }
    double dstRate() const { return m_dstRate; }

private:
    double m_srcRate;
    double m_dstRate;
    int    m_maxBlockSamples;
    std::unique_ptr<r8b::CDSPResampler24> m_resampler;
    std::vector<double> m_inBuf;   // float32 → double conversion buffer
};

} // namespace MasterSDR
