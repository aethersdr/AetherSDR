#include "core/backends/anan/AnanSpectrum.h"

#include "core/dsp/WdspChannel.h"

#include <fftw3.h>

#include <cmath>

namespace AetherSDR::anan {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

AnanSpectrum::AnanSpectrum(int fftSize) : m_fftSize(fftSize < 2 ? 2 : fftSize)
{
    m_acc.reserve(static_cast<std::size_t>(m_fftSize));
    m_window.resize(static_cast<std::size_t>(m_fftSize));
    double sum = 0.0;
    for (int n = 0; n < m_fftSize; ++n) {
        m_window[static_cast<std::size_t>(n)] =
            0.5 * (1.0 - std::cos(2.0 * kPi * n / (m_fftSize - 1)));   // Hanning
        sum += m_window[static_cast<std::size_t>(n)];
    }
    m_coherentGain = sum / 2.0;
    // Guard the per-bin normalization divisor (used in process() below). A
    // degenerate window sums to 0, which would make the magnitude divide
    // produce inf. Unreachable at the production fftSize but cheap to make
    // safe.
    if (m_coherentGain < 1e-9)
        m_coherentGain = 1.0;

    m_in = fftw_malloc(sizeof(fftw_complex) * static_cast<std::size_t>(m_fftSize));
    m_out = fftw_malloc(sizeof(fftw_complex) * static_cast<std::size_t>(m_fftSize));
    {
        // FFTW's planner is process-global (WdspChannel.cpp's own comment on
        // g_setupMutex). AnanRxDsp can now build a NEW WdspChannel on a
        // background thread while THIS spectrum's fftw_execute() keeps
        // running on the I/O thread for the still-active old channel -- this
        // lock is what keeps this constructor from racing that concurrent
        // planner use. Held only around the plan call, not the mallocs above
        // or execute() below (already established as safe unguarded, same as
        // WdspChannel::processIq()'s own contract).
        auto lock = WdspChannel::fftwSetupLock();
        m_plan = fftw_plan_dft_1d(m_fftSize, static_cast<fftw_complex*>(m_in),
                                  static_cast<fftw_complex*>(m_out), FFTW_FORWARD, FFTW_ESTIMATE);
    }
}

AnanSpectrum::~AnanSpectrum()
{
    if (m_plan) {
        auto lock = WdspChannel::fftwSetupLock();
        fftw_destroy_plan(static_cast<fftw_plan>(m_plan));
    }
    if (m_in) fftw_free(m_in);
    if (m_out) fftw_free(m_out);
}

int AnanSpectrum::process(std::span<const std::complex<float>> iq, std::vector<float>& binsDbfs)
{
    int frames = 0;
    for (const auto& s : iq) {
        m_acc.push_back(s);
        if (static_cast<int>(m_acc.size()) == m_fftSize) {
            computeFrame(binsDbfs);
            m_acc.clear();
            ++frames;
        }
    }
    return frames;
}

void AnanSpectrum::accumulate(std::span<const std::complex<float>> iq)
{
    m_acc.insert(m_acc.end(), iq.begin(), iq.end());
    const std::size_t keep = static_cast<std::size_t>(m_fftSize) - 1;
    if (m_acc.size() > keep) {
        m_acc.erase(m_acc.begin(),
                    m_acc.end() - static_cast<std::ptrdiff_t>(keep));
    }
}

void AnanSpectrum::computeFrame(std::vector<float>& binsDbfs)
{
    // Remove the frame's DC offset (the ADC offset lives on I in direct
    // sampling), then apply the window.
    double meanRe = 0.0, meanIm = 0.0;
    for (const auto& s : m_acc) { meanRe += s.real(); meanIm += s.imag(); }
    meanRe /= m_fftSize;
    meanIm /= m_fftSize;

    auto* in = static_cast<fftw_complex*>(m_in);
    for (int n = 0; n < m_fftSize; ++n) {
        const double w = m_window[static_cast<std::size_t>(n)];
        in[n][0] = (static_cast<double>(m_acc[static_cast<std::size_t>(n)].real()) - meanRe) * w;
        in[n][1] = (static_cast<double>(m_acc[static_cast<std::size_t>(n)].imag()) - meanIm) * w;
    }

    fftw_execute(static_cast<fftw_plan>(m_plan));

    const auto* out = static_cast<fftw_complex*>(m_out);
    binsDbfs.resize(static_cast<std::size_t>(m_fftSize));
    const int half = m_fftSize / 2;
    for (int k = 0; k < m_fftSize; ++k) {
        const int src = (k + half) % m_fftSize;                       // fftshift: DC -> centre
        const double re = out[src][0];
        const double im = out[src][1];
        const double mag = std::sqrt(re * re + im * im) / m_coherentGain;
        // IQ is normalized to full scale 1.0, so this is dBFS directly.
        binsDbfs[static_cast<std::size_t>(k)] =
            static_cast<float>(20.0 * std::log10(mag + 1e-12));
    }
}

}  // namespace AetherSDR::anan
