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
    m_configured = false;
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
    m_configured = true;
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
    // Echo what was actually stored, not the argument — the clamp above is
    // exactly the sort of thing a readout needs to see rather than assume.
    emit micGainChanged(m_micGain);
}

double Hl2TxDsp::alcGainDb() const noexcept
{
    return 20.0 * std::log10(std::max(1e-9, m_alcGain));
}

void Hl2TxDsp::reset()
{
    // A new transmission starts from unity. It does not TRANSMIT at unity: the
    // first block that needs reduction takes it straight there, because
    // reduction in this stage is instantaneous.
    m_alcGain = 1.0;
    m_inBuffer.clear();
    // Re-arm the mid-buffer source-change warning for the next transmission.
    m_sourceChangeWarned = false;
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

void Hl2TxDsp::processAudioBlock(const std::vector<float>& mono,
                                 TxAudioSource source,
                                 const TxCoordinator::Context& context)
{
    if (!context.permitsDispatch(TxCoordinator::monotonicMs())) {
        return;
    }
    if (!m_txContext.sameContext(context)) {
        reset();
        m_txContext = context;
    }
    if (m_bandpass.empty() || mono.empty())
        return;

    // RESIDUE FROM THE PREVIOUS BLOCK IS NOT THIS BLOCK'S TO LEVEL.
    //
    // m_inBuffer keeps up to dspBlockSize-1 samples between calls, and micGain
    // below is chosen from THIS block's source and applied to all of them. That
    // cost nothing while every source shared one multiplier; now EngineGenerated
    // bypasses m_micGain, so a carried-over sample can be levelled up to 40 dB
    // away from where its own source wanted it.
    //
    // Nothing upstream should interleave two sources inside one transmission:
    // AudioEngine::startWsprPump() calls setDaxTxMode(true), which makes
    // onTxAudioReady() return early (the mic path), and feedDaxTxAudio() returns
    // early while m_wsprBeacon->isActive() (the client path). That is an
    // argument about three call sites in another class, and it is the kind of
    // argument that stops being true quietly. This makes it structural instead:
    // if the source changes mid-transmission the stale residue is dropped rather
    // than mislevelled. It costs at most dspBlockSize-1 samples (~21 ms at
    // 24 kHz) in a state that is already wrong, and nothing at all in normal
    // operation, where this branch never runs.
    if (source != m_lastSource && !m_inBuffer.empty()) {
        // Once per transmission, not once per block: this runs on the DSP
        // worker, and a sustained interleave would alternate every block.
        if (!m_sourceChangeWarned) {
            m_sourceChangeWarned = true;
            qWarning() << "Hl2TxDsp: transmit audio source changed mid-buffer ("
                       << static_cast<int>(m_lastSource) << "->"
                       << static_cast<int>(source) << "); dropping"
                       << m_inBuffer.size()
                       << "carried samples rather than levelling them as the"
                          " new source. Two producers are feeding one"
                          " transmission.";
        }
        m_inBuffer.clear();
    }
    m_lastSource = source;

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
    float postAlcPeak = 0.0f;

    // ---- ALC: protection only — it may reduce, never add ----
    //
    // Peak-tracking with a fast attack and a slow release, which is the
    // conventional shape: catch the onset of a syllable, but do not pump
    // audibly between words. The ceiling is UNITY, on every path, and there is
    // no configuration field that can raise it. The result is hard-limited
    // below full scale afterwards, because an ALC that can overshoot is a
    // splatter generator.
    //
    // WHAT THIS STAGE STOPPED BEING. It used to carry up to 40 dB of upward
    // makeup on the mic path, with an absolute hold threshold (-45 dBFS) below
    // which it stopped lifting. Both halves are gone, and the second is why
    // the first could not simply be turned down to 0 dB.
    //
    // The makeup half is gone because a level-dependent makeup stage cannot
    // tell a voice from a room. Measured on this radio: 20.5 dB of
    // speech-to-floor separation went in and 0.33 dB came out, because the
    // hold threshold sat below the room noise, so between words the loop went
    // on lifting until the fan and the mic hiss reached the same target peak
    // as the speech. The 20-30 dB gap between a microphone and full modulation
    // is real and still has to be closed; the operator's mic gain closes it
    // now, which is why Hl2TxLevelPolicy.h reaches +40 dB rather than +20.
    // WDSP draws the same line: create_txa() in
    // third_party/wdsp/upstream/TXA.c builds its `alc` with run=1 and
    // max_gain=1.0 and its `leveler` with run=0 and max_gain=1.778 (+5 dB).
    //
    // THE HOLD HAD TO GO WITH IT, not merely lose its `!clientLeveled` term.
    // Under a unity ceiling a quiet block wants target = 1.0. If an earlier
    // loud block left the gain below unity then target > m_alcGain, `reducing`
    // is false, the block is under the threshold, and the move back to unity is
    // suppressed — the transmission strands at whatever reduction its loudest
    // block called for. That is #4796's own defect class mirrored onto the mic
    // path: exactly the failure the `!clientLeveled` term was added to keep off
    // the client path, handed to the mic path instead. A hold is only coherent
    // when there is makeup gain to hold back.
    //
    // What remains is the half that was never the bug. m_micGain now reaches
    // 100x (+40 dB, Hl2TxLevelPolicy.h) and is applied BEFORE this block, so a
    // full-scale source with the TX gain slider up arrives far inside the hard
    // clamp below — and flat-topping an SSB modulator input is a splatter
    // generator, the one failure mode here that harms other operators rather
    // than the operator who caused it. The clamp is a backstop, not a level
    // control; it must not become the only thing standing between a hot source
    // and the band.
    //
    // Below alcTargetPeak the ceiling binds, the gain sits at exactly 1.0, and
    // output is proportional to input over the whole reported range — the
    // property hl2_txdsp_test now asserts directly, as 20 dB in arriving as
    // 20 dB out. Above the target the loop takes the reduction IMMEDIATELY —
    // see the note at the assignment below for why the 5 ms attack constant
    // this sentence used to name was deleted in 5607b565 (#5646) rather than
    // shortened.
    //
    // THE MIC SLIDER IS A MICROPHONE CONTROL, so it does not reach the engine's
    // own unattended audio. See the note on the declaration in Hl2TxDsp.h.
    //
    // Decided ONCE per block and used by both loops below, so the level the ALC
    // measures and the level that reaches the modulator cannot disagree — they
    // are the same number by construction rather than by two matching edits.
    const double micGain =
        (source == TxAudioSource::EngineGenerated) ? 1.0 : m_micGain;

    if (m_config.alcEnabled) {
        float blockPeak = 0.0f;
        for (std::size_t s = 0; s < consumed; ++s)
            blockPeak = std::max(blockPeak, std::fabs(
                static_cast<float>(m_inBuffer[s] * micGain)));

        if (blockPeak > 1e-6f) {
            const double wanted = m_config.alcTargetPeak / blockPeak;
            // The unity ceiling, and the whole of what this stage promises.
            const double target = std::min(wanted, 1.0);
            const double blockSec = static_cast<double>(consumed)
                                  / static_cast<double>(m_config.inputSampleRateHz);
            // REDUCTION IS INSTANTANEOUS. Only the release is smoothed.
            //
            // This is the shape a splatter guard has to have, and the shape a
            // smoothed attack cannot deliver at this block size. The attack
            // constant was 5 ms against a 512-sample block on 24 kHz — 21.3 ms
            // — so `1 - exp(-21.3/5)` already closed 98.6% of the error in a
            // single block. It was not buying smoothing. It was leaving 1.4%
            // of whatever step had just arrived sitting above the modulator's
            // hard clamp, and 1.4% of 40 dB is not small.
            //
            // A one-shot key-on seed was tried first and covers only the FIRST
            // reduction of an over. Measured on this class: a source that
            // crosses the ALC target gently — an ordinary quiet word — spends
            // the seed on a fraction of a dB, and the next loud syllable is
            // then unprotected. A -38 dBFS word followed by a -12 dBFS
            // syllable at slider 100 reached |IQ| 1.0768 with 223 samples
            // clipped; a quiet passage, one loud burst and quiet again reached
            // 1.5045 with 727 (15.2 ms). Both are speech, not corner cases.
            // With reduction instantaneous every one of those shapes settles
            // at 0.859 with nothing at the clamp.
            //
            // A short enough attack constant would also keep the clamp
            // clear at TODAY'S block size — 0.5 ms does. That is the argument
            // for instantaneous rather than against it: whether a constant is
            // short enough depends on dspBlockSize and inputSampleRateHz, so
            // it is a guarantee that expires silently the day either moves.
            // Reduction that simply takes the target has no such dependency.
            //
            // The release keeps its slow constant, which is the half that
            // actually needs smoothing: a limiter that releases as fast as it
            // attacks pumps between words.
            const bool reducing = target < m_alcGain;
            if (reducing) {
                m_alcGain = target;
            } else {
                const double a = 1.0 - std::exp(-blockSec
                                    / std::max(1e-6, m_config.alcReleaseSec));
                m_alcGain += a * (target - m_alcGain);
            }
        }
    } else {
        // ALC configured off: unity, and the clamp below is then the only
        // over-level backstop there is. Pre-existing behaviour for an operator
        // who has turned the ALC off, and the one remaining way to reach the
        // clamp without the smooth limiting in front of it.
        m_alcGain = 1.0;
    }
    // Published unconditionally, including when the ALC is off and the answer
    // is a flat 0 dB. The TX:ALCGAIN meter is fed from this, and a meter that
    // stops updating reads as a stuck needle rather than as "no gain is being
    // applied" — the same reason the mic peak below is not gated either.
    //
    // TX:ALC is NOT fed from here: it is the post-ALC level, published from
    // alcPeak below. The two meters are the two halves of this stage and they
    // move in opposite directions, which is why they are separate keys rather
    // than one gauge that changes meaning.
    emit alcGain(static_cast<float>(alcGainDb()));

    for (std::size_t s = 0; s < consumed; ++s) {
        // Mic peak is measured BEFORE the ALC, deliberately.
        //
        // A post-ALC meter sits pinned near the target by definition and tells
        // the operator nothing — it reports the ALC's success, not their input
        // level. What a mic-gain control acts on is this, and how hard the ALC
        // is working is reported separately as alcGain(), which is published
        // to the model and diagnostic surfaces as TX:ALCGAIN.
        const float preAlc = static_cast<float>(m_inBuffer[s] * micGain);
        peak = std::max(peak, std::fabs(preAlc));

        // Hard limit AFTER the ALC. The ALC is a smoothed estimate and will
        // overshoot on a transient; letting that through would transmit
        // distortion across the band rather than merely clipping our own audio.
        const float in = std::clamp(static_cast<float>(preAlc * m_alcGain),
                                    -1.0f, 1.0f);
        postAlcPeak = std::max(postAlcPeak, std::fabs(in));

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
        emit iqReady(m_iq, context);
    // PRE-modulation level: this is what a mic-gain control acts on, so it is
    // the number that tells an operator whether they are overdriving.
    emit micPeak(peak > 0.0f ? 20.0f * std::log10(peak) : -140.0f);
    // POST-ALC level, which is a different question and needs its own meter:
    // how close to full modulation the signal reaching the wire actually is.
    emit alcPeak(postAlcPeak > 0.0f ? 20.0f * std::log10(postAlcPeak) : -140.0f);
}

}  // namespace AetherSDR::hl2
