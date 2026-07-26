#include "core/backends/hl2/Hl2RxDsp.h"

#include <QMetaType>

#include <algorithm>
#include <cmath>

namespace AetherSDR::hl2 {

Hl2RxDsp::Hl2RxDsp(QObject* parent) : QObject(parent)
{
    // Registered so audioReady/spectrumReady can cross a thread boundary once
    // this object is moved onto its own DSP thread (queued connections).
    qRegisterMetaType<std::vector<float>>("std::vector<float>");
    // Control verbs arrive here as queued invokeMethod calls from the GUI
    // thread; without this the Mode argument has no metatype and Qt drops the
    // call with only a warning.
    qRegisterMetaType<WdspChannel::Mode>("WdspChannel::Mode");
}

Hl2RxDsp::~Hl2RxDsp() = default;

bool Hl2RxDsp::configure(const Config& config, std::string* error)
{
    // Guard the rate/block inputs before the block-size division below. These
    // can come straight from a RadioConnectRequest params override, where a
    // missing or malformed "sampleRateHz" decodes to 0 (QVariant::toInt) — an
    // integer divide-by-zero in the dspBlockSize computation. Reject at the
    // boundary rather than crash or build a nonsensical WDSP channel.
    if (config.inputSampleRateHz <= 0 || config.audioSampleRateHz <= 0
        || config.dspBlockSize <= 0) {
        if (error) {
            *error = "Hl2RxDsp: input/audio sample rate and DSP block size "
                     "must all be positive";
        }
        return false;
    }

    m_config = config;

    WdspChannel::Config wc;
    wc.direction = WdspChannel::Direction::Receive;
    wc.inputBlockSize = static_cast<std::size_t>(config.dspBlockSize);
    // dsp_size describes the same span of time as in_size, but at dsp_rate:
    //     dsp_insize = dsp_size * (in_rate / dsp_rate)   [WDSP channel.c]
    // so dsp_size = in_size * dsp_rate / in_rate makes WDSP consume exactly one
    // of our input blocks per DSP pass. At the HL2's 48 kHz default that is
    // 1024; at 192 kHz it is 256.
    wc.dspBlockSize = static_cast<std::size_t>(config.dspBlockSize) *
                      static_cast<std::size_t>(kWdspDspSampleRateHz) /
                      static_cast<std::size_t>(config.inputSampleRateHz);
    wc.inputSampleRate = config.inputSampleRateHz;   // RF/IF rate from the HL2
    // The WDSP DSP rate is 48 kHz and is NOT the audio rate. WDSP's RXA stages
    // are built around a 48 kHz internal rate, and both reference clients hold
    // it there regardless of what goes in or comes out: Thetis passes a literal
    // 48000 for dsp_rate with an independent ch_outrate (cmaster.c
    // create_rcvr), and pihpsdr passes 48000 for dsp_rate with the radio's own
    // sample_rate as input (receiver.c OpenChannel). Setting dsp_rate to the
    // 24 kHz audio rate ran WDSP's chain at half the rate it is designed for.
    wc.dspSampleRate = kWdspDspSampleRateHz;
    wc.outputSampleRate = config.audioSampleRateHz;  // 24 kHz for AudioEngine
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
    m_spectrum = std::make_unique<Hl2Spectrum>(config.fftSize);

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

void Hl2RxDsp::setMode(WdspChannel::Mode mode)
{
    m_config.mode = mode;
    if (m_channel)
        m_channel->setMode(mode);
}

void Hl2RxDsp::setFilter(double lowHz, double highHz)
{
    m_config.filterLowHz = lowHz;
    m_config.filterHighHz = highHz;
    if (m_channel)
        m_channel->setFilter(lowHz, highHz);
}

void Hl2RxDsp::setAgc(int agcMode, double maximumGainDb)
{
    m_config.agcMode = agcMode;
    m_config.maximumAgcGainDb = maximumGainDb;
    if (m_channel)
        m_channel->setAgc(agcMode, maximumGainDb);
}

void Hl2RxDsp::setAudioMuted(bool muted)
{
    m_audioMuted = muted;
}

void Hl2RxDsp::setShift(double shiftHz)
{
    m_shiftHz = shiftHz;
    if (m_channel)
        m_channel->setShift(shiftHz);
}

void Hl2RxDsp::processIqBlock(const std::vector<std::complex<float>>& iq)
{
    if (!m_channel)
        return;

    // Panadapter: the FFT sees the full-rate IQ.
    if (m_spectrum->process(iq, m_bins) > 0)
        emit spectrumReady(m_bins);

    // Audio: buffer into fixed WdspChannel blocks.
    m_iqBuffer.insert(m_iqBuffer.end(), iq.begin(), iq.end());
    const std::size_t block = static_cast<std::size_t>(m_config.dspBlockSize);
    std::size_t consumed = 0;
    while (m_iqBuffer.size() - consumed >= block) {
        if (m_audioMuted) {
            // Clock the audio channel with silence rather than skipping it.
            // Skipping would let the pipeline's contents go stale and emerge on
            // unmute; feeding zeros keeps latency constant and guarantees that
            // what comes out when transmit ends is silence.
            std::fill(m_i.begin(), m_i.end(), 0.0f);
            std::fill(m_q.begin(), m_q.end(), 0.0f);
        } else
        for (std::size_t n = 0; n < block; ++n) {
            m_i[n] = m_iqBuffer[consumed + n].real();
            // Conjugate for WDSP. The HPSDR wire order (I then Q, decoded in
            // MetisProtocol) produces a spectrum whose handedness is the
            // opposite of what WDSP's sideband selection assumes, so USB
            // demodulated the LOWER sideband and LSB the upper — audibly, the
            // two sidebands were swapped while the panadapter looked right.
            //
            // Applied HERE and not in the decoder deliberately: Hl2Spectrum
            // takes the same buffer and its handedness is already correct, so
            // conjugating upstream would fix the audio and mirror the display.
            // The narrow fix is that WDSP's convention differs from the wire's.
            m_q[n] = -m_iqBuffer[consumed + n].imag();
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
        // the demodulated audio. Holding that audio level constant is precisely
        // what the AGC does, so an audio-RMS meter barely moves with signal
        // strength — it deflects, which is why it looked like it worked, but it
        // tracks the AGC's output target rather than the signal.
        emit meterUpdate(static_cast<float>(
            m_channel->meter(WdspChannel::Meter::SignalPeak)));
    }

    if (consumed > 0)
        m_iqBuffer.erase(m_iqBuffer.begin(),
                         m_iqBuffer.begin() + static_cast<std::ptrdiff_t>(consumed));
}

}  // namespace AetherSDR::hl2
