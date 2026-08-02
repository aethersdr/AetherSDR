#pragma once

#include <QByteArray>
#include <QThreadPool>

#include <array>
#include <memory>
#include <vector>

struct DenoiseState;

namespace AetherSDR {

class Resampler;

// Client-side RNN noise suppression using Mozilla/Xiph RNNoise.
// Processes 24kHz stereo FLOAT32 audio with one matched RNNoise/resampler path
// per RX channel, preserving radio pan, diversity, and binaural phase. The TX
// ProcessedMono mode keeps a single downmixed path and duplicates its output.
//
// RNNoise requires 48kHz mono float input in 480-sample (10ms) frames.

class RNNoiseFilter {
public:
  enum class OutputMode {
    PreserveRxStereo,
    ProcessedMono,
  };

  explicit RNNoiseFilter(OutputMode outputMode = OutputMode::PreserveRxStereo);
  ~RNNoiseFilter();

  // Process a block of 24kHz stereo FLOAT32 PCM (NOT int16
  // — the original comment lied; callers must pass float32 sample
  // pairs interleaved as L,R,L,R,... with each sample in [-1.0, 1.0]).
  // Returns the processed block in the same format and frame count.
  QByteArray process(const QByteArray &pcm24kStereo);

  // Returns true if every state required by the selected output mode exists.
  bool isValid() const;

  // Reset internal state (e.g., on band change).
  void reset();

private:
  QThreadPool m_channelPool;
  std::array<DenoiseState *, 2> m_states{nullptr, nullptr};
  std::array<std::unique_ptr<Resampler>, 2> m_up;
  std::array<std::unique_ptr<Resampler>, 2> m_down;
  std::array<QByteArray, 2> m_inAccum;
  QByteArray m_outAccum;
  std::array<std::vector<float>, 2> m_input24k;
  std::array<std::vector<float>, 2> m_processed48k;
  std::array<std::vector<float>, 2> m_processed48kFloat;
  OutputMode m_outputMode{OutputMode::PreserveRxStereo};
};

} // namespace AetherSDR
