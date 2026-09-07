#include "core/backends/anan/AnanRxDsp.h"

#include <QMetaType>

#include <algorithm>
#include <cmath>

namespace AetherSDR::anan {

AnanRxDsp::AnanRxDsp(QObject* parent) : QObject(parent)
{
    // Registered so audioReady/spectrumReady can cross a thread boundary
    // once this object is moved onto its own DSP thread (queued connections).
    qRegisterMetaType<std::vector<float>>("std::vector<float>");
    // Control verbs arrive here as queued invokeMethod calls from the GUI
    // thread; without this the Mode argument has no metatype and Qt drops
    // the call with only a warning.
    qRegisterMetaType<WdspChannel::Mode>("WdspChannel::Mode");
}

AnanRxDsp::~AnanRxDsp() = default;

bool AnanRxDsp::configure(const Config& config, std::string* error)
{
    // Guard the rate/block inputs before the block-size division below.
    // Reject at the boundary rather than crash or build a nonsensical WDSP
    // channel (Principle VII). buildChannel() repeats this same guard so it
    // stays safe called on its own (a rate change never routes through
    // configure() -- see the header comment on both).
    if (config.inputSampleRateHz <= 0 || config.audioSampleRateHz <= 0
        || config.dspBlockSize <= 0) {
        if (error) {
            *error = "AnanRxDsp: input/audio sample rate and DSP block size "
                     "must all be positive";
        }
        return false;
    }

    m_config = config;
    RebuildResult result = buildChannel(config);
    if (!result.channel) {
        if (error)
            *error = result.error;
        return false;
    }
    installChannel(std::move(result));
    return true;
}

AnanRxDsp::RebuildResult AnanRxDsp::buildChannel(const Config& config)
{
    RebuildResult result;

    // Same guard as configure() -- see its comment. Repeated here because
    // this is the entry point a rate change actually calls, on a thread
    // that isn't this object's own.
    if (config.inputSampleRateHz <= 0 || config.audioSampleRateHz <= 0
        || config.dspBlockSize <= 0) {
        result.error = "AnanRxDsp: input/audio sample rate and DSP block size "
                       "must all be positive";
        return result;
    }

    WdspChannel::Config wc;
    wc.direction = WdspChannel::Direction::Receive;
    wc.inputBlockSize = static_cast<std::size_t>(config.dspBlockSize);
    // dsp_size describes the same span of time as in_size, but at dsp_rate:
    //     dsp_insize = dsp_size * (in_rate / dsp_rate)   [WDSP channel.c]
    // so dsp_size = in_size * dsp_rate / in_rate makes WDSP consume exactly
    // one of our input blocks per DSP pass.
    wc.dspBlockSize = static_cast<std::size_t>(config.dspBlockSize) *
                      static_cast<std::size_t>(kWdspDspSampleRateHz) /
                      static_cast<std::size_t>(config.inputSampleRateHz);
    wc.inputSampleRate = config.inputSampleRateHz;
    // The WDSP DSP rate is 48 kHz and is NOT the audio rate -- see
    // Hl2RxDsp::configure()'s comment for the reference-client precedent
    // this matches (Thetis, pihpsdr both hold RXA's internal rate at
    // 48 kHz regardless of the radio's own IQ rate).
    wc.dspSampleRate = kWdspDspSampleRateHz;
    wc.outputSampleRate = config.audioSampleRateHz;
    wc.mode = config.mode;
    wc.filterLowHz = config.filterLowHz;
    wc.filterHighHz = config.filterHighHz;
    wc.agcMode = config.agcMode;
    wc.maximumAgcGainDb = config.maximumAgcGainDb;
    wc.blockForOutput = config.blockForOutput;
    // filterTaps left at WdspChannel::Config's own default (2048): this
    // phase has no manual notch filter, so there is no narrow-notch floor to
    // widen it for (contrast Hl2RxDsp::kRxFilterTaps, which exists solely
    // for that reason).

    auto channel = WdspChannel::create(wc, &result.error);
    if (!channel)
        return result;
    result.outputBlockSize = channel->outputBlockSize();
    result.spectrum = std::make_unique<AnanSpectrum>(config.fftSize);
    result.channel = std::move(channel);
    return result;
}

void AnanRxDsp::beginInitialBuild(const Config& config)
{
    m_config = config;
    m_rebuildInFlight = true;
}

void AnanRxDsp::beginRebuild()
{
    m_rebuildInFlight = true;
}

bool AnanRxDsp::installRebuiltChannel(RebuildResult result)
{
    m_rebuildInFlight = false;
    if (!result.channel)
        return false;
    installChannel(std::move(result));
    return true;
}

void AnanRxDsp::installChannel(RebuildResult result)
{
    m_iqBuffer.clear();
    m_i.assign(static_cast<std::size_t>(m_config.dspBlockSize), 0.0f);
    m_q.assign(static_cast<std::size_t>(m_config.dspBlockSize), 0.0f);
    m_left.assign(result.outputBlockSize, 0.0f);
    m_right.assign(result.outputBlockSize, 0.0f);
    m_stereo.assign(result.outputBlockSize * 2, 0.0f);
    // DC blocker pole for the AUDIO rate -- the blocker runs on
    // WdspChannel's output, not its 48 kHz internal rate. Recomputed here so
    // a rate change keeps the same corner frequency instead of moving it.
    const float pole = dcBlockerPole(kDcBlockerCornerHz,
                                     static_cast<double>(m_config.audioSampleRateHz));
    m_dcBlockL.r = pole;
    m_dcBlockR.r = pole;
    m_dcBlockL.reset();
    m_dcBlockR.reset();
    // Fresh smoothing state for the new channel -- see smoothSpectrumBins()'s
    // own comment. Without this, the first frames after a rate change would
    // blend against the PRIOR rate's noise floor/levels, which can differ
    // enough to show as a brief visible drift instead of a clean start.
    m_smoothedBins.clear();

    // Re-apply m_config's CURRENT values -- for configure() this is exactly
    // what buildChannel() was just given (m_config == config already); for
    // a rate-change swap it may include mode/filter/AGC changes the operator
    // made WHILE the build was in flight, which only ever reached m_config
    // (setMode() et al.'s in-flight gating) and never reached the snapshot
    // buildChannel() actually built from.
    result.channel->setMode(m_config.mode);
    result.channel->setFilter(m_config.filterLowHz, m_config.filterHighHz);
    result.channel->setAgc(m_config.agcMode, m_config.maximumAgcGainDb);
    // A rebuild creates a fresh channel; restore the operator's current
    // slice offset rather than silently snapping the slice to centre.
    if (m_shiftHz != 0.0)
        result.channel->setShift(m_shiftHz);

    m_channel = std::move(result.channel);
    m_spectrum = std::move(result.spectrum);
}

void AnanRxDsp::setMode(WdspChannel::Mode mode)
{
    m_config.mode = mode;
    // See beginRebuild()'s own comment: pushing through mid-build would
    // block this thread on WDSP's process-wide setup mutex for however long
    // the background build has left. m_config still updates, so the value
    // is not lost -- installRebuiltChannel() re-applies it at the swap.
    if (m_channel && !m_rebuildInFlight)
        m_channel->setMode(mode);
}

void AnanRxDsp::setFilter(double lowHz, double highHz)
{
    m_config.filterLowHz = lowHz;
    m_config.filterHighHz = highHz;
    if (m_channel && !m_rebuildInFlight)
        m_channel->setFilter(lowHz, highHz);
}

void AnanRxDsp::setAgc(int agcMode, double maximumGainDb)
{
    m_config.agcMode = agcMode;
    m_config.maximumAgcGainDb = maximumGainDb;
    if (m_channel && !m_rebuildInFlight)
        m_channel->setAgc(agcMode, maximumGainDb);
}

void AnanRxDsp::setAudioMuted(bool muted)
{
    m_audioMuted = muted;
}

void AnanRxDsp::setSpectrumRateFps(int fps)
{
    m_spectrumIntervalMs = fps > 0 ? (1000 / fps) : 0;
    // Do NOT reset the clock or the last-emit stamp -- a rate change
    // mid-stream should take effect on the next frame that comes due, not
    // grant an immediate extra one.
}

void AnanRxDsp::setShift(double shiftHz)
{
    m_shiftHz = shiftHz;
    if (m_channel && !m_rebuildInFlight)
        m_channel->setShift(shiftHz);
}

bool AnanRxDsp::spectrumFrameDue()
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

void AnanRxDsp::smoothSpectrumBins(std::vector<float>& binsDbfs)
{
    if (m_smoothedBins.size() != binsDbfs.size()) {
        // First frame after configure(), or an fftSize change -- nothing to
        // blend against yet.
        m_smoothedBins = binsDbfs;
        return;
    }
    for (std::size_t i = 0; i < binsDbfs.size(); ++i) {
        m_smoothedBins[i] = kSpectrumSmoothAlpha * m_smoothedBins[i]
            + (1.0f - kSpectrumSmoothAlpha) * binsDbfs[i];
    }
    binsDbfs = m_smoothedBins;
}

void AnanRxDsp::processIqBlock(const std::vector<std::complex<float>>& iq)
{
    if (!m_channel)
        return;

    // *** CONFIRMED FOR PROTOCOL 2, 2026-08-21 *** -- was a starting
    // hypothesis; is now a measured fact, both sources HERMES §16 asks for.
    // Read HERMES.md §16 and this class's header comment for the full
    // history if you're touching this.
    //
    // Two facts feed this split (HERMES.md §16.1):
    //   1. WDSP's RXA, as configured here, selects the OPPOSITE sign to its
    //      passband bounds. A property of THIS CODEBASE's WdspChannel
    //      configuration, which Protocol 2 reuses unchanged. Known with full
    //      confidence since before this backend existed.
    //   2. "The HPSDR wire is the conjugate of the analytic convention" --
    //      originally measured against Protocol 1 / the HL2 only, and only a
    //      plausible starting point for Protocol 2's different wire encoding
    //      (typed packets vs C0 register banks). Now independently confirmed
    //      for THIS radio too: `radiocert rx` (2026-08-19, real WWV carrier)
    //      showed the textbook USB/DIGU-recover, LSB/DIGL-don't signature,
    //      and an RSP1B running SDR++ -- sharing zero code with this
    //      backend -- reproduced the identical pattern at the same dial/
    //      offset geometry and confirmed the panadapter draws the carrier on
    //      the correct side. That is the "two-source bar" HERMES §16 sets
    //      before a polarity claim can be trusted; both are in.
    //
    // Fact 1 alone already implies the demodulator and the spectrum must get
    // OPPOSITE handling from each other. Which one gets the conjugate and
    // which gets the raw wire is fact 2's contribution -- demodulator raw,
    // spectrum conjugated, same structure Hl2RxDsp settled on, now confirmed
    // correct here too, not just structurally borrowed
    // (HERMES.md §16.6 rule 2 -- conjugate exactly once, at one place).
    m_conjugated.resize(iq.size());
    for (std::size_t n = 0; n < iq.size(); ++n)
        m_conjugated[n] = std::conj(iq[n]);

    // Panadapter: conjugated. Fed on BOTH paths (this branch and the
    // accumulate() branch below) because the accumulator is shared and both
    // feed the same FFT -- see Hl2RxDsp::processIqBlock's comment for why a
    // raw block during a skipped interval would corrupt part of the next
    // displayed frame.
    if (spectrumFrameDue()) {
        if (m_spectrum->process(m_conjugated, m_bins) > 0) {
            smoothSpectrumBins(m_bins);
            emit spectrumReady(m_bins);
            m_lastSpectrumMs = m_spectrumClock.elapsed();
        }
    } else {
        m_spectrum->accumulate(m_conjugated);
    }

    // Audio: the RAW wire (see the handedness note above).
    m_iqBuffer.insert(m_iqBuffer.end(), iq.begin(), iq.end());
    const std::size_t block = static_cast<std::size_t>(m_config.dspBlockSize);
    std::size_t consumed = 0;
    while (m_iqBuffer.size() - consumed >= block) {
        if (m_audioMuted) {
            std::fill(m_i.begin(), m_i.end(), 0.0f);
            std::fill(m_q.begin(), m_q.end(), 0.0f);
        } else {
            for (std::size_t n = 0; n < block; ++n) {
                m_i[n] = m_iqBuffer[consumed + n].real();
                m_q[n] = m_iqBuffer[consumed + n].imag();
            }
        }
        consumed += block;

        const auto res = m_channel->processIq(m_i, m_q, m_left, m_right);
        if (res != WdspChannel::ProcessResult::Ok)
            continue;   // underrun while the pipeline fills, etc. -- no output yet

        const std::size_t outN = m_left.size();
        for (std::size_t k = 0; k < outN; ++k) {
            m_stereo[2 * k] = m_dcBlockL.process(m_left[k]);
            m_stereo[2 * k + 1] = m_dcBlockR.process(m_right[k]);
        }
        emit audioReady(m_stereo);
        emit meterUpdate(static_cast<float>(
            m_channel->meter(WdspChannel::Meter::SignalPeak)));
    }

    if (consumed > 0)
        m_iqBuffer.erase(m_iqBuffer.begin(),
                         m_iqBuffer.begin() + static_cast<std::ptrdiff_t>(consumed));
}

}  // namespace AetherSDR::anan
