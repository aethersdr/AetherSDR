#include "core/backends/anan/AnanRxDsp.h"

#include <QDebug>
#include <QLoggingCategory>
#include <QMetaType>

#include <algorithm>
#include <cmath>
#include <cstdint>

Q_LOGGING_CATEGORY(lcAnanRxDsp, "aether.anan.rxdsp")

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

AnanRxDsp::~AnanRxDsp()
{
    // Stop the channel first so WdspChannel::close() skips WDSP's 100 ms
    // stop-and-flush timeout (docs/HERMES.md §13 item 9b); nothing feeds it after
    // this, so no drain runs. setRunning() refuses while a processIq() callback is in
    // flight, which cannot happen on this (the feeding) thread -- a false means that
    // assumption broke, so log it.
    if (m_channel && !m_channel->setRunning(false)) {
        qCWarning(lcAnanRxDsp)
            << "could not stop the WDSP channel before destroying it: a "
               "processIq callback was in flight. Teardown will pay WDSP's "
               "100 ms stop-and-flush timeout.";
    }
}

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
    if (!result.channel || !result.analyzer) {
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
    wc.agcFixedGainDb = config.agcFixedGainDb;
    wc.blockForOutput = config.blockForOutput;
    wc.noiseBlanker = config.noiseBlanker;
    wc.noiseBlankerLevel = config.noiseBlankerLevel;
    wc.noiseBlankerFill = config.noiseBlankerFill;
    // filterTaps left at WdspChannel::Config's own default (2048): this
    // phase has no manual notch filter, so there is no narrow-notch floor to
    // widen it for (contrast Hl2RxDsp::kRxFilterTaps, which exists solely
    // for that reason).

    auto channel = WdspChannel::create(wc, &result.error);
    if (!channel)
        return result;
    // The analyzer reuses the channel's id as its analyzer slot: both are
    // unique for as long as the channel lives, and RebuildResult/this class
    // destroy the analyzer before the channel returns the id. Built here, on
    // the build thread, because its first SetAnalyzer() plans FFTW_PATIENT.
    AnanPanAnalyzer::Settings as;
    as.sampleRateHz = config.inputSampleRateHz;
    as.numPoints = config.panPoints;
    as.framesPerSecond = config.spectrumFps;
    as.averageTimeMs = config.spectrumAverageMs;
    as.logAverage = config.spectrumLogAverage;
    auto analyzer = AnanPanAnalyzer::create(channel->channelId(), as, &result.error);
    if (!analyzer)
        return result;   // channel is released here, analyzer never existed
    result.outputBlockSize = channel->outputBlockSize();
    result.inputSampleRateHz = config.inputSampleRateHz;
    result.channel = std::move(channel);
    result.analyzer = std::move(analyzer);
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
    // buildChannel() never returns one without the other; refuse a result
    // that does rather than install a channel with no spectrum stage.
    if (!result.channel || !result.analyzer)
        return false;
    installChannel(std::move(result));
    return true;
}

void AnanRxDsp::installChannel(RebuildResult result)
{
    // The only update site for this field outside configure()'s own
    // synchronous m_config = config -- see RebuildResult::inputSampleRateHz's
    // comment. Must land before droopTableForRate() is ever consulted again,
    // which processIqBlock() does on every block once m_channel is swapped
    // below.
    m_config.inputSampleRateHz = result.inputSampleRateHz;

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
    // PcmFormat accepts 24000/48000 only. AnanBackend hardcodes 24000 today, so
    // this cannot fail in production — but if that rate ever moves, a silent
    // refusal here stops ANAN audio dead while the spectrum keeps updating,
    // which reads as a dead radio rather than a configuration error.
    if (!m_pcmProducer.start(PcmPurpose::Speaker, -1,
                             {m_config.audioSampleRateHz, PcmLayout::Stereo})) {
        qWarning() << "AnanRxDsp: no PCM producer for audio rate"
                   << m_config.audioSampleRateHz
                   << "Hz - RX audio will be silent on this channel";
    }
    m_dcBlockL.reset();
    m_dcBlockR.reset();

    // Same idea for the S-meter, one stage further back: the new channel's
    // own average starts at zero (create_meter() -> flush_meter()), so its
    // first readings are low for the length of the tap's time constant
    // whether or not a mute is involved. A rate change arms this twice --
    // here, and again when beginRateChange()'s settle window unmutes -- which
    // costs nothing: the window is re-armed, not accumulated.
    armMeterSettle();

    // Re-apply m_config's CURRENT values -- for configure() this is exactly
    // what buildChannel() was just given (m_config == config already); for
    // a rate-change swap it may include mode/filter/AGC changes the operator
    // made WHILE the build was in flight, which only ever reached m_config
    // (setMode() et al.'s in-flight gating) and never reached the snapshot
    // buildChannel() actually built from.
    result.channel->setMode(m_config.mode);
    result.channel->setFilter(m_config.filterLowHz, m_config.filterHighHz);
    result.channel->setAgcFixedGain(m_config.agcFixedGainDb);
    result.channel->setAgc(m_config.agcMode, m_config.maximumAgcGainDb);
    // A rebuild creates a fresh channel; restore the operator's current
    // slice offset rather than silently snapping the slice to centre.
    if (m_shiftHz != 0.0)
        result.channel->setShift(m_shiftHz);
    // Sent even when it matches what the channel was built with: the operator
    // may have moved the NB button while the background build ran.
    if (!result.channel->setNoiseBlanker(m_config.noiseBlanker,
                                         m_config.noiseBlankerLevel,
                                         m_config.noiseBlankerFill)) {
        qCWarning(lcAnanRxDsp) << "noise blanker refused by the rebuilt channel";
    }
    // The hold flag belongs to the channel, so a rebuild loses it. A rate
    // change swaps the channel while audio is muted for its settle window.
    result.channel->setNoiseBlankerHold(m_audioMuted);

    // Stop the OUTGOING channel before it is destroyed so close() skips the 100 ms
    // stop-and-flush timeout; this is the processIq() thread, so no block reaches it
    // in between. Not in beginRebuild(): the old channel must keep producing audio
    // through the background build. Checked as in the destructor.
    if (m_channel && !m_channel->setRunning(false)) {
        qCWarning(lcAnanRxDsp)
            << "could not stop the outgoing WDSP channel before the swap: a "
               "processIq callback was in flight. The rebuild will pay WDSP's "
               "100 ms stop-and-flush timeout.";
    }
    // The analyzer was built for m_config.spectrumFps as it stood when the
    // build began; bring it up to the operator's current rate.
    result.analyzer->setFramesPerSecond(m_config.spectrumFps);
    result.analyzer->setNumPoints(m_config.panPoints);
    result.analyzer->setAverageTimeMs(m_config.spectrumAverageMs);
    result.analyzer->setLogAverage(m_config.spectrumLogAverage);
    // Analyzer before channel: the outgoing analyzer uses the outgoing
    // channel's id as its slot and must be destroyed before that channel
    // releases the id -- see RebuildResult.
    m_analyzer = std::move(result.analyzer);
    m_channel = std::move(result.channel);
    publishNoiseBlankerState();
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

void AnanRxDsp::setAgcFixedGain(double fixedGainDb)
{
    m_config.agcFixedGainDb = fixedGainDb;
    if (m_channel && !m_rebuildInFlight)
        m_channel->setAgcFixedGain(fixedGainDb);
}

void AnanRxDsp::setAudioMuted(bool muted)
{
    // The mute LIFTING is the edge that matters to the S-meter: WDSP's
    // average has been integrating the zeros this class fed it for the whole
    // mute and nothing flushes it, so the first blocks after the unmute still
    // read that silence. Swallow them instead of publishing them -- see
    // WdspSMeter.h for why the guard in processIqBlock() is not enough on
    // its own.
    if (m_audioMuted && !muted)
        armMeterSettle();
    m_audioMuted = muted;
    // Muted, the channel is clocked with ZEROS. The noise blanker triggers on
    // magnitude against a running average magnitude, so silence would drag
    // that average toward zero and the first real block afterwards would look
    // like one long impulse and be blanked. Holding the stage freezes the
    // average at the pre-mute level; see WdspChannel::setNoiseBlankerHold().
    if (m_channel)
        m_channel->setNoiseBlankerHold(muted);
}

void AnanRxDsp::setNoiseBlanker(WdspChannel::NoiseBlanker kind, int level,
                                WdspChannel::NoiseBlankerFill fill)
{
    m_config.noiseBlanker = kind;
    m_config.noiseBlankerLevel = std::clamp(level, 0, 100);
    m_config.noiseBlankerFill = fill;
    if (!m_channel || m_rebuildInFlight)
        return;
    // WdspChannel refuses, rather than blocks on, a control call that races
    // another one. Logged so a refused toggle is not silent; m_config keeps
    // the request and the next rebuild applies it.
    if (!m_channel->setNoiseBlanker(m_config.noiseBlanker,
                                    m_config.noiseBlankerLevel,
                                    m_config.noiseBlankerFill)) {
        qCWarning(lcAnanRxDsp) << "noise blanker kind" << static_cast<int>(kind)
                               << "level" << m_config.noiseBlankerLevel
                               << "fill" << static_cast<int>(fill)
                               << "refused by the channel";
    }
    publishNoiseBlankerState();
}

void AnanRxDsp::publishNoiseBlankerState()
{
    // Read the applied channel even after refusal, never the requested config.
    const int state = m_channel
        ? m_channel->config().noiseBlankerLevel
              + kNbLevelStride * static_cast<int>(m_channel->noiseBlankerKind())
              + kNbLevelStride * kNbKindStride
                    * static_cast<int>(m_channel->noiseBlankerFill())
        : -1;
    m_nbAppliedState.store(state, std::memory_order_relaxed);
}

void AnanRxDsp::setSpectrumRateFps(int fps)
{
    m_spectrumIntervalMs = fps > 0 ? (1000 / fps) : 0;
    if (fps > 0) {
        m_config.spectrumFps = fps;
        // Mid-rebuild the incoming analyzer picks the rate up at install
        // (installChannel()); the outgoing one is about to be discarded.
        if (m_analyzer && !m_rebuildInFlight)
            m_analyzer->setFramesPerSecond(fps);
    }
    // Do NOT reset the clock or the last-emit stamp -- a rate change
    // mid-stream should take effect on the next frame that comes due, not
    // grant an immediate extra one.
}

void AnanRxDsp::setSpectrumAverageMs(int ms)
{
    if (ms < 0)
        return;
    m_config.spectrumAverageMs = ms;
    // Same deferral as setSpectrumRateFps(): the incoming analyzer picks it
    // up at install.
    if (m_analyzer && !m_rebuildInFlight)
        m_analyzer->setAverageTimeMs(ms);
}

void AnanRxDsp::setPanPoints(int points)
{
    if (points < 2)
        return;
    m_config.panPoints = std::min(points, AnanPanAnalyzer::kMaxPoints);
    // Same deferral as setSpectrumRateFps(): the incoming analyzer picks it
    // up at install.
    if (m_analyzer && !m_rebuildInFlight)
        m_analyzer->setNumPoints(m_config.panPoints);
}

void AnanRxDsp::setSpectrumLogAverage(bool on)
{
    m_config.spectrumLogAverage = on;
    if (m_analyzer && !m_rebuildInFlight)
        m_analyzer->setLogAverage(on);
}

void AnanRxDsp::setDroopCorrectionTable(int rateKsps, const std::vector<float>& table)
{
    if (table.size() != kDroopCorrectionFftSize)
        return;
    bool valid = false;
    for (const int r : kDdc0RatesKsps)
        valid |= (r == rateKsps);
    if (!valid)
        return;
    DroopCorrectionTable t;
    std::copy(table.begin(), table.end(), t.begin());
    m_droopTables[rateKsps] = t;
}

void AnanRxDsp::setDroopCorrectionBypassed(bool bypassed)
{
    m_droopBypassed = bypassed;
}

void AnanRxDsp::clearDroopCorrectionTables()
{
    m_droopTables.clear();
}

const DroopCorrectionTable& AnanRxDsp::droopTableForRate(int rateKsps) const noexcept
{
    // Returning the kDroopCorrectionZero OBJECT (not a zero-valued copy) is
    // what also suppresses the edge fade in processIqBlock(), which tests
    // identity against exactly this address -- see
    // setDroopCorrectionBypassed()'s comment.
    if (m_droopBypassed)
        return kDroopCorrectionZero;
    const auto it = m_droopTables.constFind(rateKsps);
    return it != m_droopTables.constEnd() ? it.value() : kDroopCorrectionZero;
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

void AnanRxDsp::onSequenceGap()
{
    if (!m_analyzer) {
        return;   // between rebuilds; the new analyzer starts empty
    }
    // Counted only when something was actually in flight -- see
    // Hl2RxDsp::onSequenceGap() for why a boundary-aligned gap must not be
    // counted, and why neither the frame-rate clock nor the audio path is
    // touched here.
    if (m_analyzer->dropStagedPartial() > 0) {
        m_spectrumGapDiscards.fetch_add(1, std::memory_order_relaxed);
    }
}

void AnanRxDsp::processIqBlock(const std::vector<std::complex<float>>& iq)
{
    if (!m_channel)
        return;

    // Handedness: read docs/HERMES.md §16 before touching this. WDSP's RXA as
    // configured here selects the opposite sign to its passband bounds, and the HPSDR
    // wire is the conjugate of the analytic convention (confirmed for P2 by
    // `radiocert rx` on WWV and by SDR++ on an RSP1B). So the demodulator takes the
    // raw wire and the spectrum is mirrored exactly once (§16.6 rule 2) -- inside
    // WDSP's analyzer, whose Spectrum0() swaps I and Q. Conjugating here too would
    // mirror twice. anan_rxdsp_handedness_test pins this.

    // Panadapter: every block goes to the analyzer, whatever the display rate
    // -- its FFTs overlap so that one completes per display frame over fresh
    // samples, and none are thrown away. Only TAKING a frame is paced here.
    m_analyzer->feed(iq);
    if (spectrumFrameDue() && m_analyzer->takeFrame(m_bins)) {
        // Real DDC0 roll-off -- the anti-alias FIR's transition band, not CIC
        // sin(x)/x (see AnanDroopCorrection.h). The analyzer's points are
        // already time-averaged, so the correction lands on the averaged
        // level. inputSampleRateHz is always an exact multiple of 1000 for
        // the six valid DDC0 rates.
        const DroopCorrectionTable& droopTable =
            droopTableForRate(m_config.inputSampleRateHz / 1000);
        applyDroopCorrectionDbResampled(m_bins, droopTable);
        // Cosmetic edge fade (see applyEdgeFade()). connectRadio() seeds defaults for
        // all six rates, so on a G2 this always runs; the identity test only suppresses
        // it during a calibration bypass, when droopTableForRate() returns the
        // kDroopCorrectionZero object so a sweep measures the radio, not our fade.
        if (&droopTable != &kDroopCorrectionZero)
            applyEdgeFade(m_bins);
        emit spectrumReady(m_bins);
        m_lastSpectrumMs = m_spectrumClock.elapsed();
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
        // Count every outcome, Ok included -- the Ok count is the denominator
        // a fault total has to be read against. Same rule, same words and the
        // same bounded log schedule as Hl2RxDsp::processIqBlock: these two
        // stages are copies of each other and the counting rule is the part
        // that must not drift, which is why it lives in WdspProcessTally.h
        // rather than twice here.
        const std::uint64_t seen = m_processTally.record(res);
        if (res != WdspChannel::ProcessResult::Ok) {
            // Underrun excluded from the log and NOT from the count: it is
            // normal while the asynchronous output side fills, and logging it
            // would drown the four outcomes that are not normal.
            //
            // After processIq() returns, never inside it -- qCWarning
            // allocates, and allocating between WdspChannel's two reads of
            // wdspPortAllocationSequence() would manufacture the very
            // AllocationViolation being reported.
            if (res != WdspChannel::ProcessResult::Underrun
                && WdspProcessTally::shouldLog(seen)) {
                qCWarning(lcAnanRxDsp)
                    << "WDSP processIq failed:" << WdspProcessTally::name(res)
                    << "- occurrence" << seen
                    << "on WDSP channel" << m_channel->channelId()
                    << "- this block produces no audio";
            }
            continue;   // underrun while the pipeline fills, etc. -- no output yet
        }

        const std::size_t outN = m_left.size();
        for (std::size_t k = 0; k < outN; ++k) {
            m_stereo[2 * k] = m_dcBlockL.process(m_left[k]);
            m_stereo[2 * k + 1] = m_dcBlockR.process(m_right[k]);
        }
        QVector<float> samples(m_stereo.begin(), m_stereo.end());
        if (const auto frame = m_pcmProducer.produce(std::move(samples))) {
            emit pcmReady(*frame);
            emit audioReady(m_stereo);
        }
        // AVERAGE, NOT PEAK: xmeter's `avg` is an EMA of I*I + Q*Q, `peak` a decaying
        // peak-hold. They agree on a carrier but peak reads band noise ~11-14 dB high,
        // and S9 (-73 dBm) is an RMS quantity. The backend applies its own ballistics.
        // Gated off while muted and for the settle window after unmute or a channel
        // install (the channel is fed zeros, and the average would carry that silence
        // past the unmute; see WdspSMeter.h). The gate also fixes cadence at one reading
        // per 48k-equivalent block (~47/s at every DDC0 rate). Blocks that underrun
        // above do not tick it.
        if (!m_audioMuted && m_meterTap.tick()) {
            emit meterUpdate(static_cast<float>(
                m_channel->meter(WdspChannel::Meter::SignalAverage)));
        }
    }

    if (consumed > 0)
        m_iqBuffer.erase(m_iqBuffer.begin(),
                         m_iqBuffer.begin() + static_cast<std::ptrdiff_t>(consumed));
}

}  // namespace AetherSDR::anan
