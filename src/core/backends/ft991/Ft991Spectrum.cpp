#include "core/backends/ft991/Ft991Spectrum.h"

#include <fftw3.h>

#include <algorithm>
#include <cmath>

namespace AetherSDR::ft991 {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

Ft991Spectrum::Ft991Spectrum(int sampleRateHz, double spanHz, int fftSize)
    : m_fftSize(fftSize < 16 ? 16 : fftSize)
{
    const int rate = sampleRateHz > 0 ? sampleRateHz : 48000;
    m_binHz = static_cast<double>(rate) / m_fftSize;
    // Never past Nyquist, never zero: a span request the rate cannot cover
    // clamps to what the transform actually produces.
    const int nyquistBins = m_fftSize / 2;
    m_binCount = std::clamp(static_cast<int>(std::lround(spanHz / m_binHz)),
                            1, nyquistBins);

    m_acc.reserve(static_cast<std::size_t>(m_fftSize));
    m_window.resize(static_cast<std::size_t>(m_fftSize));
    double sum = 0.0;
    for (int n = 0; n < m_fftSize; ++n) {
        m_window[static_cast<std::size_t>(n)] =
            0.5 * (1.0 - std::cos(2.0 * kPi * n / (m_fftSize - 1)));   // Hanning
        sum += m_window[static_cast<std::size_t>(n)];
    }
    m_coherentGain = sum / 2.0;
    if (m_coherentGain < 1e-9)
        m_coherentGain = 1.0;

    m_in = fftw_malloc(sizeof(double) * static_cast<std::size_t>(m_fftSize));
    m_out = fftw_malloc(sizeof(fftw_complex)
                        * (static_cast<std::size_t>(m_fftSize) / 2 + 1));
    m_plan = fftw_plan_dft_r2c_1d(m_fftSize, static_cast<double*>(m_in),
                                  static_cast<fftw_complex*>(m_out),
                                  FFTW_ESTIMATE);
}

Ft991Spectrum::~Ft991Spectrum()
{
    if (m_plan) fftw_destroy_plan(static_cast<fftw_plan>(m_plan));
    if (m_in) fftw_free(m_in);
    if (m_out) fftw_free(m_out);
}

int Ft991Spectrum::process(std::span<const float> mono, std::vector<float>& binsDbfs)
{
    int frames = 0;
    for (const float s : mono) {
        m_acc.push_back(s);
        if (static_cast<int>(m_acc.size()) == m_fftSize) {
            computeFrame(binsDbfs);
            m_acc.clear();
            ++frames;
        }
    }
    return frames;
}

void Ft991Spectrum::accumulate(std::span<const float> mono)
{
    m_acc.insert(m_acc.end(), mono.begin(), mono.end());
    // Hold at most fftSize - 1: exactly-full would wedge process()'s
    // boundary check (it fires on == fftSize after a push_back).
    const std::size_t keep = static_cast<std::size_t>(m_fftSize) - 1;
    if (m_acc.size() > keep) {
        m_acc.erase(m_acc.begin(),
                    m_acc.end() - static_cast<std::ptrdiff_t>(keep));
    }
}

void Ft991Spectrum::computeFrame(std::vector<float>& binsDbfs)
{
    // Remove the frame's DC offset (codec/ADC bias), then window.
    double mean = 0.0;
    for (const float s : m_acc)
        mean += s;
    mean /= m_fftSize;

    auto* in = static_cast<double*>(m_in);
    for (int n = 0; n < m_fftSize; ++n) {
        in[n] = (static_cast<double>(m_acc[static_cast<std::size_t>(n)]) - mean)
                * m_window[static_cast<std::size_t>(n)];
    }

    fftw_execute(static_cast<fftw_plan>(m_plan));

    const auto* out = static_cast<fftw_complex*>(m_out);
    binsDbfs.resize(static_cast<std::size_t>(m_binCount));
    for (int k = 0; k < m_binCount; ++k) {
        const double re = out[k][0];
        const double im = out[k][1];
        // A real input splits each tone across the +/- pair the r2c transform
        // folds together; coherentGain already carries the /2, so a full-scale
        // sine reads 0 dBFS here just as it does on the complex siblings.
        const double mag = std::sqrt(re * re + im * im) / m_coherentGain;
        binsDbfs[static_cast<std::size_t>(k)] =
            static_cast<float>(20.0 * std::log10(mag + 1e-12));
    }
}

}  // namespace AetherSDR::ft991
