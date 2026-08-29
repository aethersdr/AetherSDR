#include "core/backends/hl2/Hl2RxDsp.h"

#include <QLoggingCategory>
#include <QMetaType>

#include <algorithm>
#include <cmath>

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
    // Opened WITH the operator's blanker rather than switched on afterwards, so
    // the very first block through a rebuilt chain is already blanked and the
    // stage is not re-armed (and therefore briefly deaf to impulses) on a rate
    // change. m_nbOn survives configure(); Config deliberately does not carry it.
    wc.noiseBlankerEnabled = m_nbOn;
    wc.noiseBlankerLevel = m_nbLevel;
    // Filter length. WDSP's default of 2048 is fine for a passband edge, but it
    // also sets the NARROWEST POSSIBLE NOTCH — min_notch_width is
    // 1600 / (nc/256) Hz at 48 kHz, so 2048 taps floors a notch at 200 Hz and
    // WDSP silently widens anything narrower rather than refusing it. A carrier
    // heterodyne wants ~50 Hz, which needs 8192. That is what pihpsdr runs
    // (receiver.c), and the cost is filter-delay, not CPU.
    wc.filterTaps = kRxFilterTaps;

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
    // DC blocker pole for the AUDIO rate — the blocker runs on WdspChannel's
    // output, not on its 48 kHz internal rate. Recomputed here so a rate change
    // keeps the same corner frequency instead of moving it.
    const float pole = dcBlockerPole(kDcBlockerCornerHz,
                                     static_cast<double>(config.audioSampleRateHz));
    m_dcBlockL.r = pole;
    m_dcBlockR.r = pole;
    m_dcBlockL.reset();
    m_dcBlockR.reset();
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
    // The channel was OPENED with the blanker (wc.noiseBlankerEnabled above),
    // so a successful configure() is also the point at which the request has
    // definitively landed.
    m_nbAppliedOn.store(m_nbOn, std::memory_order_relaxed);
    m_nbAppliedLevel.store(m_nbLevel, std::memory_order_relaxed);
    return true;
}

void Hl2RxDsp::setNoiseBlanker(bool on, int level)
{
    m_nbOn = on;
    m_nbLevel = std::clamp(level, 0, 100);
    if (!m_channel)
        return;   // no chain yet; configure() opens with the request
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
    // The mute path clocks the channel with ZEROS, and the noise blanker
    // triggers on a RATIO — magnitude against a running average magnitude — so
    // a transmit period of silence drags that average toward zero and the first
    // real sample afterwards looks like an enormous impulse. The blanker would
    // then gate the start of every receive period. Holding it freezes the
    // average instead; WdspChannel flushes it on release.
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
    if (m_channel)
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
    if (m_channel && !m_channel->addNotch(index, centerHz, widthHz, active))
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
        if (m_channel && !m_channel->removeNotch(index))
            return;
        m_notches.pop_back();
    }
}

void Hl2RxDsp::editNotch(int index, double centerHz, double widthHz, bool active)
{
    widthHz = std::max(widthHz, kMinNotchWidthHz);
    if (index < 0 || index >= static_cast<int>(m_notches.size()))
        return;
    if (m_channel && !m_channel->editNotch(index, centerHz, widthHz, active))
        return;   // see addNotch(): the mirror must not claim what WDSP refused
    m_notches[static_cast<std::size_t>(index)] = Notch {centerHz, widthHz, active};
}

void Hl2RxDsp::removeNotch(int index)
{
    if (index < 0 || index >= static_cast<int>(m_notches.size()))
        return;
    if (m_channel && !m_channel->removeNotch(index))
        return;   // see addNotch(): the mirror must not lose what WDSP kept
    m_notches.erase(m_notches.begin() + index);
}

void Hl2RxDsp::setNotchesEnabled(bool on)
{
    m_notchesEnabled = on;
    if (m_channel)
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
    if (m_channel)
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
        if (res != WdspChannel::ProcessResult::Ok)
            continue;   // Underrun while the pipeline fills, etc. — no output yet

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
        emit meterUpdate(static_cast<float>(
            m_channel->meter(WdspChannel::Meter::SignalPeak)));
    }

    if (consumed > 0)
        m_iqBuffer.erase(m_iqBuffer.begin(),
                         m_iqBuffer.begin() + static_cast<std::ptrdiff_t>(consumed));
}

}  // namespace AetherSDR::hl2
