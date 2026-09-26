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
    // STOP THE CHANNEL BEFORE WE LET IT GO. WdspChannel::close() asks WDSP to
    // stop-and-flush in the BLOCKING form, and that wait can only be satisfied
    // by a host still calling fexchange* — which, at teardown, nothing is. So a
    // channel destroyed while WDSP still thinks it is running burns WDSP's full
    // 100 ms timeout, per channel, on whichever thread is doing the tearing
    // down. Stopping here makes close()'s SetChannelState a no-op and the wait
    // is skipped entirely. docs/HERMES.md §13 item 9b.
    //
    // WHAT THIS DOES NOT BUY: a clean down-slew. Every path that destroys an
    // Hl2RxDsp has already withdrawn it from the sample fan-out (or the wire
    // was never started), so no block reaches processIq() after this line and
    // WDSP's mute ramp never actually runs. The saving is the skipped wait, not
    // a smoother exit. The one place a stop CAN be taken with samples still
    // flowing is the T/R mute, which is item 9a and is a bench decision.
    //
    // "NO BLOCK REACHES processIq() AFTER THIS LINE" IS A CORRECTNESS
    // PRECONDITION, NOT A PERFORMANCE DETAIL, and it is stated here rather than
    // enforced. A stop followed by clocking leaves WDSP's flushChannel thread
    // runnable, and before AetherSDR patch 9 nothing in CloseChannel waited for
    // it: destroy_main() freed the RXA chain while that thread was inside
    // flush_rxa() on it. MEASURED as a use-after-free, 30 of 30 trials, on the
    // exact shape stop-then-clock-then-destroy; stop-then-destroy with nothing
    // clocked between was clean. Found by ten9876 in review of #5628.
    //
    // Patch 9 makes that barrier explicit in the vendored tree, so this
    // destructor is no longer the only thing standing between the two. The
    // precondition is still worth stating: it is what makes the ramp's absence
    // here intentional rather than a silent loss, and item 9a is the change that
    // will make a clocked stop routine.
    //
    // CHECKED, not discarded. setRunning() goes through beginControlOperation(),
    // which REFUSES rather than waits when a processIq() callback is in flight.
    // That cannot happen here today — Hl2Backend destroys these through
    // deleteLater() posted to the I/O thread, which is the thread that drives
    // processIqBlock() — so a false is not a hazard, it is a statement that the
    // ownership assumption above has stopped being true. Say so instead of just
    // getting slow again.
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
    // (receiver.c), and the cost is filter-delay, not CPU.
    wc.filterTaps = kRxFilterTaps;

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
    result.spectrum = std::make_unique<Hl2Spectrum>(config.fftSize);
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

    // Stop the OUTGOING channel before the assignment below destroys it — same
    // reason as the destructor's, and this is the path that actually shows: a
    // rate change rebuilds EVERY receiver, because the DDC rate register is
    // radio-wide, so each un-stopped close added WDSP's 100 ms stop-and-flush
    // timeout per receiver (docs/HERMES.md §22.4).
    //
    // AFTER the build, never before it. The old channel keeps producing audio
    // for the whole of a background build; stopping it at the START would trade
    // exactly the receive audio the asynchronous rebuild exists to preserve for
    // 100 ms of teardown. And on the synchronous path create() can fail, which
    // leaves the existing chain in place — stopping first would make a failed
    // rebuild silence a working receiver.
    //
    // No drain here either. This function always runs ON the DSP thread, which
    // is the same thread that calls processIq(), so no block reaches the old
    // channel between this line and its destruction: the down-slew does NOT
    // complete and this buys the skipped wait, nothing more.
    //
    // Checked for the same reason as the destructor's — see there.
    if (m_channel && !m_channel->setRunning(false)) {
        qCWarning(lcHl2RxDsp)
            << "could not stop the outgoing WDSP channel before the swap: a "
               "processIq callback was in flight. The rebuild will pay WDSP's "
               "100 ms stop-and-flush timeout.";
    }
    m_channel = std::move(result.channel);
    m_spectrum = std::move(result.spectrum);

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
    m_channel->setFilter(m_config.filterLowHz, m_config.filterHighHz);
    m_channel->setAgc(m_config.agcMode, m_config.maximumAgcGainDb);
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
    // AND THE S-METER'S EQUIVALENT OF "NEVER OBSERVED", which is a wait rather
    // than a sentinel. A fresh WdspChannel means a fresh RXA, and create_meter()
    // ends in flush_meter(): avg is 0 and the tap reads -400 dB until enough
    // real samples have gone through it. The ADC peak two lines above answers
    // that with NaN because its consumer is a poll that can say "not reported";
    // this one is a signal into a needle, so there is nothing to publish but
    // the last good reading, held, until the new channel's average is real.
    //
    // The same settle length as the mute's release edge, and for the same
    // reason — it is the accumulator's time constant either way. HL2
    // deliberately does NOT mute across a rate change (see finishRateChange in
    // Hl2Backend), so without this a sample-rate change publishes the dive with
    // no mute anywhere near it.
    armMeterSettle();
}

void Hl2RxDsp::armMeterSettle()
{
    // The window is WdspSMeter's: three time constants of the 0.100 s average
    // RXA.c builds the S-meter with, counted in blocks so it measures the
    // clock the meter integrates on. Two things about it are HL2's:
    //
    // The EMA does NOT get all three taus. xmeter(smeter) runs after xnbp in
    // xrxa(), so the samples it sees have been through the RXA bandpass: a
    // linear-phase FIR of kRxFilterTaps, whose group delay is about half that
    // — 4096 samples, ~85 ms at the 48 kHz DSP rate — during which the meter
    // is still being fed the zeros that were in the filter when the mute
    // ended. What is left for the average to wash out in is ~0.215 s, so the
    // first published reading sits roughly half a decibel below the true
    // level rather than the ~0.2 dB three clean taus would give. That is the
    // number to compare against: half a dB, not zero, against the ~214 dB
    // step this replaces. Both halves of that trade are measured in
    // hl2_adc_sampling_seam_test, which measures the reading itself rather
    // than retyping the arithmetic.
    //
    // And the same arm sets the read cadence: every inputRate/48k-th block,
    // so the backend's smoother sees ~47 readings a second at 384 ksps as at
    // 48 rather than eight times as many.
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

void Hl2RxDsp::setMode(WdspChannel::Mode mode)
{
    m_config.mode = mode;
    // DEFERRED, not lost, while a background rebuild is running: see
    // beginRebuild(). m_config still takes it and installChannel() re-applies
    // it at the swap.
    if (canPushToChannel())
        m_channel->setMode(mode);
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
    // THE RELEASE EDGE IS THE ONE THAT PUBLISHES THE SILENCE. Suppressing the
    // S-meter tap while muted stops the needle walking down during the over,
    // but it does not undo what the mute did to WDSP's accumulator: xmeter()
    // integrated every one of the zeros this class clocked in, for the whole
    // over, and there is no flush that helps. So the first block after the
    // unmute reads the silence at full depth and the needle DIVES on unkey
    // instead of on key-down — the same artefact, moved to the other edge.
    //
    // Measured by ten9876 in review of this change, on a real Hl2RxDsp with a
    // steady tone, 235 muted blocks (~5 s) at 48 kHz/1024, with Hl2Backend's
    // own attack/decay EMA applied to what this tap emits: the first unmuted
    // block published -224.5 dBFS against a pre-mute -10.5, which the backend
    // EMA turns into a ~32 dB step down at the instant of unkey, bottoming ~54
    // dB down ~85 ms later and taking ~300 ms to climb back. The tap itself was
    // within 1 dB by block 12.
    //
    // This file already recognises the shape: setNoiseBlankerHold() below
    // exists because a zero-fed running average makes the first real sample
    // afterwards look wrong. The blanker can be held because the stage is ours
    // to skip. The meter cannot, so it is WAITED OUT instead.
    if (m_audioMuted && !muted)
        armMeterSettle();
    m_audioMuted = muted;
    // The mute path clocks the channel with ZEROS, and the noise blanker
    // triggers on a RATIO — magnitude against a running average magnitude — so
    // a transmit period of silence drags that average toward zero and the first
    // real sample afterwards looks like an enormous impulse. The blanker would
    // then gate the start of every receive period.
    //
    // Holding it makes WdspChannel::processIq SKIP the stage entirely rather
    // than feed it, so its running average still holds the pre-transmit signal
    // level and it is already armed for the first receive sample. NOTHING IS
    // FLUSHED, here or on release — this sentence used to say "WdspChannel
    // flushes it on release", which is the opposite of what that branch does
    // and of what its own comment argues at length. The only flush_anbEXT call
    // in the tree is in WdspChannel::setNoiseBlanker, on ENABLE. A flush on
    // release would leave the blanker unarmed for ~200 ms at backtau 0.05 s —
    // measured as a blanked/unblanked impulse peak ratio of 1.000, bit
    // identical to the blanker being switched off. (#5499 item 3)
    //
    // With the blanker OFF this does nothing at all: processIq's m_nbHold check
    // sits inside the m_nbActive gate, so the hold gates a stage that is not
    // running. Off is the default, and it is the configuration #5497's unkey
    // transient was measured in — so whatever this mitigation is worth, it is
    // not in the path there. Noted rather than "fixed": hoisting the hold out
    // of the m_nbActive block would be a behaviour change to a real-time path
    // with no reported problem behind it.
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
    // Order matters: WDSP first, and the mirror only if it took it. The mirror
    // is what configure() replays after a rate change, so an entry WDSP refused
    // (database full, or a beginControlOperation that lost to a concurrent
    // reconfigure) would put every index above it permanently out of step —
    // and the desync would outlive the rebuild that might have resynced it.
    // With no channel yet the mirror still takes it; that replay is the point.
    // WHILE A REBUILD IS IN FLIGHT the mirror takes it and WDSP is not asked —
    // the same path as "no channel yet", and for a related reason: the notch
    // database is rebuilt from this mirror at the swap, so the entry is not
    // lost, it merely does nothing for the tens of milliseconds the build has
    // left. Asking WDSP instead would block this object's thread on the setup
    // mutex the background build holds, which is the whole starvation
    // beginRebuild() exists to prevent.
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

    // The two consumers need OPPOSITE handedness, and each was wired to the
    // other's. Two facts, both measured rather than reasoned:
    //
    //   1. The HPSDR wire is the conjugate of the analytic convention: a signal
    //      ABOVE the NCO arrives at a NEGATIVE frequency.
    //   2. WDSP's RXA, as configured here, selects the OPPOSITE sign to its
    //      passband bounds — USB with [+150,+3000] passes negative frequencies.
    //      (Confirmed independently by hl2_rxdsp_test and hl2_shift_test.)
    //
    // So the DEMODULATOR wants the raw wire — (1) and (2) cancel — while the
    // SPECTRUM, which has no such quirk, wants the conjugate.
    //
    // Conjugated unconditionally, ahead of the frame-due branch below: the
    // accumulator is fed on BOTH paths and it feeds the same FFT, so a raw
    // block accumulated during a skipped interval would mirror part of the very
    // next displayed frame. The cost is one pass over a block whichever branch
    // runs, which is the cheap half of what the shaper already skips.
    m_conjugated.resize(iq.size());
    for (std::size_t n = 0; n < iq.size(); ++n)
        m_conjugated[n] = std::conj(iq[n]);

    // Panadapter: conjugated into the analytic convention Hl2Spectrum's fftshift
    // assumes. Fed the raw wire it drew the spectrum MIRRORED about the pan
    // centre — on 40 m that put FT8, which lives at 7.074..7.077, on screen at
    // 7.071..7.074, left of a correctly-drawn DIGU cursor.
    //
    // The FFT sees the full-rate IQ, but only when a frame is actually due.
    // Skipping the whole computation — not just the emit — is what keeps a wide
    // span affordable; see setSpectrumRateFps.
    //
    // The accumulator is fed on BOTH paths, so a skipped interval advances the
    // window rather than emptying it: whichever fftSize samples complete a frame
    // when the next one comes due are a real, contiguous, correctly-scaled
    // snapshot. The cost is that signals landing entirely between two displayed
    // frames are not seen at all, which is the accepted trade for a display-rate
    // panadapter.
    //
    // Feeding it is also what decouples the achieved rate from the span. Leaving
    // the accumulator empty between frames means every due frame first has to
    // refill from scratch, and that refill is ~9 EP6 blocks — 23.6 ms at 48 kHz
    // against 3.0 ms at 384 kHz. Added to the interval, a 25 fps request landed
    // at ~16 fps zoomed in and ~23 fps zoomed out: the rate tracked the span,
    // which is the exact coupling this shaper exists to remove. Fed, the cost is
    // bounded by one block instead (2.6 ms at 48 kHz, 0.3 ms at 384 kHz).
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

    // Audio: the RAW wire. See the note in the block loop below.
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
            // NOT conjugated. This carried a `-imag()` on the stated reasoning
            // that the HPSDR wire order is the opposite handedness to WDSP's
            // convention. Measured against WWV on live hardware, it is not: with
            // the slice shift forced to zero — the one geometry where no second
            // error can compensate — that conjugation made USB hear signals
            // BELOW the dial and LSB hear them above, by 100-300x in magnitude.
            // Removing it puts every mode on the sideband it advertises.
            //
            // It survived because it never acted alone: the shift sign in
            // Hl2Backend::setSliceFrequency was calibrated THROUGH this
            // inversion and validated in LSB (hl2_shift_test), the one mode the
            // inversion makes correct. The two did not cancel cleanly, though —
            // they left the slice mistuned by twice its offset from the NCO,
            // which is the "DIGU is ~3 kHz off" an operator sees at a 1.5 kHz
            // offset. Both halves have to come out together.
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
            // A FAULT ALSO GETS A LINE, on a bounded schedule. The counter
            // says how many; only a log line says WHEN, and when is what ties
            // a fault to the rate change or the panadapter open that caused
            // it. Powers of two so a fault recurring at the block rate cannot
            // put 47 warnings a second on the thread that also paces EP2 —
            // the first occurrence of each kind is always logged, and the
            // hundredth is not.
            //
            // Underrun is excluded: it is normal, it is frequent, and logging
            // it would drown the four that are not. It is still counted.
            //
            // AFTER processIq() RETURNS, not inside it. qCWarning formats and
            // allocates, and doing that between the two reads of
            // wdspPortAllocationSequence() would manufacture the very
            // AllocationViolation this line is reporting.
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
        // S-meter from WDSP's own signal-strength meter, NOT from the RMS of
        // the demodulated audio. Holding that audio level constant is precisely
        // what the AGC does, so an audio-RMS meter barely moves with signal
        // strength — it deflects, which is why it looked like it worked, but it
        // tracks the AGC's output target rather than the signal.
        // AVERAGE, NOT PEAK. WDSP's xmeter keeps both from the same
        // smag = I*I + Q*Q: `avg` is an EMA of power, `peak` is a peak-hold
        // that DECAYS across blocks rather than resetting per block. Both take
        // the log after averaging, so the domain is right either way -- the tap
        // is the whole difference.
        //
        // On a steady carrier the two agree exactly, because I*I + Q*Q is
        // constant for a complex exponential. They diverge only on noise and on
        // modulation, so every check against a test tone passes and the error
        // appears precisely where an operator judges a receiver: the band noise
        // floor, which a peak-hold reads roughly 11-14 dB high.
        //
        // That also makes the peak tap wrong for a dBm-labelled axis. S9 is
        // defined as -73 dBm of sine, i.e. an RMS quantity, and `avg` is the
        // mean-square -- so the average tap is what the calibration means.
        // Meter ballistics are not lost: the backend already applies its own
        // attack/decay EMA to the dBm value before publishing.
        // NOT WHILE MUTED, for the same reason the ADC peak below is not
        // sampled while muted -- and this site was the one of the two that
        // forgot. The muted branch at the top of this loop clocks the channel
        // with literal zeros on purpose, so `avg` is then measuring the silence
        // this code fed it, not the band. The mute is the TRANSMIT mute
        // (Hl2Backend queues setAudioMuted around an over), so an unguarded
        // read drops the S-meter needle to the floor on every key-down and
        // walks it back up on unkey: an artefact of our own muting, presented
        // as a signal level.
        //
        // Found by cross-checking tropo1234's #5818, which fixes exactly this
        // on the ANAN side of the same #5785 change. Their reasoning is the
        // rate-change settle window; ours is T/R. Same tap, same mute, same
        // needle.
        //
        // AND NOT FOR THE FIRST FEW BLOCKS AFTER THE MUTE RELEASES, which is
        // the other half of the same fault and the half a guard alone does not
        // close. `avg` is an EMA with a 0.100 s time constant and it kept
        // integrating this loop's zeros for the whole over; suppressing the
        // read while muted does not un-integrate them. Without the settle
        // count below, the block that arrives one instant after unkey reads
        // the silence at its full depth — ten9876 measured -224.5 dBFS against
        // a pre-mute -10.5 — and the needle dives on UNKEY rather than on
        // key-down. Same zeros, same tap, other edge.
        //
        // COUNTED HERE, not on a clock, and counted only on this path. One
        // decrement per block that WDSP actually completed is the same clock
        // xmeter() integrates on, so the window means the same thing whatever
        // the block rate is doing; a wall clock would expire early on a stalled
        // stream and publish exactly the reading it exists to withhold.
        //
        // The cost is that the held pre-transmit reading is held ~300 ms longer
        // than the mute itself. That is the honest trade: a held reading is at
        // least a reading of the band, and the alternative on offer is a
        // measurement of our own silence.
        //
        // The same gate sets the READ CADENCE (WdspSMeter::emitEveryBlocks):
        // one reading per DSP-rate block's worth of input, so the backend's
        // per-reading EMA keeps the same time constant at every sample rate
        // instead of shrinking eightfold between 48 and 384 ksps.
        if (!m_audioMuted && m_meterTap.tick()) {
            emit meterUpdate(static_cast<float>(
                m_channel->meter(WdspChannel::Meter::SignalAverage)));
        }
        // The POST-DDC half of §13 item 16's ADC pairing, sampled here because
        // this is the one instant it means something: a block has just gone
        // through, and RXA.c's adcmeter has just run on its input. Stored, not
        // emitted — the consumer is Hl2Backend::healthSnapshot(), a poll from
        // another thread, and a signal per block would be ~47 a second of
        // display traffic nobody asked for. See Hl2RxDsp.h for why the stores
        // are atomic and Hl2AdcPairing.h for what the value is and is not.
        //
        // NOT WHILE MUTED. The mute above clocks this channel with ZEROS, so
        // WDSP's adcmeter would decay toward its -400 dB floor and the health
        // row would report a dead converter for the length of every
        // transmission — a measurement of our own mute, presented as a
        // measurement of the band. The last receive reading is held instead,
        // and adcPeakObservedAgoMs() is what tells the reader it is standing
        // still — and, since the freshness gate went in, what stops
        // Hl2AdcPairing.h turning a held number into a causal sentence about
        // now. See kSliceStaleMs: on an HL2 the transmitter shares the
        // receiver's port, so the held-value window is exactly the window in
        // which a pre-DDC overload is OUR OWN carrier.
        //
        // A SENTINEL IS NOT A READING, so it does not get a timestamp either.
        // healthSnapshot() already refuses to show a sentinel as a level, but
        // the age is a separate row and a separate gate: stamping one would
        // publish "observed 30 ms ago" beside "peak: not reported", and would
        // tell Hl2AdcPairing.h the slice side is current when there is no
        // slice side. Storing both or neither keeps value and age inseparable.
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
