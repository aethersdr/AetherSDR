#include "core/backends/hl2/Hl2TxDsp.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <QDebug>
#include <QLoggingCategory>

// Same category string as Hl2Backend.cpp's lcHl2Tx, a different C++ symbol.
// One name for "the HL2 transmit path" so an operator turning it on gets the
// modulator and the backend together, which is the pair a transmit bug report
// needs.
static Q_LOGGING_CATEGORY(lcTxMod, "aether.hl2.tx")

namespace AetherSDR::hl2 {

#if !AETHER_HL2_TX_TXA
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
#endif

Hl2TxDsp::Hl2TxDsp(QObject* parent) : QObject(parent) {}
Hl2TxDsp::~Hl2TxDsp() = default;

// ── WHICH MODULATOR, AND WHY IT IS A BUILD FLAG ─────────────────────────────
//
// AETHER_HL2_TX_TXA selects one of the two implementations below AT COMPILE
// TIME. The other one is not in the binary. That is the point, and it is the
// condition #5678 was approved on: a compile flag gives a way back without
// giving an operator a way to be on the wrong one, because two silent transmit
// paths are worse than one.
//
// WHAT THE PHASING MODULATOR WAS FOR, stated fairly, because it was right at
// the time. WDSP's transmit path always WORKED -- wdsp_channel_test drove a TXA
// channel and got IQ out of it -- so "WDSP TX is broken" was never the claim.
// The claim was narrower: driven from THIS backend's configuration a TXA
// channel returned Underrun on most blocks and zeros on the rest, and the
// failure mode was SILENT. Fifty lines of arithmetic whose correctness is a
// number a test can print beat a canonical chain that can fail without saying
// so. Two speculative attempts at the missing initialisation sequence made
// things worse. Choosing the measurable thing was the right trade.
//
// WHAT CHANGED IS THAT BOTH HALVES OF THAT FAILURE ARE NOW EXPLAINED, and
// neither was a missing call:
//
//   * The UNDERRUNS were caller CADENCE, not configuration. Unpaced, 240-255
//     of 256 blocks underrun; at 5 ms and at the live 21.33 ms block period,
//     64 of 64 are Ok including the first. See processAudioBlock's note.
//   * The ZEROS were the priming stretch -- create_slews' ndelup + ntup = 840
//     input samples of mute ramp, plus the rest of the fill, bounded at about
//     48.7 ms -- sampled through a starved reader that walked it one block at a
//     time over hundreds of calls, which is what made a bounded transient look
//     permanent. Audio placed in Q is a separate, real and permanently silent
//     fault (xpanel's inselect = 2), and it is NOT what the note recorded,
//     because it underruns nothing.
//
// The configuration it actually needs is TWO CALLS on top of create_txa's
// defaults -- SetTXAMode and SetTXABandpassFreqs -- which applyModeAndFilter()
// makes and which WdspChannel already wraps.
//
// AND WHAT THE MIGRATION IS WORTH, narrowly, because the wide claim is wrong.
// Measured on the same instrument, at the live geometry, in the same units:
//
//     tone    mode    phasing modulator      TXA
//     150 Hz  DIGU          22.06 dB      >= 180.6 dB
//       1 kHz  USB          87.15 dB      >= 297.2 dB
//
// The 1 kHz column is worth NOTHING. MetisProtocol.cpp's ep2WriteTxIq packs I
// and Q as signed 16-bit, so EP2 quantises the transmit stream at roughly 96 dB
// before a sample reaches the radio: the incumbent's 87 dB is already at the
// wire's own floor and TXA's advantage there is below the wire and unusable.
//
// THE ENTIRE RETURN IS THE LOW EDGE -- 22 dB against >= 180 dB at 150 Hz, on
// the {150, 3000} passband, in DIGU and DIGL. That is the modes WSJT-X
// transmits in and nothing else. Seventy decibels the wire could carry and 255
// taps do not fill.
//
// Every TXA figure above is a LOWER BOUND, not a value: the image bin sits at
// the measuring instrument's own numerical floor, established by a halving
// probe rather than assumed. And every one of them was read off a unit test's
// emitted IQ. No radio was keyed for any of it.

#if AETHER_HL2_TX_TXA

bool Hl2TxDsp::buildModulator(std::string* error)
{
    m_channel.reset();
    m_modulatorRunning = false;
    m_txBlocks = 0;
    m_txFaultBlocks = 0;

    // The LIVE geometry, and it is arithmetic rather than a table: WDSP's
    // three-rate channel model wants the DSP block expressed in DSP-rate
    // samples, so one input block of Config::dspBlockSize audio samples is
    // m_upsample times that many at the DSP rate and the channel consumes
    // exactly one input block per pass. That is the mirror of what
    // Hl2RxDsp::configure does for receive.
    WdspChannel::Config c;
    c.direction = WdspChannel::Direction::Transmit;
    c.inputSampleRate = m_config.inputSampleRateHz;
    c.dspSampleRate = m_config.outputSampleRateHz;
    c.outputSampleRate = m_config.outputSampleRateHz;
    c.inputBlockSize = static_cast<std::size_t>(m_config.dspBlockSize);
    c.dspBlockSize = c.inputBlockSize * static_cast<std::size_t>(m_upsample);
    c.mode = m_config.mode;
    // blockForOutput = false is WdspChannel::Config's default and is the
    // setting every figure quoted above was measured at. Do not flip it to
    // silence an underrun: with it set, fexchange2's `*error += -2` branch is
    // structurally unreachable, so the stage would stop REPORTING starvation
    // rather than stop being starved, and it would do that by blocking the
    // audio I/O thread. The fix for an underrun is the caller's cadence.
    c.blockForOutput = false;

    // Signed, from the mode. See applyModeAndFilter().
    const double lo = std::min(std::abs(m_config.filterLowHz),
                               std::abs(m_config.filterHighHz));
    const double hi = std::max(std::abs(m_config.filterLowHz),
                               std::abs(m_config.filterHighHz));
    c.filterLowHz = isLowerSideband() ? -hi : lo;
    c.filterHighHz = isLowerSideband() ? -lo : hi;

    std::string err;
    m_channel = WdspChannel::create(c, &err);
    if (!m_channel) {
        // A REFUSAL, reported. Hl2Backend::beginDspSetup already carries a
        // txOk/txErr pair back to the GUI thread for exactly this, and it was
        // dead weight until now because the phasing modulator could only fail
        // its own argument validation.
        if (error) {
            *error = err.empty() ? "WDSP transmit channel refused" : err;
        }
        qCWarning(lcTxMod) << "HL2 TXA modulator: channel refused:"
                           << QString::fromStdString(err);
        return false;
    }

    m_zeroQ.assign(c.inputBlockSize, 0.0f);
    m_outI.assign(m_channel->outputBlockSize(), 0.0f);
    m_outQ.assign(m_channel->outputBlockSize(), 0.0f);

    qCInfo(lcTxMod).nospace()
        << "HL2 TXA modulator: channel " << m_channel->channelIdForTest()
        << " open, " << c.inputSampleRate << " Hz in / " << c.dspSampleRate
        << " Hz dsp, blocks " << static_cast<int>(c.inputBlockSize) << "/"
        << static_cast<int>(c.dspBlockSize) << ", passband "
        << c.filterLowHz << ".." << c.filterHighHz << " Hz";
    return true;
}

void Hl2TxDsp::applyModeAndFilter()
{
    if (!m_channel) {
        return;
    }
    // BOTH calls, every time, and the order does not matter -- but leaving
    // either out does. SetTXAMode alone does NOT choose a sideband for SSB:
    // TXASetupBPFilters handles TXA_LSB and TXA_USB with the identical
    // CalcBandpassFilter call, so the sideband rides entirely on the SIGN of
    // the passband, exactly as it does in RXA. A mode change from USB to LSB
    // that pushed only the mode would leave the transmitter on the upper
    // sideband with the mode readout saying LSB -- the same class of fault as
    // the missing conjugation, and just as invisible from inside this
    // application, because the panadapter reads the same wire order.
    const double lo = std::min(std::abs(m_config.filterLowHz),
                               std::abs(m_config.filterHighHz));
    const double hi = std::max(std::abs(m_config.filterLowHz),
                               std::abs(m_config.filterHighHz));
    const double lowHz = isLowerSideband() ? -hi : lo;
    const double highHz = isLowerSideband() ? -lo : hi;

    if (!m_channel->setMode(m_config.mode)) {
        qCWarning(lcTxMod) << "HL2 TXA modulator: mode change refused";
    }
    if (!m_channel->setFilter(lowHz, highHz)) {
        qCWarning(lcTxMod) << "HL2 TXA modulator: passband change refused,"
                           << lowHz << ".." << highHz << "Hz";
    }
}

void Hl2TxDsp::resetModulatorState()
{
    if (!m_channel) {
        return;
    }
    // HL2 stops supplying audio on unkey. setRunning(false) only schedules a
    // fade/flush, which cannot complete without further processIq calls. A
    // restart cancels that pending fade and would replay the previous over.
    // Discard under the channel's control fence before another context can emit.
    if (!m_channel->discardTransmitData()) {
        m_configured = false;
        qCWarning(lcTxMod) << "HL2 TXA modulator: discard refused; transmit disabled until reconfigured";
    }
    m_modulatorRunning = false;
}

void Hl2TxDsp::modulate(std::span<const float> audio)
{
    if (!m_channel) {
        return;
    }
    const std::size_t block = m_zeroQ.size();
    const std::size_t outBlock = m_outI.size();
    if (block == 0 || outBlock == 0) {
        return;
    }

    // Started HERE rather than on a key-down signal, because this stage has no
    // key-down edge of its own -- reset() is the only transition it is told
    // about. The first block of an over starts the channel and runs the mute
    // envelope UP, which is the T/R shaping WDSP already owns.
    if (!m_modulatorRunning) {
        if (!m_channel->setRunning(true)) {
            qCWarning(lcTxMod) << "HL2 TXA modulator: start refused; this over "
                                  "will not reach the wire";
            return;
        }
        m_modulatorRunning = true;
    }

    for (std::size_t off = 0; off + block <= audio.size(); off += block) {
        // MONO AUDIO IN I, ZEROS IN Q. Not a convenience: xpanel runs with
        // inselect = 2 and multiplies Q by zero, which was measured rather than
        // read -- a tone fed in Q alone produces bit-exact zeros on 200 of 200
        // blocks with no error and no underrun. That is the silent fault this
        // arrangement avoids by construction.
        const WdspChannel::ProcessResult r =
            m_channel->processIq(audio.subspan(off, block), m_zeroQ,
                                 m_outI, m_outQ);
        ++m_txBlocks;
        if (r != WdspChannel::ProcessResult::Ok) {
            ++m_txFaultBlocks;
            // LOUD, and rate-limited so a starved caller does not drown the
            // log it needs to read. The first one is always printed: the prior
            // TXA attempt's whole defect was that this moment said nothing.
            if (m_txFaultBlocks == 1 || m_txFaultBlocks % 64 == 0) {
                qCWarning(lcTxMod).nospace()
                    << "HL2 TXA modulator: block not placed on the wire ("
                    << (r == WdspChannel::ProcessResult::Underrun
                            ? "underrun" : "engine refused")
                    << "), " << m_txFaultBlocks << " of " << m_txBlocks
                    << " blocks so far. The caller is feeding faster than the "
                       "channel can drain, or the channel has stalled.";
            }
            // DROPPED, not zero-filled. A block of zeros is what the original
            // silent failure looked like from the outside, and it would leave
            // the emitted stream sample-count-correct and phase-wrong -- an
            // underrun already slips the channel's output by a whole DSP buffer
            // permanently, so the samples this block WOULD have carried are
            // gone whatever we emit in their place.
            continue;
        }
        for (std::size_t k = 0; k < outBlock; ++k) {
            // NO CONJUGATION, and that is the opposite of the phasing build.
            // fir_bandpass builds exp(-j*w_osc*pos), so the signed passband
            // applied in applyModeAndFilter() already selects the half that
            // gives the HPSDR wire's handedness. Adding the phasing modulator's
            // -imag() here would transmit every SSB mode on the wrong sideband,
            // which is the recommendation the S6 study made and the mutation
            // that falsified it: flipping LSB's passband sign back to positive
            // inverts every in-band row, 167.31 dB becoming -175.64 dB.
            m_iq.emplace_back(m_outI[k], m_outQ[k]);
        }
    }
}

const char* Hl2TxDsp::modulatorName() noexcept { return "wdsp-txa"; }

int Hl2TxDsp::wdspChannelId() const noexcept
{
    return m_channel ? m_channel->channelIdForTest() : -1;
}

const WdspChannel::Config* Hl2TxDsp::channelConfig() const noexcept
{
    return m_channel ? &m_channel->config() : nullptr;
}

unsigned long long Hl2TxDsp::modulatorFaultBlocks() const noexcept
{
    return m_txFaultBlocks;
}

unsigned long long Hl2TxDsp::modulatorBlocks() const noexcept
{
    return m_txBlocks;
}

#else   // !AETHER_HL2_TX_TXA — the in-tree phasing modulator, the way back

// A windowed-sinc bandpass and a matching quadrature filter, sharing one delay
// line. Fifty lines, no hidden state, and its correctness is a number
// hl2_txdsp_test measures directly.
bool Hl2TxDsp::buildModulator(std::string* error)
{
    (void)error;
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
    return true;
}

void Hl2TxDsp::applyModeAndFilter()
{
    // The mode is read per block by isLowerSideband() and the passband is baked
    // into the kernels, so a passband change is a rebuild and a mode change is
    // nothing at all. configure()/setFilter() call buildModulator() directly.
}

void Hl2TxDsp::resetModulatorState()
{
    std::fill(m_hist.begin(), m_hist.end(), 0.0f);
    m_histPos = 0;
}

void Hl2TxDsp::modulate(std::span<const float> audio)
{
    const std::size_t N = m_bandpass.size();
    if (N == 0) {
        return;
    }
    const bool lsb = isLowerSideband();

    for (const float in : audio) {
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
            //
            // A TXA CHANNEL MUST NOT HAVE THIS. See the TXA build above.
            m_iq.emplace_back(bi, -q);
        }
    }
}

const char* Hl2TxDsp::modulatorName() noexcept { return "phasing"; }
int Hl2TxDsp::wdspChannelId() const noexcept { return -1; }
const WdspChannel::Config* Hl2TxDsp::channelConfig() const noexcept { return nullptr; }
// Arithmetic cannot starve. Always zero, so a health snapshot reports the same
// field in both builds rather than omitting it in one.
unsigned long long Hl2TxDsp::modulatorFaultBlocks() const noexcept { return 0; }
unsigned long long Hl2TxDsp::modulatorBlocks() const noexcept { return 0; }

#endif  // AETHER_HL2_TX_TXA

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
    m_inBuffer.clear();
    // The modulator may REFUSE now, which it could not before: the TXA build
    // opens a WDSP channel here and a channel can be refused (the pool is 32
    // wide and shared with every receiver). Leaving m_configured false on that
    // path is what makes the readback honest -- Hl2Backend already carries the
    // txOk/txErr pair back for it.
    if (!buildModulator(error)) {
        return false;
    }
    m_configured = true;
    return true;
}

void Hl2TxDsp::setMode(WdspChannel::Mode mode)
{
    m_config.mode = mode;
    // In the TXA build this is NOT a no-op and must not become one: the
    // sideband rides on the sign of the passband, so a mode change has to
    // re-push the passband too. applyModeAndFilter() does both.
    applyModeAndFilter();
}

void Hl2TxDsp::setFilter(double lowHz, double highHz)
{
    m_config.filterLowHz = lowHz;
    m_config.filterHighHz = highHz;
#if AETHER_HL2_TX_TXA
    // A control call on the open channel, not a rebuild: SetTXABandpassFreqs is
    // cheap and a rebuild would be FFTW planning on the I/O thread. The census
    // shows a running channel keeps producing across an accepted setFilter
    // (0.0705 RMS before, 0.0709 after) and REFUSES an inverted pair rather
    // than going silent.
    applyModeAndFilter();
#else
    buildModulator(nullptr);
#endif
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
    // The modulator's own state, per build. In the phasing build this is the
    // delay line; in the TXA build it deliberately does NOT rebuild the
    // channel. See resetModulatorState().
    resetModulatorState();
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
    // Readiness is independent of the selected modulator.
    if (!m_configured || mono.empty())
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

    m_iq.clear();
    m_iq.reserve(consumed * static_cast<std::size_t>(m_upsample));
    if (m_levelled.size() < consumed)
        m_levelled.resize(consumed);
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
        m_levelled[s] = in;
    }

    // ── AND HERE IS THE ONLY LINE THE BUILD FLAG MOVES ─────────────────────
    //
    // Everything above this point is the LEVEL chain and is identical in both
    // builds: mic gain, the ALC and its client-leveled ceiling, the hold, the
    // hard clamp and all three meters. Everything below is emission. The
    // modulator is the single step between them, and it is the only thing
    // AETHER_HL2_TX_TXA selects.
    //
    // That cut is deliberate and it is narrower than "the transmit path". The
    // level chain is what #4796, #5646 and #5647 are about, it is pinned by
    // most of hl2_txdsp_test, and duplicating it per modulator would be two
    // level policies to keep in step -- which is the same failure shape as two
    // runtime transmit paths, one level down.
    modulate(std::span<const float>(m_levelled.data(), consumed));

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
