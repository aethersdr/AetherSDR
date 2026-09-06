#include "core/backends/hl2/Hl2Spectrum.h"

#include <fftw3.h>

#include "core/dsp/WdspChannel.h"

#include <cmath>

namespace AetherSDR::hl2 {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

Hl2Spectrum::Hl2Spectrum(int fftSize) : m_fftSize(fftSize < 2 ? 2 : fftSize)
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
    // degenerate window sums to 0 — e.g. a length-2 Hanning, whose two endpoints
    // are both 0 — which would make the magnitude divide produce inf. Unreachable
    // at the production fftSize (1024) but cheap to make safe; with an all-zero
    // window the input is zeroed anyway, so the bins come out 0 rather than inf.
    if (m_coherentGain < 1e-9)
        m_coherentGain = 1.0;

    {
        // FFTW's planner is process-global and NOT thread-safe. Hl2Backend's
        // beginDspSetup() constructs this on its worker while WdspChannel::open()
        // builds a WDSP channel — which plans, allocates and frees through FFTW
        // too — so the two raced with nothing between them. TSan, once Qt and the
        // vendored C were both instrumented (#5275 S4/S6):
        //
        //   Write of size 8 by thread T9:  free <- create_fircore (firmin.c:360)
        //                                       <- OpenChannel <- WdspChannel::open()
        //   Previous write     by thread T4:  memalign <- fftw_malloc_plain
        //                                       <- make_unique<Hl2Spectrum>
        //
        // AnanSpectrum, the sibling of this class, has taken this same lock since
        // it hit the same problem; this one never did.
        //
        // The lock covers the ALLOCATIONS as well as the plan, which is wider
        // than AnanSpectrum's — deliberately. AnanSpectrum's comment records the
        // mallocs as safe unguarded, but the two frames TSan actually names here
        // are fftw_malloc_plain and free, not the planner, so a plan-only lock
        // would leave the reported edge unsynchronised. It costs nothing: this
        // runs once per spectrum construction, never on the audio path.
        // execute() below stays unguarded, same as AnanSpectrum and
        // WdspChannel::processIq().
        //
        // The evidence came from radiomodel_pan_id_mapping_test, which failed
        // this way in 4 of 4 sanitizer runs and has since been removed as
        // intermittent (#5423). The race is not intermittent and is not about
        // that test: two threads reach a non-thread-safe planner on every HL2
        // connect, and nothing tests for it now.
        auto lock = WdspChannel::fftwSetupLock();
        m_in = fftw_malloc(sizeof(fftw_complex) * static_cast<std::size_t>(m_fftSize));
        m_out = fftw_malloc(sizeof(fftw_complex) * static_cast<std::size_t>(m_fftSize));
        m_plan = fftw_plan_dft_1d(m_fftSize, static_cast<fftw_complex*>(m_in),
                                  static_cast<fftw_complex*>(m_out), FFTW_FORWARD, FFTW_ESTIMATE);
    }
}

Hl2Spectrum::~Hl2Spectrum()
{
    // Same lock as the constructor, and over the frees for the same reason: the
    // race TSan reported was a free on one thread against an allocation on
    // another, so guarding only destroy_plan would leave the teardown half of
    // that edge open.
    auto lock = WdspChannel::fftwSetupLock();
    if (m_plan) fftw_destroy_plan(static_cast<fftw_plan>(m_plan));
    if (m_in) fftw_free(m_in);
    if (m_out) fftw_free(m_out);
}

int Hl2Spectrum::process(std::span<const std::complex<float>> iq, std::vector<float>& binsDbfs)
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

void Hl2Spectrum::accumulate(std::span<const std::complex<float>> iq)
{
    m_acc.insert(m_acc.end(), iq.begin(), iq.end());
    // Hold at most fftSize - 1: see the header for why exactly-full would wedge
    // process()'s boundary check.
    const std::size_t keep = static_cast<std::size_t>(m_fftSize) - 1;
    if (m_acc.size() > keep) {
        m_acc.erase(m_acc.begin(),
                    m_acc.end() - static_cast<std::ptrdiff_t>(keep));
    }
}

void Hl2Spectrum::computeFrame(std::vector<float>& binsDbfs)
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

}  // namespace AetherSDR::hl2
