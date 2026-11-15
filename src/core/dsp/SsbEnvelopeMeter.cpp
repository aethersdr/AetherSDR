#include "core/dsp/SsbEnvelopeMeter.h"

#include <algorithm>
#include <utility>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace AetherSDR {

namespace {

constexpr double kPi = 3.14159265358979323846;

using Complex = std::complex<double>;

// In-place iterative radix-2 FFT. `inverse` scales by 1/N. Only used to design
// the minimum-phase filter, once per measurement, so clarity beats speed.
void fft(std::vector<Complex>& data, bool inverse)
{
    const std::size_t n = data.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle = (inverse ? 2.0 : -2.0) * kPi / static_cast<double>(len);
        const Complex step(std::cos(angle), std::sin(angle));
        for (std::size_t start = 0; start < n; start += len) {
            Complex w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const Complex a = data[start + k];
                const Complex b = data[start + k + len / 2] * w;
                data[start + k] = a + b;
                data[start + k + len / 2] = a - b;
                w *= step;
            }
        }
    }
    if (inverse) {
        for (auto& v : data) {
            v /= static_cast<double>(n);
        }
    }
}

// Homomorphic (folded real cepstrum) minimum-phase version of `h`, same
// magnitude response, same length. Works for complex taps: log|H| is real but
// not symmetric in frequency, so its cepstrum is complex, and folding the
// anticausal half onto the causal half is still what makes it minimum phase.
std::vector<Complex> toMinimumPhase(const std::vector<Complex>& h)
{
    std::size_t nfft = 1;
    while (nfft < h.size() * 16) {
        nfft <<= 1;
    }

    std::vector<Complex> spectrum(nfft);
    std::copy(h.begin(), h.end(), spectrum.begin());
    fft(spectrum, false);
    // -200 dB floor: the stopband must not send the log to -inf.
    for (auto& v : spectrum) {
        v = Complex(std::log(std::max(std::abs(v), 1e-10)), 0.0);
    }
    fft(spectrum, true); // real cepstrum

    for (std::size_t i = 1; i < nfft / 2; ++i) {
        spectrum[i] *= 2.0;
    }
    for (std::size_t i = nfft / 2 + 1; i < nfft; ++i) {
        spectrum[i] = 0.0;
    }

    fft(spectrum, false);
    for (auto& v : spectrum) {
        v = std::exp(v);
    }
    fft(spectrum, true);

    return {spectrum.begin(), spectrum.begin() + static_cast<std::ptrdiff_t>(h.size())};
}

double toDb(double ratio)
{
    return 20.0 * std::log10(std::max(ratio, 1e-12));
}

// Envelope statistics shared by the audio and IQ entry points.
void summarizeEnvelope(const std::vector<double>& envelope, double sampleRate,
                       const std::vector<std::uint8_t>* activeBlocksIn,
                       std::vector<std::uint8_t>* activeBlocksOut, SsbEnvelopeReport& report)
{
    const std::size_t count = envelope.size();
    report.samplesAnalysed = count;
    double energy = 0.0;
    for (double e : envelope) {
        report.envelopePeak = std::max(report.envelopePeak, e);
        energy += e * e;
    }
    report.envelopeRms = std::sqrt(energy / static_cast<double>(count));

    std::vector<double> sorted(envelope);
    const std::size_t rank = static_cast<std::size_t>(0.999 * static_cast<double>(count - 1));
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(rank),
                     sorted.end());
    report.envelopeP999 = sorted[rank];

    // Speech has pauses, and a whole-signal RMS would score silence as
    // "low average power". Active blocks are the 20 ms blocks within 30 dB of
    // the loudest one.
    const std::size_t block =
        std::max<std::size_t>(1, static_cast<std::size_t>(0.020 * sampleRate));
    std::vector<double> blockEnergy;
    for (std::size_t start = 0; start + block <= count; start += block) {
        double e = 0.0;
        for (std::size_t i = start; i < start + block; ++i) {
            e += envelope[i] * envelope[i];
        }
        blockEnergy.push_back(e);
    }
    if (blockEnergy.empty()) {
        blockEnergy.push_back(energy);
    }
    const double loudest = *std::max_element(blockEnergy.begin(), blockEnergy.end());
    const double floor = loudest * 1e-3; // -30 dB
    double activeEnergy = 0.0;
    std::size_t activeBlocks = 0;
    std::vector<std::uint8_t> mask(blockEnergy.size(), 0);
    for (std::size_t b = 0; b < blockEnergy.size(); ++b) {
        const bool active = activeBlocksIn ? b < activeBlocksIn->size() && (*activeBlocksIn)[b] != 0
                                           : blockEnergy[b] >= floor;
        if (!active) {
            continue;
        }
        mask[b] = 1;
        activeEnergy += blockEnergy[b];
        ++activeBlocks;
    }
    if (activeBlocksOut) {
        *activeBlocksOut = std::move(mask);
    }
    const std::size_t activeSamples = activeBlocks * std::min(block, count);
    report.activeEnvelopeRms = activeSamples
        ? std::sqrt(activeEnergy / static_cast<double>(activeSamples))
        : report.envelopeRms;

    report.paprDb = toDb(report.envelopePeak / std::max(report.envelopeRms, 1e-12));
    report.activePaprDb = toDb(report.envelopePeak / std::max(report.activeEnvelopeRms, 1e-12));
    report.activeP999PaprDb = toDb(report.envelopeP999 / std::max(report.activeEnvelopeRms, 1e-12));
}

} // namespace

std::vector<Complex> designSsbFilter(double sampleRate, SsbPassband passband, int taps,
                                     SsbFilterPhase phase)
{
    const int length = std::max(3, taps | 1);
    const int mid = length / 2;
    const double low = std::min(passband.lowHz, passband.highHz);
    const double high = std::max(passband.lowHz, passband.highHz);
    const double halfWidth = 0.5 * (high - low) / sampleRate; // cycles/sample
    const double center = 0.5 * (high + low) / sampleRate;

    // Blackman-windowed sinc lowpass of half the passband width, normalized
    // to unity DC gain, then shifted to the passband center. One-sided, so a
    // real input loses half its amplitude; the factor 2 puts it back.
    std::vector<double> lowpass(static_cast<std::size_t>(length));
    double sum = 0.0;
    for (int n = 0; n < length; ++n) {
        const double t = n - mid;
        const double sinc =
            t == 0.0 ? 2.0 * halfWidth : std::sin(2.0 * kPi * halfWidth * t) / (kPi * t);
        const double w = 0.42 - 0.5 * std::cos(2.0 * kPi * n / (length - 1)) +
            0.08 * std::cos(4.0 * kPi * n / (length - 1));
        lowpass[static_cast<std::size_t>(n)] = sinc * w;
        sum += sinc * w;
    }

    std::vector<Complex> h(static_cast<std::size_t>(length));
    for (int n = 0; n < length; ++n) {
        const double t = n - mid;
        h[static_cast<std::size_t>(n)] = 2.0 * lowpass[static_cast<std::size_t>(n)] / sum *
            std::polar(1.0, 2.0 * kPi * center * t);
    }
    return phase == SsbFilterPhase::Minimum ? toMinimumPhase(h) : h;
}

SsbEnvelopeReport measureSsbEnvelope(const float* audio, std::size_t frames, double sampleRate,
                                     SsbPassband passband, SsbFilterPhase phase, int taps,
                                     const std::vector<std::uint8_t>* activeBlocksIn,
                                     std::vector<std::uint8_t>* activeBlocksOut)
{
    SsbEnvelopeReport report;
    const auto linear = designSsbFilter(sampleRate, passband, taps, SsbFilterPhase::Linear);
    const std::size_t length = linear.size();
    if (!audio || frames <= length || sampleRate <= 0.0) {
        return report;
    }

    const auto minimum =
        phase == SsbFilterPhase::Minimum ? toMinimumPhase(linear) : std::vector<Complex>{};
    const auto& h = phase == SsbFilterPhase::Minimum ? minimum : linear;
    const std::size_t mid = length / 2;

    // Only output samples whose whole filter span lies inside the input are
    // analysed, so no filter start-up transient reaches the figures.
    const std::size_t count = frames - length + 1;
    std::vector<double> envelope(count);
    double inBandEnergy = 0.0;
    double outOfBandEnergy = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        // y[i] = sum_k h[k] * x[i + length - 1 - k]
        Complex acc(0.0, 0.0);
        Complex accLinear(0.0, 0.0);
        const float* x = audio + i + length - 1;
        for (std::size_t k = 0; k < length; ++k) {
            const double sample = x[-static_cast<std::ptrdiff_t>(k)];
            acc += h[k] * sample;
            if (&h != &linear) {
                accLinear += linear[k] * sample;
            }
        }
        if (&h == &linear) {
            accLinear = acc;
        }
        envelope[i] = std::abs(acc);

        // The real part of the linear-phase output is the input band-passed
        // to the same edges with unity gain and a delay of `mid` samples, so
        // what is left over is everything outside the passband.
        const double inBand = accLinear.real();
        const double residual = audio[i + length - 1 - mid] - inBand;
        inBandEnergy += inBand * inBand;
        outOfBandEnergy += residual * residual;
    }

    for (std::size_t i = length - 1; i < frames; ++i) {
        report.audioPeak = std::max(report.audioPeak, static_cast<double>(std::fabs(audio[i])));
    }

    summarizeEnvelope(envelope, sampleRate, activeBlocksIn, activeBlocksOut, report);
    report.envelopeOverAudioPeakDb = toDb(report.envelopePeak / std::max(report.audioPeak, 1e-12));
    report.outOfBandDbc =
        10.0 * std::log10(std::max(outOfBandEnergy, 1e-30) / std::max(inBandEnergy, 1e-30));
    return report;
}

SsbEnvelopeReport measureIqEnvelope(const std::complex<float>* iq, std::size_t count,
                                    double sampleRate,
                                    const std::vector<std::uint8_t>* activeBlocksIn,
                                    std::vector<std::uint8_t>* activeBlocksOut)
{
    SsbEnvelopeReport report;
    if (!iq || count == 0 || sampleRate <= 0.0) {
        return report;
    }
    std::vector<double> envelope(count);
    for (std::size_t i = 0; i < count; ++i) {
        envelope[i] = std::abs(std::complex<double>(iq[i]));
    }
    summarizeEnvelope(envelope, sampleRate, activeBlocksIn, activeBlocksOut, report);
    return report;
}

} // namespace AetherSDR
