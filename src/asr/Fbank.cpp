#include "asr/Fbank.h"

#include <cmath>

namespace AetherSDR {

namespace {
constexpr int kSampleRate = 16000;
constexpr int kFrameLen = 400;   // 25 ms
constexpr int kFrameShift = 160; // 10 ms
constexpr int kFftSize = 512;    // next pow2 >= kFrameLen
constexpr int kFftBins = kFftSize / 2 + 1; // 257
constexpr float kPreemph = 0.97f;
constexpr float kLowFreq = 20.0f;
constexpr float kHighFreq = 8000.0f; // Nyquist
constexpr float kLogFloor = 1.1920929e-7f;
constexpr double kPi = 3.14159265358979323846;

float hzToMel(float hz) { return 1127.0f * std::log(1.0f + hz / 700.0f); }
} // namespace

Fbank::Fbank()
{
    // Povey window: (0.5 - 0.5 cos(2*pi*n/(N-1)))^0.85
    m_window.resize(kFrameLen);
    for (int n = 0; n < kFrameLen; ++n) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * n / (kFrameLen - 1));
        m_window[n] = static_cast<float>(std::pow(w, 0.85));
    }

    // Triangular mel filters, evenly spaced in mel between low/high.
    const float melLow = hzToMel(kLowFreq);
    const float melHigh = hzToMel(kHighFreq);
    const float melDelta = (melHigh - melLow) / (kNumBins + 1);
    m_melWeights.assign(kNumBins, std::vector<float>(kFftBins, 0.0f));
    for (int m = 0; m < kNumBins; ++m) {
        const float center = melLow + (m + 1) * melDelta;
        const float left = center - melDelta;
        const float right = center + melDelta;
        for (int k = 0; k < kFftBins; ++k) {
            const float mel = hzToMel(static_cast<float>(k) * kSampleRate / kFftSize);
            float w = 0.0f;
            if (mel > left && mel <= center) {
                w = (mel - left) / melDelta;
            } else if (mel > center && mel < right) {
                w = (right - mel) / melDelta;
            }
            m_melWeights[m][k] = w;
        }
    }
}

// In-place iterative radix-2 Cooley-Tukey FFT, size 512.
void Fbank::fft512(float* re, float* im) const
{
    constexpr int n = kFftSize;
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / len;
        const float wr = static_cast<float>(std::cos(ang));
        const float wi = static_cast<float>(std::sin(ang));
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; ++k) {
                const int a = i + k;
                const int b = i + k + len / 2;
                const float xr = re[b] * cr - im[b] * ci;
                const float xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
                const float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

std::vector<float> Fbank::compute(const float* samples, int count, int* frames) const
{
    const int numFrames = count >= kFrameLen ? 1 + (count - kFrameLen) / kFrameShift : 0;
    if (frames != nullptr) {
        *frames = numFrames;
    }
    if (numFrames <= 0) {
        return {};
    }

    std::vector<float> feats(static_cast<size_t>(numFrames) * kNumBins, 0.0f);
    std::vector<float> re(kFftSize), im(kFftSize), frame(kFrameLen);

    for (int t = 0; t < numFrames; ++t) {
        const float* src = samples + t * kFrameShift;
        // Kaldi wave scale: samples are in [-1,1]; kaldi works on int16 range, so
        // scale by 32768 to match its energy levels (WeSpeaker was trained thus).
        double mean = 0.0;
        for (int n = 0; n < kFrameLen; ++n) {
            frame[n] = src[n] * 32768.0f;
            mean += frame[n];
        }
        mean /= kFrameLen;
        for (int n = 0; n < kFrameLen; ++n) {
            frame[n] -= static_cast<float>(mean); // remove DC
        }
        // Pre-emphasis (x[-1] = x[0]), then window.
        for (int n = kFrameLen - 1; n > 0; --n) {
            frame[n] -= kPreemph * frame[n - 1];
        }
        frame[0] -= kPreemph * frame[0];
        for (int n = 0; n < kFrameLen; ++n) {
            re[n] = frame[n] * m_window[n];
        }
        for (int n = kFrameLen; n < kFftSize; ++n) {
            re[n] = 0.0f;
        }
        std::fill(im.begin(), im.end(), 0.0f);

        fft512(re.data(), im.data());

        float* out = &feats[static_cast<size_t>(t) * kNumBins];
        for (int m = 0; m < kNumBins; ++m) {
            const std::vector<float>& w = m_melWeights[m];
            float energy = 0.0f;
            for (int k = 0; k < kFftBins; ++k) {
                if (w[k] != 0.0f) {
                    energy += w[k] * (re[k] * re[k] + im[k] * im[k]); // power spectrum
                }
            }
            out[m] = std::log(energy > kLogFloor ? energy : kLogFloor);
        }
    }

    // Cepstral mean normalization: subtract each bin's mean over time.
    for (int m = 0; m < kNumBins; ++m) {
        double sum = 0.0;
        for (int t = 0; t < numFrames; ++t) {
            sum += feats[static_cast<size_t>(t) * kNumBins + m];
        }
        const float mean = static_cast<float>(sum / numFrames);
        for (int t = 0; t < numFrames; ++t) {
            feats[static_cast<size_t>(t) * kNumBins + m] -= mean;
        }
    }
    return feats;
}

} // namespace AetherSDR
