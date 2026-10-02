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
    // Squared once here because computeFrame() works in power: dividing
    // (re^2 + im^2) by this is the same normalisation as dividing the
    // magnitude by m_coherentGain, without the square root in between.
    m_coherentGainSq = m_coherentGain * m_coherentGain;
    // Sized now, never resized later: process() is documented allocation-free
    // and setAverageFrames() must not allocate either, since the alternative
    // is a first-frame malloc on whichever thread happens to change the depth.
    m_avgPower.assign(static_cast<std::size_t>(m_fftSize), 0.0);

    {
        // FFTW's planner is process-global and not thread-safe, and this runs on
        // the beginDspSetup() worker while WdspChannel::open() plans and
        // allocates through FFTW on another thread (#5275). The lock covers the
        // allocations too: TSan names fftw_malloc_plain vs free (firmin.c), not
        // only the planner. execute() stays unguarded, as in processIq().
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

void Hl2Spectrum::setAverageFrames(int frames) noexcept
{
    if (frames < 1) {
        frames = 1;
    }
    // A re-applied identical depth (settings replay) must not clear the
    // accumulator. A zoom rebuilds this object at depth 1, so the depth does not
    // survive a zoom today.
    if (frames == m_averageFrames) {
        return;
    }
    m_averageFrames = frames;
    // An exponential state built at one alpha is not a state at the next one.
    // Assign rather than resize — the vector was sized at construction, so
    // this touches no allocator.
    m_avgPower.assign(static_cast<std::size_t>(m_fftSize), 0.0);
    m_haveAverage = false;
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
    const bool blending = m_averageFrames > 1;
    // alpha = 1/N. Only read when blending, so the division is never by the
    // 1 that means "no averaging".
    const double alpha = blending ? 1.0 / static_cast<double>(m_averageFrames) : 1.0;
    for (int k = 0; k < m_fftSize; ++k) {
        const int src = (k + half) % m_fftSize;                       // fftshift: DC -> centre
        const double re = out[src][0];
        const double im = out[src][1];
        // POWER, not magnitude. The square root this replaces was only ever
        // undone by the logarithm below — 20*log10(mag) IS 10*log10(mag^2) —
        // so for a single frame this is the same number by the same route.
        // What it buys is that anything accumulated across frames is
        // accumulated in the domain where an average means what the operator
        // reads it to mean; see setAverageFrames() in the header for what an
        // average of logarithms actually computes.
        double power = (re * re + im * im) / m_coherentGainSq;
        if (blending) {
            double& state = m_avgPower[static_cast<std::size_t>(k)];
            // Take the first frame whole rather than blending it into a zero
            // state, which would open every average with a ramp from the
            // floor. After that it is an ordinary exponential blend.
            state = m_haveAverage ? state + alpha * (power - state) : power;
            power = state;
        }
        // IQ is normalized to full scale 1.0, so this is dBFS directly. The
        // 1e-24 power floor (a 1e-12 magnitude floor squared) keeps log10 finite;
        // an exactly-zero bin reads -240 dBFS.
        binsDbfs[static_cast<std::size_t>(k)] =
            static_cast<float>(10.0 * std::log10(power + 1e-24));
    }
    // Set after the loop and only while blending, so a pass with averaging off
    // cannot leave a stale "we have a state" behind for the next one to blend
    // into. setAverageFrames() clears it on every depth change.
    if (blending) {
        m_haveAverage = true;
    }
}

}  // namespace AetherSDR::hl2
