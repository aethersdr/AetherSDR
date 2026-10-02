#include "core/backends/hl2/Hl2RxDsp.h"

#include "core/backends/hl2/Hl2AdcPairing.h"

#include <QLoggingCategory>
#include <QMetaType>

#include <algorithm>
#include <cmath>
#include <cstdint>

Q_LOGGING_CATEGORY(lcHl2RxDsp, "aether.hl2.rxdsp")

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

Hl2RxDsp::~Hl2RxDsp()
{
    // Stop the channel before destroying it: WdspChannel::close()'s blocking
    // stop-and-flush can only complete while fexchange* is being called, so an
    // unstopped channel burns WDSP's 100 ms timeout (docs/HERMES.md §13 item 9b).
    // Precondition: no block reaches processIq() after this, since every
    // destroying path has already left the sample fan-out. Stop, then clock,
    // then destroy is a use-after-free without WDSP patch 9 (#5628).
    // setRunning() refuses while a processIq() callback is in flight; that would
    // mean the I/O-thread deleteLater() ownership assumption broke, so warn.
    if (m_channel && !m_channel->setRunning(false)) {
        qCWarning(lcHl2RxDsp)
            << "could not stop the WDSP channel before destroying it: a "
               "processIq callback was in flight. Teardown will pay WDSP's "
               "100 ms stop-and-flush timeout.";
    }
}

bool Hl2RxDsp::configure(const Config& config, std::string* error)
{
    // Guard the rate/block inputs before the block-size division below. These
    // can come straight from a RadioConnectRequest params override, where a
    // missing or malformed "sampleRateHz" decodes to 0 (QVariant::toInt) — an
    // integer divide-by-zero in the dspBlockSize computation. Reject at the
    // boundary rather than crash or build a nonsensical WDSP channel.
    //
    // Repeated inside buildChannel() rather than relied on from here, because a
    // rate change reaches buildChannel() DIRECTLY and never routes through this
    // function — see the header comment on both.
    if (config.inputSampleRateHz <= 0 || config.audioSampleRateHz <= 0
        || config.dspBlockSize <= 0) {
        if (error) {
            *error = "Hl2RxDsp: input/audio sample rate and DSP block size "
                     "must all be positive";
        }
        return false;
    }

    m_config = config;

    // SYNCHRONOUS, on this object's own thread: build, then install. Identical
    // in effect to what this function did as one block before the split — the
    // split exists so a live rate change can put the two halves on DIFFERENT
    // threads and keep the old channel producing audio in between.
    RebuildResult result = buildChannel(config, m_nbOn, m_nbLevel);
    if (!result.channel) {
        if (error)
            *error = result.error;
        return false;
    }
    installChannel(std::move(result));
    return true;
}

Hl2RxDsp::RebuildResult Hl2RxDsp::buildChannel(const Config& config,
                                               bool noiseBlankerEnabled,
                                               int noiseBlankerLevel)
{
    RebuildResult result;

    // Same guard as configure() — see its comment. Repeated because this is the
    // entry point a live rate change actually calls, from a thread that is not
    // the owning object's, where there is no m_config to fall back on.
    if (config.inputSampleRateHz <= 0 || config.audioSampleRateHz <= 0
        || config.dspBlockSize <= 0) {
        result.error = "Hl2RxDsp: input/audio sample rate and DSP block size "
                       "must all be positive";
        return result;
    }

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
    // Opened WITH the operator's blanker rather than switched on afterwards, so
    // the very first block through a rebuilt chain is already blanked and the
    // stage is not re-armed (and therefore briefly deaf to impulses) on a rate
    // change. Config deliberately does not carry the blanker — it survives a
    // rebuild in its own members — so it is passed in; see the header.
    wc.noiseBlankerEnabled = noiseBlankerEnabled;
    wc.noiseBlankerLevel = noiseBlankerLevel;
    // Filter length. WDSP's default of 2048 is fine for a passband edge, but it
    // also sets the NARROWEST POSSIBLE NOTCH — min_notch_width is
    // 1600 / (nc/256) Hz at 48 kHz, so 2048 taps floors a notch at 200 Hz and
    // WDSP silently widens anything narrower rather than refusing it. A carrier
    // heterodyne wants ~50 Hz, which needs 8192. That is what pihpsdr runs
    // (receiver.c), and the cost is filter-delay, not CPU — a cost minimum
    // phase removes outside CW; see rxMinimumPhaseFor().
    wc.filterTaps = kRxFilterTaps;
    // Opened in the phase its mode wants (rxMinimumPhaseFor). installChannel()
    // re-applies the phase for the mode in force at the swap, and that is what
    // makes it CORRECT; this makes that re-apply a no-op in the usual case, so
    // a build does not open linear and then re-plan all six masks under the
    // process-global FFTW lock.
    wc.minimumPhase = rxMinimumPhaseFor(config.mode);

    auto channel = WdspChannel::create(wc, &result.error);
    if (!channel)
        return result;
    result.outputBlockSize = channel->outputBlockSize();
    result.built = config;
    result.builtNbOn = noiseBlankerEnabled;
    result.builtNbLevel = noiseBlankerLevel;
    // Constructed HERE, not at install, because it plans an FFT and that is the
    // other half of what makes a rebuild slow. Hl2Spectrum's constructor takes
    // WdspChannel::fftwSetupLock() (see its own comment), which is the same
    // process-wide lock OpenChannel above runs under — so building this on a
    // background thread is serialised against every other FFTW planner user in
    // the process, including a concurrent connect on the I/O thread.
    // The IQ rate is what turns the averaging time into a blend weight.
    result.spectrum = std::make_unique<Hl2Spectrum>(
        config.fftSize, static_cast<double>(config.inputSampleRateHz));
    result.channel = std::move(channel);
    return result;
}

void Hl2RxDsp::beginRebuild(const Config& config)
{
    ++m_rebuildsInFlight;
    // The OPERATOR-FACING half only. The geometry fields describe the channel
    // that is still running and still producing audio, and a build that fails
    // must leave this object describing the chain it really has — see the
    // header. installChannel() takes the geometry from what was actually built.
    m_config.mode = config.mode;
    m_config.filterLowHz = config.filterLowHz;
    m_config.filterHighHz = config.filterHighHz;
    m_config.agcMode = config.agcMode;
    m_config.maximumAgcGainDb = config.maximumAgcGainDb;
}

void Hl2RxDsp::abandonRebuild()
{
    if (m_rebuildsInFlight > 0)
        --m_rebuildsInFlight;
}

bool Hl2RxDsp::installRebuiltChannel(RebuildResult result)
{
    abandonRebuild();   // balance beginRebuild() whether or not the build worked
    if (!result.channel)
        return false;
    installChannel(std::move(result));
    return true;
}

void Hl2RxDsp::installChannel(RebuildResult result)
{
    // Re-opens the control verbs for the length of this function — see
    // canPushToChannel(). The re-application below IS those verbs.
    m_installing = true;
    struct InstallScope {
        bool& flag;
        ~InstallScope() { flag = false; }
    } scope {m_installing};

    const Config& config = result.built;
    // The geometry this object now HAS. Only updated here, on a successful
    // swap — see beginRebuild().
    m_config.inputSampleRateHz = config.inputSampleRateHz;
    m_config.audioSampleRateHz = config.audioSampleRateHz;
    m_config.dspBlockSize = config.dspBlockSize;
    m_config.fftSize = config.fftSize;
    m_config.blockForOutput = config.blockForOutput;

    // Stop the outgoing channel before destroying it, for the destructor's
    // reason: a rate change rebuilds every receiver (the DDC rate register is
    // radio-wide), so each unstopped close costs 100 ms (docs/HERMES.md §22.4).
    // After the build, never before: the old channel keeps audio flowing during
    // a background build, and a failed synchronous create() leaves it in place.
    // This runs on the DSP thread, so no block reaches it before destruction.
    if (m_channel && !m_channel->setRunning(false)) {
        qCWarning(lcHl2RxDsp)
            << "could not stop the outgoing WDSP channel before the swap: a "
               "processIq callback was in flight. The rebuild will pay WDSP's "
               "100 ms stop-and-flush timeout.";
    }
    m_channel = std::move(result.channel);
    m_spectrum = std::move(result.spectrum);
    // The operator's averaging, which the fresh spectrum does not know. A new
    // span is new geometry, so the average starts over (it was constructed
    // empty) — but at the operator's time constant, not at none.
    if (m_spectrum) {
        m_spectrum->setAverageTimeMs(static_cast<double>(m_spectrumAverageMs));
        m_spectrum->setLogAverage(m_spectrumLogAverage);
    }

    m_iqBuffer.clear();
    m_i.assign(static_cast<std::size_t>(config.dspBlockSize), 0.0f);
    m_q.assign(static_cast<std::size_t>(config.dspBlockSize), 0.0f);
    const std::size_t outN = result.outputBlockSize;
    m_left.assign(outN, 0.0f);
    m_right.assign(outN, 0.0f);
    m_stereo.assign(outN * 2, 0.0f);
    // DC blocker pole for the AUDIO rate — the blocker runs on WdspChannel's
    // output, not on its 48 kHz internal rate. Recomputed here so a rate change
    // keeps the same corner frequency instead of moving it.
    const float pole = dcBlockerPole(kDcBlockerCornerHz,
                                     static_cast<double>(config.audioSampleRateHz));
    m_dcBlockL.r = pole;
    m_dcBlockR.r = pole;
    m_dcBlockL.reset();
    m_dcBlockR.reset();
    // RE-APPLIED FROM m_config, not from what the build was handed. On the
    // asynchronous path the operator can have moved mode, passband or AGC while
    // the build ran; those updates reached m_config and were deliberately NOT
    // pushed at the old channel (beginRebuild()), so this is the point they
    // land. On the synchronous path m_config == config already and these are
    // the values the channel was just opened with.
    m_channel->setMode(m_config.mode);
    // The chain was BUILT for the mode of its Config; a mode change during a
    // background build lands in m_config instead, so the phase follows it here.
    applyMinimumPhaseForMode();
    m_channel->setFilter(m_config.filterLowHz, m_config.filterHighHz);
    m_channel->setAgc(m_config.agcMode, m_config.maximumAgcGainDb);
    // The squelch, AFTER setMode above so WdspChannel routes it to the stage
    // for the mode actually in force. A fresh channel opens with every
    // squelch stage off; without this a rate change would open the squelch
    // under a lit SQL button.
    pushSquelchToChannel();
    // A rebuild (rate change) creates a fresh channel; restore the operator's
    // current slice offset rather than silently snapping the slice to centre.
    if (m_shiftHz != 0.0)
        m_channel->setShift(m_shiftHz);
    // Same for the notch set. The database belongs to the channel, so a rebuild
    // destroys it — without this replay an operator's notches disappear on any
    // sample-rate change, which reads as the notch feature randomly failing.
    // Tune frequency FIRST: the centres are absolute, so a notch added before
    // the channel knows where it is tuned is placed against a tune frequency of
    // zero and rebuilt at a wildly wrong offset.
    m_channel->setNotchTuneFrequency(m_notchTuneHz);
    // Replayed THROUGH addNotch() rather than straight at the channel, so the
    // rebuild lands under the same WDSP-first rule the live path uses: a notch
    // the fresh channel refuses drops out of the mirror too, instead of leaving
    // a phantom that every later index is measured from. It also stops at the
    // first refusal, because an index that skips one is an index that addresses
    // the wrong notch.
    std::vector<Notch> pending;
    pending.swap(m_notches);
    for (std::size_t index = 0; index < pending.size(); ++index) {
        const Notch& notch = pending[index];
        addNotch(static_cast<int>(index), notch.centerHz, notch.widthHz, notch.active);
    }
    m_channel->setNotchesEnabled(m_notchesEnabled);
    // The blanker's hold flag belongs to the channel, so a rebuild loses it.
    // Re-assert it, or a rate change made while transmitting comes back with
    // the blanker running on the mute path's silence.
    m_channel->setNoiseBlankerHold(m_audioMuted);
    // The channel was OPENED with the blanker buildChannel() was handed, so
    // that pair — not the current request — is what has definitively landed.
    m_nbAppliedOn.store(result.builtNbOn, std::memory_order_relaxed);
    m_nbAppliedLevel.store(result.builtNbLevel, std::memory_order_relaxed);
    // AND THEN THE CURRENT REQUEST, if the operator moved the NB button while a
    // background build was running. setNoiseBlanker() deliberately does not push
    // at the channel during a rebuild (it would block this thread on WDSP's
    // setup mutex), so without this the swap would come back with the blanker
    // the operator had a rebuild ago and the readback would agree with it.
    // Goes through setNoiseBlanker() so the refusal handling stays in one place.
    if (m_nbOn != result.builtNbOn || m_nbLevel != result.builtNbLevel)
        setNoiseBlanker(m_nbOn, m_nbLevel);
    // The ADC-peak reading belongs to the channel that produced it. A rebuild
    // is a NEW channel at a possibly different rate, so carrying the old value
    // across would answer healthSnapshot() with a level measured through a
    // decimation chain that no longer exists. Back to "never observed" until a
    // block has gone through the chain that is actually running.
    m_adcPeakDbfs.store(std::numeric_limits<float>::quiet_NaN(),
                        std::memory_order_relaxed);
    m_adcPeakAtNs.store(0, std::memory_order_relaxed);
    // A fresh RXA's S-meter reads -400 dB until its average fills, so hold the
    // last good reading for the same settle as the mute's release edge. HL2
    // does not mute across a rate change (Hl2Backend's finishRateChange).
    armMeterSettle();
}

void Hl2RxDsp::armMeterSettle()
{
    // WdspSMeter's window: three time constants of RXA.c's 0.100 s S-meter
    // average, counted in blocks. xmeter runs after xnbp, so in CW the
    // linear-phase filter's ~85 ms group delay (4096 samples at 48 kHz) still
    // feeds zeros; the remaining ~0.215 s leaves the first reading ~0.5 dB low
    // (pinned by hl2_adc_sampling_seam_test). The arm also sets the read cadence
    // to every inputRate/48k-th block, ~47 readings/s at any rate.
    m_meterTap.arm(m_config.inputSampleRateHz, m_config.dspBlockSize,
                   kWdspDspSampleRateHz);
}

void Hl2RxDsp::setNoiseBlanker(bool on, int level)
{
    m_nbOn = on;
    m_nbLevel = std::clamp(level, 0, 100);
    if (!canPushToChannel())
        return;   // no chain yet, or a rebuild holds the setup mutex; the
                  // request is held and the swap opens/re-applies with it
    // CHECKED, unlike a fire-and-forget setter, because WdspChannel refuses a
    // control operation that races another one and returns false rather than
    // blocking. Swallowing that would leave this object — and therefore the NB
    // button and the bridge readback — claiming a blanker the channel is not
    // running, which is the one failure this whole feature is built to avoid.
    if (!m_channel->setNoiseBlanker(m_nbOn, m_nbLevel)) {
        qCWarning(lcHl2RxDsp) << "noise blanker" << (m_nbOn ? "on" : "off") << "level"
                         << m_nbLevel << "refused by the channel; the request is "
                            "held and re-applied on the next configure()";
        return;
    }
    m_nbAppliedOn.store(m_nbOn, std::memory_order_relaxed);
    m_nbAppliedLevel.store(m_nbLevel, std::memory_order_relaxed);
}

void Hl2RxDsp::setSquelch(bool on, int level)
{
    m_squelchOn = on;
    m_squelchLevel = std::clamp(level, 0, 100);
    if (!canPushToChannel())
        return;   // held; installChannel() applies it at the swap
    pushSquelchToChannel();
}

void Hl2RxDsp::pushSquelchToChannel()
{
    if (m_channel->setSquelch(m_squelchOn, m_squelchLevel)) {
        m_squelchPending = false;
        return;
    }
    // Logged on the EDGE into pending only: the retry runs once per block, and
    // a refusal that persisted would otherwise log at the block rate.
    if (!m_squelchPending) {
        qCWarning(lcHl2RxDsp) << "squelch" << (m_squelchOn ? "on" : "off") << "level"
                              << m_squelchLevel << "refused by the channel; retrying "
                                 "on the next IQ block";
    }
    m_squelchPending = true;
}

void Hl2RxDsp::setMode(WdspChannel::Mode mode)
{
    m_config.mode = mode;
    // DEFERRED, not lost, while a background rebuild is running: see
    // beginRebuild(). m_config still takes it and installChannel() re-applies
    // it at the swap.
    if (canPushToChannel()) {
        m_channel->setMode(mode);
        applyMinimumPhaseForMode();
    }
}

void Hl2RxDsp::applyMinimumPhaseForMode()
{
    // The phase follows the mode (rxMinimumPhaseFor), so a change INTO or OUT
    // OF CW has to move it too. WdspChannel::setMinimumPhase() returns early
    // when the phase is already right, so SSB <-> digital costs nothing; a
    // real switch re-designs the six masks under the FFTW lock, ~27 ms of
    // control-path work measured on a cold cache (the design FFTs plan with
    // FFTW_ESTIMATE, WDSP patch 12 -- a measured plan here took over a minute,
    // on the thread that paces EP2), and the notch database survives it.
    const bool wanted = rxMinimumPhaseFor(m_config.mode);
    if (!m_channel->setMinimumPhase(wanted)) {
        qCWarning(lcHl2RxDsp) << "could not switch the RX filter to"
                              << (wanted ? "minimum" : "linear")
                              << "phase for the new mode; receive latency stays"
                                 " as it was until the next mode change or"
                                 " rebuild";
    }
}

void Hl2RxDsp::setFilter(double lowHz, double highHz)
{
    m_config.filterLowHz = lowHz;
    m_config.filterHighHz = highHz;
    if (canPushToChannel())
        m_channel->setFilter(lowHz, highHz);
}

void Hl2RxDsp::setAgc(int agcMode, double maximumGainDb)
{
    m_config.agcMode = agcMode;
    m_config.maximumAgcGainDb = maximumGainDb;
    if (canPushToChannel())
        m_channel->setAgc(agcMode, maximumGainDb);
}

void Hl2RxDsp::setAudioMuted(bool muted)
{
    // On the release edge xmeter's average has integrated every zero clocked in
    // while muted (the first unmuted block read -224.5 dBFS vs -10.5 before), so
    // the S-meter would dive on unkey. Unlike the blanker it can't be skipped,
    // so the settle window waits it out.
    if (m_audioMuted && !muted)
        armMeterSettle();
    m_audioMuted = muted;
    // The noise blanker triggers on magnitude vs a running average, so the
    // muted zeros would make the first real sample look like an impulse.
    // Holding makes processIq skip the stage, keeping the pre-TX average.
    // Nothing is flushed on release: a flush leaves it unarmed ~200 ms at
    // backtau 0.05 s (#5499 item 3). The hold sits inside the m_nbActive gate,
    // so with the blanker off (the default) it does nothing.
    if (m_channel)
        m_channel->setNoiseBlankerHold(muted);
}

void Hl2RxDsp::setSpectrumRateFps(int fps)
{
    m_spectrumIntervalMs = fps > 0 ? (1000 / fps) : 0;
    // Do NOT reset the clock or the last-emit stamp. A rate change mid-stream
    // should take effect on the next frame that comes due, not grant an
    // immediate extra one — an operator dragging the FPS slider would
    // otherwise fire a frame per drag step, which is exactly the burst this
    // cap exists to prevent.
}

void Hl2RxDsp::setSpectrumAverageMs(int ms)
{
    m_spectrumAverageMs = ms > 0 ? ms : 0;
    // Not gated on canPushToChannel(): the spectrum is ours, not WDSP's, and
    // touching it takes no WDSP lock. A rebuild in flight will re-apply this
    // from the member at the swap anyway.
    if (m_spectrum)
        m_spectrum->setAverageTimeMs(static_cast<double>(m_spectrumAverageMs));
}

void Hl2RxDsp::setSpectrumLogAverage(bool on)
{
    m_spectrumLogAverage = on;
    if (m_spectrum)
        m_spectrum->setLogAverage(on);
}

void Hl2RxDsp::dropSpectrumAverage()
{
    if (m_spectrum)
        m_spectrum->dropAverage();
}

void Hl2RxDsp::setShift(double shiftHz)
{
    m_shiftHz = shiftHz;
    if (canPushToChannel())
        m_channel->setShift(shiftHz);
}

void Hl2RxDsp::addNotch(int index, double centerHz, double widthHz, bool active)
{
    // Clamp to what this channel can actually produce. WDSP would accept a
    // narrower request and quietly widen it, leaving the operator with a notch
    // wider than the one drawn on the panadapter and no way to tell.
    widthHz = std::max(widthHz, kMinNotchWidthHz);
    if (index < 0 || index > static_cast<int>(m_notches.size()))
        return;
    // WDSP first; the mirror takes the entry only if WDSP accepted it, since a
    // refused entry would put every higher index out of step. With no channel
    // or a rebuild in flight only the mirror takes it and the swap replays it
    // (pushing then would block on the build's setup mutex).
    if (canPushToChannel() && !m_channel->addNotch(index, centerHz, widthHz, active))
        return;
    m_notches.insert(m_notches.begin() + index, Notch {centerHz, widthHz, active});
}

void Hl2RxDsp::clearNotches()
{
    // Delete from the top down so each removal is the last index — WDSP closes
    // the gap on every delete, exactly as removeNotch() relies on.
    //
    // And drop each mirror entry only once WDSP has let go of it, for the reason
    // addNotch() gives: a mirror that empties while the database does not would
    // put every index one notch out — and the caller this exists for is seeding,
    // which would then stack a second copy on top of the set it meant to replace.
    for (int index = static_cast<int>(m_notches.size()) - 1; index >= 0; --index) {
        if (canPushToChannel() && !m_channel->removeNotch(index))
            return;
        m_notches.pop_back();
    }
}

void Hl2RxDsp::editNotch(int index, double centerHz, double widthHz, bool active)
{
    widthHz = std::max(widthHz, kMinNotchWidthHz);
    if (index < 0 || index >= static_cast<int>(m_notches.size()))
        return;
    if (canPushToChannel() && !m_channel->editNotch(index, centerHz, widthHz, active))
        return;   // see addNotch(): the mirror must not claim what WDSP refused
    m_notches[static_cast<std::size_t>(index)] = Notch {centerHz, widthHz, active};
}

void Hl2RxDsp::removeNotch(int index)
{
    if (index < 0 || index >= static_cast<int>(m_notches.size()))
        return;
    if (canPushToChannel() && !m_channel->removeNotch(index))
        return;   // see addNotch(): the mirror must not lose what WDSP kept
    m_notches.erase(m_notches.begin() + index);
}

void Hl2RxDsp::setNotchesEnabled(bool on)
{
    m_notchesEnabled = on;
    if (canPushToChannel())
        m_channel->setNotchesEnabled(on);
}

int Hl2RxDsp::notchCount() const
{
    return static_cast<int>(m_notches.size());
}

int Hl2RxDsp::wdspNotchCount() const
{
    return m_channel ? m_channel->notchCount() : 0;
}

void Hl2RxDsp::setNotchTuneFrequency(double tuneHz)
{
    m_notchTuneHz = tuneHz;
    if (canPushToChannel())
        m_channel->setNotchTuneFrequency(tuneHz);
}

bool Hl2RxDsp::spectrumFrameDue()
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

void Hl2RxDsp::onSequenceGap()
{
    if (!m_spectrum) {
        return;   // between rebuilds; the new spectrum starts empty
    }
    // Counted only when something was actually in flight. A gap that lands on a
    // frame boundary discards nothing and has corrupted nothing, and counting
    // it here would make this row a second, worse copy of `droppedPackets`
    // instead of the narrower statement it exists to make.
    if (m_spectrum->reset() > 0) {
        m_spectrumGapDiscards.fetch_add(1, std::memory_order_relaxed);
    }
    // The frame-rate clock is NOT touched. spectrumFrameDue() measures the
    // operator's requested display interval, and a gap is not a frame having
    // been shown -- resetting m_lastSpectrumMs here would hand the shaper a
    // fresh interval it did not earn and drop the achieved rate by one frame
    // per gap on top of the frame already lost.
}

void Hl2RxDsp::processIqBlock(const std::vector<std::complex<float>>& iq)
{
    if (!m_channel)
        return;

    // A squelch change the channel refused, retried here: this thread is the
    // one that calls processIq(), and this block's call has not started, so
    // no callback of ours is in flight. See setSquelch().
    if (m_squelchPending && canPushToChannel())
        pushSquelchToChannel();

    // The two consumers need opposite handedness (measured; hl2_rxdsp_test,
    // hl2_shift_test): the HPSDR wire puts signals above the NCO at negative
    // frequency, and WDSP's RXA here passes the opposite sign to its passband
    // bounds. So the demodulator takes the raw wire and the spectrum the
    // conjugate. Conjugated before the frame-due branch because the accumulator
    // is fed on both paths.
    m_conjugated.resize(iq.size());
    for (std::size_t n = 0; n < iq.size(); ++n)
        m_conjugated[n] = std::conj(iq[n]);

    // The FFT runs only when a frame is due (see setSpectrumRateFps); otherwise
    // the accumulator is still fed, so the next due frame is a contiguous
    // snapshot without a ~9-EP6-block refill. That keeps the achieved rate
    // independent of span; signals entirely between displayed frames are not
    // seen.
    if (spectrumFrameDue()) {
        // "Due" STAYS true until a frame actually completes: one EP6 block is
        // 126 samples and a frame is 1024, so a frame boundary can be up to one
        // block away even with a full window behind it.
        if (m_spectrum->process(m_conjugated, m_bins) > 0) {
            emit spectrumReady(m_bins);
            m_lastSpectrumMs = m_spectrumClock.elapsed();
        }
    } else {
        // Keep the window fed without paying for a transform. This is the whole
        // saving at a wide span: the FFT is skipped, not merely its emit.
        m_spectrum->accumulate(m_conjugated);
    }

    // Audio: the raw wire; see the handedness note above.
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
            // Not conjugated (see above). Hl2Backend::setSliceFrequency's shift
            // sign depends on this convention; change them together.
            m_i[n] = m_iqBuffer[consumed + n].real();
            m_q[n] = m_iqBuffer[consumed + n].imag();
        }
        consumed += block;

        const auto res = m_channel->processIq(m_i, m_q, m_left, m_right);
        // COUNT EVERY OUTCOME, including Ok — the Ok count is the denominator,
        // and "4 engine errors" against "4 engine errors in 5 blocks" are
        // different reports. See WdspProcessTally.h for why Underrun is not
        // summed with the four faults.
        const std::uint64_t seen = m_processTally.record(res);
        if (res != WdspChannel::ProcessResult::Ok) {
            // Faults are logged on a power-of-two schedule (the first of each
            // kind always) so a per-block fault can't flood the thread that
            // paces EP2. Underrun is normal and only counted. Logged after
            // processIq() returns: allocating between its two
            // wdspPortAllocationSequence() reads would cause an AllocationViolation.
            if (res != WdspChannel::ProcessResult::Underrun
                && WdspProcessTally::shouldLog(seen)) {
                qCWarning(lcHl2RxDsp)
                    << "WDSP processIq failed:" << WdspProcessTally::name(res)
                    << "- occurrence" << seen
                    << "on WDSP channel" << m_channel->channelId()
                    << "- this block produces no audio";
            }
            continue;   // Underrun while the pipeline fills, etc. — no output yet
        }

        const std::size_t outN = m_left.size();
        for (std::size_t k = 0; k < outN; ++k) {
            // DC-block on the way out. AM/SAM arrive with the carrier as a DC
            // pedestal that nothing upstream removes — see DcBlocker in the
            // header for why WDSP's levelfade and the symmetric AM passband
            // both leave it in place.
            m_stereo[2 * k] = m_dcBlockL.process(m_left[k]);
            m_stereo[2 * k + 1] = m_dcBlockR.process(m_right[k]);
        }
        emit audioReady(m_stereo);
        // S-meter from WDSP's average signal meter (xmeter `avg`, an EMA of
        // I*I + Q*Q, the RMS quantity S9 is defined in); the peak-hold reads band
        // noise ~11-14 dB high. Not read while muted or during the settle after
        // unmute, because the EMA integrated the muted zeros. The settle counts
        // WDSP blocks, so a stalled stream can't expire it; one reading per
        // DSP-rate block of input. The backend applies its own ballistics.
        if (!m_audioMuted && m_meterTap.tick()) {
            emit meterUpdate(static_cast<float>(
                m_channel->meter(WdspChannel::Meter::SignalAverage)));
        }
        // Post-DDC half of §13 item 16's ADC pairing: adcmeter just ran on this
        // block's input. Stored for Hl2Backend::healthSnapshot() to poll. Not
        // while muted: zeros would read as a dead converter, and on HL2 the TX
        // shares the RX port, so the held value's age (kSliceStaleMs) is what
        // stops Hl2AdcPairing.h blaming our own carrier on now. A sentinel gets
        // no timestamp: value and age are stored together or not at all.
        if (!m_audioMuted) {
            const double pk = m_channel->meter(WdspChannel::Meter::AdcPeak);
            if (adcMeterReadingIsReal(pk)) {
                m_adcPeakDbfs.store(static_cast<float>(pk), std::memory_order_relaxed);
                m_adcPeakAtNs.store(steadyNowNs(), std::memory_order_relaxed);
            }
        }
    }

    if (consumed > 0)
        m_iqBuffer.erase(m_iqBuffer.begin(),
                         m_iqBuffer.begin() + static_cast<std::ptrdiff_t>(consumed));
}

}  // namespace AetherSDR::hl2
