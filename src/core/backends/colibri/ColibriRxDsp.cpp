#include "core/backends/colibri/ColibriRxDsp.h"

#include <QMetaType>

#include <algorithm>
#include <cmath>

namespace AetherSDR::colibri {

ColibriRxDsp::ColibriRxDsp(QObject* parent) : QObject(parent)
{
    // Registered so audioReady/spectrumReady can cross the thread boundary
    // (queued connections), and so control verbs arriving as queued
    // invokeMethod calls do not get dropped for an unregistered Mode argument.
    qRegisterMetaType<std::vector<float>>("std::vector<float>");
    qRegisterMetaType<WdspChannel::Mode>("WdspChannel::Mode");
}

ColibriRxDsp::~ColibriRxDsp() = default;

bool ColibriRxDsp::configure(const Config& config, std::string* error)
{
    // Guard the rate/block inputs before the block-size division below —
    // these can come straight from a RadioConnectRequest params override,
    // where a malformed value decodes to 0 (Principle VII: reject at the
    // boundary rather than divide by zero).
    if (config.inputSampleRateHz <= 0 || config.audioSampleRateHz <= 0
        || config.dspBlockSize <= 0) {
        if (error) {
            *error = "ColibriRxDsp: input/audio sample rate and DSP block size "
                     "must all be positive";
        }
        return false;
    }

    m_config = config;

    WdspChannel::Config wc;
    wc.direction = WdspChannel::Direction::Receive;
    wc.inputBlockSize = static_cast<std::size_t>(config.dspBlockSize);
    // dsp_size describes the same span of time as in_size, but at dsp_rate
    // (WDSP channel.c) — so WDSP consumes exactly one of our input blocks per
    // DSP pass. Guard against the widest rates flooring this to zero.
    std::size_t dspBlock = static_cast<std::size_t>(config.dspBlockSize) *
                           static_cast<std::size_t>(kWdspDspSampleRateHz) /
                           static_cast<std::size_t>(config.inputSampleRateHz);
    if (dspBlock == 0)
        dspBlock = 16;   // 3.072 MHz input: 1024 * 48000 / 3072000 = 16
    wc.dspBlockSize = dspBlock;
    wc.inputSampleRate = config.inputSampleRateHz;
    // The WDSP DSP rate is 48 kHz and is NOT the audio rate — WDSP's RXA
    // stages are built around a 48 kHz internal rate (both reference clients
    // hold it there; see Hl2RxDsp for the receipts).
    wc.dspSampleRate = kWdspDspSampleRateHz;
    wc.outputSampleRate = config.audioSampleRateHz;
    wc.mode = config.mode;
    wc.filterLowHz = config.filterLowHz;
    wc.filterHighHz = config.filterHighHz;
    wc.agcMode = config.agcMode;
    wc.maximumAgcGainDb = config.maximumAgcGainDb;
    wc.blockForOutput = config.blockForOutput;

    auto channel = WdspChannel::create(wc, error);
    if (!channel)
        return false;
    m_channel = std::move(channel);
    m_spectrum = std::make_unique<ColibriSpectrum>(config.fftSize);

    m_iqBuffer.clear();
    m_i.assign(static_cast<std::size_t>(config.dspBlockSize), 0.0f);
    m_q.assign(static_cast<std::size_t>(config.dspBlockSize), 0.0f);
    const std::size_t outN = m_channel->outputBlockSize();
    m_left.assign(outN, 0.0f);
    m_right.assign(outN, 0.0f);
    m_stereo.assign(outN * 2, 0.0f);
    // A rebuild (rate change) creates a fresh channel; restore the operator's
    // current slice offset rather than silently snapping the slice to centre.
    if (m_shiftHz != 0.0)
        m_channel->setShift(m_shiftHz);
    return true;
}

void ColibriRxDsp::setMode(WdspChannel::Mode mode)
{
    m_config.mode = mode;
    if (m_channel)
        m_channel->setMode(mode);
}

void ColibriRxDsp::setFilter(double lowHz, double highHz)
{
    m_config.filterLowHz = lowHz;
    m_config.filterHighHz = highHz;
    if (m_channel)
        m_channel->setFilter(lowHz, highHz);
}

void ColibriRxDsp::setAgc(int agcMode, double maximumGainDb)
{
    m_config.agcMode = agcMode;
    m_config.maximumAgcGainDb = maximumGainDb;
    if (m_channel)
        m_channel->setAgc(agcMode, maximumGainDb);
}

void ColibriRxDsp::setSpectrumRateFps(int fps)
{
    m_spectrumIntervalMs = fps > 0 ? (1000 / fps) : 0;
    // Do NOT reset the clock or the last-emit stamp: a rate change mid-stream
    // takes effect on the next frame that comes due, not an immediate extra one.
}

void ColibriRxDsp::setShift(double shiftHz)
{
    m_shiftHz = shiftHz;
    if (m_channel)
        m_channel->setShift(shiftHz);
}

bool ColibriRxDsp::spectrumFrameDue()
{
    if (m_spectrumIntervalMs <= 0)
        return true;                       // uncapped
    if (!m_spectrumClock.isValid()) {
        m_spectrumClock.start();
        m_lastSpectrumMs = 0;
        return true;                       // paint the first frame immediately
    }
    return (m_spectrumClock.elapsed() - m_lastSpectrumMs) >= m_spectrumIntervalMs;
}

void ColibriRxDsp::processIqBlock(const std::vector<std::complex<float>>& iq)
{
    if (!m_channel)
        return;

    // The two consumers need OPPOSITE handedness (measured on the HL2 — see
    // Hl2RxDsp::processIqBlock for the two facts: the HPSDR wire is the
    // conjugate of the analytic convention, and WDSP's RXA selects the
    // opposite sign to its passband bounds).
    //
    //   wireAnalytic (this library's expected convention):
    //       spectrum <- RAW      (already analytic, fftshift is honest)
    //       demod    <- CONJ     (conjugating an analytic wire reproduces the
    //                             HPSDR wire, which is what WDSP's quirk wants)
    //   !wireAnalytic (HPSDR handedness):
    //       spectrum <- CONJ, demod <- RAW — the HL2 arrangement verbatim.
    //
    // Either way the slice shift stays `slice - NCO` (design doc §IQ
    // handedness), so the flag flips only which buffer feeds which consumer.
    m_conjugated.resize(iq.size());
    for (std::size_t n = 0; n < iq.size(); ++n)
        m_conjugated[n] = std::conj(iq[n]);

    const std::vector<std::complex<float>>& forSpectrum =
        m_config.wireAnalytic ? iq : m_conjugated;
    const std::vector<std::complex<float>>& forDemod =
        m_config.wireAnalytic ? m_conjugated : iq;

    // Panadapter: fed on BOTH paths so a skipped interval advances the window
    // rather than emptying it; the FFT itself is skipped when a frame is not
    // due, which is the whole saving at a wide span (see setSpectrumRateFps).
    if (spectrumFrameDue()) {
        // "Due" STAYS true until a frame actually completes: a callback block
        // can be smaller than the FFT frame, so the boundary can be several
        // blocks away even with a full window behind it.
        if (m_spectrum->process(forSpectrum, m_bins) > 0) {
            emit spectrumReady(m_bins);
            m_lastSpectrumMs = m_spectrumClock.elapsed();
        }
    } else {
        // Keep the window fed without paying for a transform.
        m_spectrum->accumulate(forSpectrum);
    }

    // Audio path: buffer to WdspChannel's fixed block.
    m_iqBuffer.insert(m_iqBuffer.end(), forDemod.begin(), forDemod.end());
    const std::size_t block = static_cast<std::size_t>(m_config.dspBlockSize);
    std::size_t consumed = 0;
    while (m_iqBuffer.size() - consumed >= block) {
        for (std::size_t n = 0; n < block; ++n) {
            m_i[n] = m_iqBuffer[consumed + n].real();
            m_q[n] = m_iqBuffer[consumed + n].imag();
        }
        consumed += block;

        const auto res = m_channel->processIq(m_i, m_q, m_left, m_right);
        if (res != WdspChannel::ProcessResult::Ok)
            continue;   // Underrun while the pipeline fills, etc. — no output yet

        const std::size_t outN = m_left.size();
        for (std::size_t k = 0; k < outN; ++k) {
            m_stereo[2 * k] = m_left[k];
            m_stereo[2 * k + 1] = m_right[k];
        }
        emit audioReady(m_stereo);
        // S-meter from WDSP's own signal-strength meter, NOT from the RMS of
        // the demodulated audio — holding that constant is precisely what the
        // AGC does.
        emit meterUpdate(static_cast<float>(
            m_channel->meter(WdspChannel::Meter::SignalPeak)));
    }

    if (consumed > 0)
        m_iqBuffer.erase(m_iqBuffer.begin(),
                         m_iqBuffer.begin() + static_cast<std::ptrdiff_t>(consumed));
}

}  // namespace AetherSDR::colibri
