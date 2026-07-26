#include "core/backends/hl2/Hl2TxDsp.h"

#include <algorithm>
#include <cmath>
#include <QDebug>

namespace AetherSDR::hl2 {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Blackman window — ~58 dB sidelobes, which is what sets the achievable
// opposite-sideband suppression.
double blackman(std::size_t n, std::size_t N)
{
    const double x = 2.0 * kPi * static_cast<double>(n) / static_cast<double>(N - 1);
    return 0.42 - 0.5 * std::cos(x) + 0.08 * std::cos(2.0 * x);
}

}  // namespace

Hl2TxDsp::Hl2TxDsp(QObject* parent) : QObject(parent) {}
Hl2TxDsp::~Hl2TxDsp() = default;

// Phasing SSB modulator: a windowed-sinc bandpass and a matching Hilbert
// transformer, sharing one delay line.
//
// WHY NOT WDSP HERE, stated carefully because the obvious reading is wrong:
// WDSP's transmit path WORKS. wdsp_channel_test drives a TXA channel and gets
// IQ out of it, so "WDSP TX is broken" is not the claim.
//
// The claim is narrower. Driven from this backend's configuration, a TXA channel
// returned Underrun on most blocks and zeros on the rest, and closing that gap
// means working out which of a long, largely undocumented initialisation
// sequence it was missing — bandpass run, filter length, phase mode, panel,
// leveler, ALC, CFIR, each with defaults that are not obviously safe. Two
// speculative attempts at that sequence made things worse; one of them broke
// wdsp_channel_test outright, which is how the "WDSP is at fault" reading got
// disproved.
//
// The deciding factor is that the failure mode is SILENT. A transmit chain that
// is subtly misconfigured emits nothing, or emits something wrong, on the air,
// and neither announces itself. A phasing modulator is fifty lines, holds no
// hidden state, and its correctness is a number this file's test measures
// directly: opposite-sideband suppression in dB. Choosing the thing that can be
// measured over the thing that is merely canonical is the right trade for a
// path that keys a transmitter.
//
// Revisiting WDSP TXA later is entirely reasonable — with wdsp_channel_test's
// working configuration as the starting point rather than a guess.
void Hl2TxDsp::designFilters()
{
    const double fs = static_cast<double>(m_config.outputSampleRateHz);
    const double lo = m_config.filterLowHz / fs;      // normalised
    const double hi = m_config.filterHighHz / fs;
    const std::size_t N = kTaps;
    const double mid = static_cast<double>(N - 1) / 2.0;

    m_bandpass.assign(N, 0.0f);
    m_hilbert.assign(N, 0.0f);

    for (std::size_t n = 0; n < N; ++n) {
        const double k = static_cast<double>(n) - mid;
        const double w = blackman(n, N);

        // Bandpass = difference of two lowpass sincs.
        double bp;
        if (k == 0.0) {
            bp = 2.0 * (hi - lo);
        } else {
            bp = (std::sin(2.0 * kPi * hi * k) - std::sin(2.0 * kPi * lo * k))
                 / (kPi * k);
        }
        m_bandpass[n] = static_cast<float>(bp * w);

        // Quadrature filter = the IMAGINARY part of the same analytic bandpass,
        // NOT a wideband Hilbert transformer.
        //
        // This distinction is the whole correctness of the modulator. A textbook
        // Hilbert (2/(pi*k) on odd taps) is all-pass in magnitude: it passes
        // out-of-band audio at FULL amplitude with a 90-degree shift. Pairing it
        // with a band-limited I meant energy above the passband arrived in Q
        // only — which is a REAL signal, so it came out double-sideband on both
        // sides of the carrier. Measured: a 5 kHz tone against a 2700 Hz filter
        // appeared at both +5 kHz and -5 kHz, just 6 dB down, i.e. splatter
        // outside our own passband.
        //
        // Deriving both filters from one analytic prototype,
        //   ha[k] = (exp(j*2*pi*hi*k) - exp(j*2*pi*lo*k)) / (j*2*pi*k),
        // makes I and Q share a passband by construction, and keeps their group
        // delay identical for free.
        double hq;
        if (k == 0.0) {
            hq = 0.0;
        } else {
            hq = (std::cos(2.0 * kPi * lo * k) - std::cos(2.0 * kPi * hi * k))
                 / (kPi * k);
        }
        m_hilbert[n] = static_cast<float>(hq * w);
    }

    m_hist.assign(N, 0.0f);
    m_histPos = 0;
}

bool Hl2TxDsp::configure(const Config& config, std::string* error)
{
    if (config.inputSampleRateHz <= 0 || config.outputSampleRateHz <= 0) {
        if (error) *error = "invalid sample rate";
        return false;
    }
    if (config.outputSampleRateHz % config.inputSampleRateHz != 0) {
        // Zero-stuffing needs an integer ratio, and every rate this backend uses
        // is one. Fail loudly rather than transmit at the wrong pitch.
        if (error) *error = "output rate must be an integer multiple of the input rate";
        return false;
    }
    m_config = config;
    m_upsample = config.outputSampleRateHz / config.inputSampleRateHz;
    designFilters();
    m_inBuffer.clear();
    return true;
}

void Hl2TxDsp::setMode(WdspChannel::Mode mode)
{
    m_config.mode = mode;
}

void Hl2TxDsp::setFilter(double lowHz, double highHz)
{
    m_config.filterLowHz = lowHz;
    m_config.filterHighHz = highHz;
    designFilters();
}

void Hl2TxDsp::setMicGain(double linear)
{
    m_micGain = linear < 0.0 ? 0.0 : linear;
}

double Hl2TxDsp::alcGainDb() const noexcept
{
    return 20.0 * std::log10(std::max(1e-9, m_alcGain));
}

void Hl2TxDsp::reset()
{
    m_alcGain = 1.0;   // a new transmission starts from unity, not mid-ramp
    m_inBuffer.clear();
    std::fill(m_hist.begin(), m_hist.end(), 0.0f);
    m_histPos = 0;
}

bool Hl2TxDsp::isLowerSideband() const
{
    switch (m_config.mode) {
    case WdspChannel::Mode::Lsb:
    case WdspChannel::Mode::Cwl:
    case WdspChannel::Mode::Digl:
        return true;
    default:
        return false;
    }
}

void Hl2TxDsp::processAudioBlock(const std::vector<float>& mono)
{
    if (m_bandpass.empty() || mono.empty())
        return;

    m_inBuffer.insert(m_inBuffer.end(), mono.begin(), mono.end());

    const std::size_t block = static_cast<std::size_t>(m_config.dspBlockSize);
    if (m_inBuffer.size() < block)
        return;

    const std::size_t blocks = m_inBuffer.size() / block;
    const std::size_t consumed = blocks * block;
    const std::size_t N = m_bandpass.size();
    const bool lsb = isLowerSideband();

    m_iq.clear();
    m_iq.reserve(consumed * static_cast<std::size_t>(m_upsample));
    float peak = 0.0f;

    // ---- ALC: bring speech up to something that actually modulates ----
    //
    // Peak-tracking with a fast attack and a slow release, which is the
    // conventional shape: catch the onset of a syllable, but do not pump audibly
    // between words. The gain is capped so a quiet room is not amplified into
    // hiss, and the result is hard-limited below full scale afterwards, because
    // an ALC that can overshoot is a splatter generator.
    if (m_config.alcEnabled) {
        float blockPeak = 0.0f;
        for (std::size_t s = 0; s < consumed; ++s)
            blockPeak = std::max(blockPeak, std::fabs(
                static_cast<float>(m_inBuffer[s] * m_micGain)));

        if (blockPeak > 1e-6f) {
            const double wanted = m_config.alcTargetPeak / blockPeak;
            const double maxGain = std::pow(10.0, m_config.alcMaxGainDb / 20.0);
            const double target = std::min(wanted, maxGain);
            // Per-block time constants. Attack when we need LESS gain (the
            // signal got louder) so overshoot is corrected immediately.
            const double blockSec = static_cast<double>(consumed)
                                  / static_cast<double>(m_config.inputSampleRateHz);
            const double tau = (target < m_alcGain) ? m_config.alcAttackSec
                                                    : m_config.alcReleaseSec;
            const double a = 1.0 - std::exp(-blockSec / std::max(1e-6, tau));
            m_alcGain += a * (target - m_alcGain);
        }
        emit alcGain(static_cast<float>(alcGainDb()));
    } else {
        m_alcGain = 1.0;
    }

    for (std::size_t s = 0; s < consumed; ++s) {
        // Mic peak is measured BEFORE the ALC, deliberately.
        //
        // A post-ALC meter sits pinned near the target by definition and tells
        // the operator nothing — it reports the ALC's success, not their input
        // level. What a mic-gain control acts on is this, and how hard the ALC
        // is working is reported separately as alcGain().
        const float preAlc = static_cast<float>(m_inBuffer[s] * m_micGain);
        peak = std::max(peak, std::fabs(preAlc));

        // Hard limit AFTER the ALC. The ALC is a smoothed estimate and will
        // overshoot on a transient; letting that through would transmit
        // distortion across the band rather than merely clipping our own audio.
        const float in = std::clamp(static_cast<float>(preAlc * m_alcGain),
                                    -1.0f, 1.0f);

        for (int u = 0; u < m_upsample; ++u) {
            // Zero-stuff: only the first sub-sample carries energy. The bandpass
            // below doubles as the anti-imaging filter, and the m_upsample
            // factor restores the amplitude that stuffing divides away.
            const float x = (u == 0) ? in * static_cast<float>(m_upsample) : 0.0f;

            m_hist[m_histPos] = x;

            // One pass over the shared history feeding both filters.
            float bi = 0.0f, bq = 0.0f;
            std::size_t idx = m_histPos;
            for (std::size_t k = 0; k < N; ++k) {
                const float h = m_hist[idx];
                bi += h * m_bandpass[k];
                bq += h * m_hilbert[k];
                idx = (idx == 0) ? N - 1 : idx - 1;
            }
            m_histPos = (m_histPos + 1) % N;

            // bi and bq are the in-phase and quadrature halves of the analytic
            // signal; negating Q mirrors the spectrum, which is the sideband
            // choice.
            const float q = lsb ? -bq : bq;

            // CONJUGATE FOR THE WIRE.
            //
            // The HPSDR wire order has the OPPOSITE handedness to the standard
            // analytic convention — the receive path already compensates for
            // exactly this, conjugating with -imag() before handing IQ to WDSP.
            // Transmit needs the same correction in the same place and did not
            // have it, so every transmission went out on the wrong sideband.
            //
            // Caught on the air, not on the bench: the operator's Yaesu heard
            // our LSB on its USB. It is invisible from inside this application
            // because the panadapter reads the same wire order, so our own
            // display and our own transmission agreed with each other while
            // both disagreed with the rest of the band.
            m_iq.emplace_back(bi, -q);
        }
    }

    m_inBuffer.erase(m_inBuffer.begin(),
                     m_inBuffer.begin() + static_cast<std::ptrdiff_t>(consumed));

    if (!m_iq.empty())
        emit iqReady(m_iq);
    // PRE-modulation level: this is what a mic-gain control acts on, so it is
    // the number that tells an operator whether they are overdriving.
    emit micPeak(peak > 0.0f ? 20.0f * std::log10(peak) : -140.0f);
}

}  // namespace AetherSDR::hl2
