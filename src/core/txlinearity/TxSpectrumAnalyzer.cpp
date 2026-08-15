#include "TxSpectrumAnalyzer.h"

#include <fftw3.h>

#include <algorithm>
#include <cmath>

namespace AetherSDR::txlin {

TxSpectrumAnalyzer::TxSpectrumAnalyzer(int fftSize, WindowKind window)
    : m_fftSize(fftSize > 0 ? fftSize : 0)
{
    if (m_fftSize < 4) {
        m_fftSize = 0;
        return;
    }
    m_window = makeWindow(window, std::size_t(m_fftSize));

    auto* in = fftw_alloc_complex(std::size_t(m_fftSize));
    auto* out = fftw_alloc_complex(std::size_t(m_fftSize));
    m_in = in;
    m_out = out;
    // FFTW_MEASURE would be faster per transform but reorders and overwrites
    // the buffers during planning and costs a noticeable pause; the analyzer
    // runs a handful of 16k transforms per capture, so ESTIMATE is the right
    // trade and matches Hl2Spectrum.
    m_plan = fftw_plan_dft_1d(m_fftSize, in, out, FFTW_FORWARD, FFTW_ESTIMATE);
    m_avg.assign(std::size_t(m_fftSize), 0.0);
}

TxSpectrumAnalyzer::~TxSpectrumAnalyzer()
{
    if (m_plan != nullptr) {
        fftw_destroy_plan(static_cast<fftw_plan>(m_plan));
    }
    if (m_in != nullptr) {
        fftw_free(static_cast<fftw_complex*>(m_in));
    }
    if (m_out != nullptr) {
        fftw_free(static_cast<fftw_complex*>(m_out));
    }
}

void TxSpectrumAnalyzer::reset() noexcept
{
    m_averages = 0;
    std::fill(m_avg.begin(), m_avg.end(), 0.0);
}

bool TxSpectrumAnalyzer::accumulate(std::span<const std::complex<float>> frame)
{
    if (m_fftSize == 0 || m_plan == nullptr) {
        return false;
    }
    if (frame.size() != std::size_t(m_fftSize)) {
        return false;
    }

    auto* in = static_cast<fftw_complex*>(m_in);
    for (int i = 0; i < m_fftSize; ++i) {
        const double w = m_window.taps[std::size_t(i)];
        in[i][0] = double(frame[std::size_t(i)].real()) * w;
        in[i][1] = double(frame[std::size_t(i)].imag()) * w;
    }

    fftw_execute(static_cast<fftw_plan>(m_plan));

    const auto* out = static_cast<const fftw_complex*>(m_out);
    const double norm = (m_window.sumTaps != 0.0) ? (1.0 / m_window.sumTaps) : 1.0;
    const int half = m_fftSize / 2;
    const double n = double(m_averages);
    const double invNext = 1.0 / (n + 1.0);

    // Running mean, not sum-then-divide: an operator can leave the analyzer
    // averaging for a long time and a sum of 16k-bin power spectra would drift
    // in precision long before the count itself became implausible.
    //
    // fftshift folded into the same pass — DC (bin 0) lands at index half, so
    // output index = (k + half) mod fftSize.
    for (int k = 0; k < m_fftSize; ++k) {
        const double re = out[k][0] * norm;
        const double im = out[k][1] * norm;
        const double p = re * re + im * im;
        const int dst = (k + half) % m_fftSize;
        m_avg[std::size_t(dst)] += (p - m_avg[std::size_t(dst)]) * invNext;
    }
    ++m_averages;
    return true;
}

double TxSpectrumAnalyzer::binFreqHz(int index, double sampleRateHz) const
{
    if (m_fftSize == 0) {
        return 0.0;
    }
    return double(index - m_fftSize / 2) * binHz(sampleRateHz, m_fftSize);
}

int TxSpectrumAnalyzer::freqToBin(double freqHz, double sampleRateHz) const
{
    if (m_fftSize == 0) {
        return 0;
    }
    const double bw = binHz(sampleRateHz, m_fftSize);
    if (bw <= 0.0) {
        return 0;
    }
    const int idx = int(std::lround(freqHz / bw)) + m_fftSize / 2;
    return std::clamp(idx, 0, m_fftSize - 1);
}

void TxSpectrumAnalyzer::toDb(std::vector<float>& out, float floorDb) const
{
    out.resize(m_avg.size());
    const double floorLin = std::pow(10.0, double(floorDb) / 10.0);
    for (std::size_t i = 0; i < m_avg.size(); ++i) {
        out[i] = float(10.0 * std::log10(std::max(m_avg[i], floorLin)));
    }
}

InterpolatedPeak interpolatePeak(const std::vector<double>& powerDcCentred,
                                 int loBin, int hiBin,
                                 double sampleRateHz)
{
    InterpolatedPeak peak;
    const int n = int(powerDcCentred.size());
    if (n < 4 || sampleRateHz <= 0.0) {
        return peak;
    }

    // The interpolation needs a neighbour either side, so the search never
    // returns an edge bin. A tone sitting against the capture-band edge is not
    // measurable anyway — the window skirt runs off the end of the spectrum.
    const int lo = std::max(loBin, 1);
    const int hi = std::min(hiBin, n - 2);
    if (lo > hi) {
        return peak;
    }

    int best = lo;
    for (int k = lo; k <= hi; ++k) {
        if (powerDcCentred[std::size_t(k)] > powerDcCentred[std::size_t(best)]) {
            best = k;
        }
    }
    if (powerDcCentred[std::size_t(best)] <= 0.0) {
        return peak;
    }

    const double alpha = 10.0 * std::log10(std::max(powerDcCentred[std::size_t(best - 1)], 1e-300));
    const double beta  = 10.0 * std::log10(std::max(powerDcCentred[std::size_t(best)],     1e-300));
    const double gamma = 10.0 * std::log10(std::max(powerDcCentred[std::size_t(best + 1)], 1e-300));

    // Standard three-point parabolic vertex. The denominator vanishes only for
    // three collinear points (a flat or perfectly linear neighbourhood), which
    // is not a peak at all — fall back to the raw bin rather than dividing by
    // something near zero and reporting a wild offset.
    const double denom = alpha - 2.0 * beta + gamma;
    double delta = 0.0;
    double peakDb = beta;
    if (std::abs(denom) > 1e-12) {
        delta = 0.5 * (alpha - gamma) / denom;
        if (std::abs(delta) <= 1.0) {
            peakDb = beta - 0.25 * (alpha - gamma) * delta;
        } else {
            delta = 0.0;
        }
    }

    const double bw = TxSpectrumAnalyzer::binHz(sampleRateHz, n);
    peak.valid = true;
    peak.deltaBins = delta;
    peak.freqHz = (double(best - n / 2) + delta) * bw;
    peak.powerDb = peakDb;
    return peak;
}

}  // namespace AetherSDR::txlin
