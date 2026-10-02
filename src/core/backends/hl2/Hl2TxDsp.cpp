#include "core/backends/hl2/Hl2TxDsp.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <QDebug>
#include <QLoggingCategory>

// Same category string as Hl2Backend.cpp's lcHl2Tx, so one switch enables the
// modulator and the backend together.
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

// AETHER_HL2_TX_TXA selects one implementation at compile time; the other is
// not in the binary (#5678). TXA needs only SetTXAMode and SetTXABandpassFreqs
// (applyModeAndFilter()) on top of create_txa's defaults, plus a paced caller.
// The first ~48.7 ms is the create_slews mute ramp and fill.
// Opposite-sideband suppression from unit-test IQ (TXA figures are lower bounds):
//     tone    mode    phasing     TXA
//     150 Hz  DIGU    22.06 dB    >= 180.6 dB
//       1 kHz  USB    87.15 dB    >= 297.2 dB
// EP2 packs signed 16-bit I/Q (ep2WriteTxIq, ~96 dB floor), so the real gain
// is the 150 Hz low edge of DIGU/DIGL (WSJT-X modes).

#if AETHER_HL2_TX_TXA

bool Hl2TxDsp::buildModulator(std::string* error)
{
    m_channel.reset();
    m_modulatorRunning = false;
    m_txBlocks = 0;
    m_txFaultBlocks = 0;

    // One input block of dspBlockSize audio samples is m_upsample times that
    // at the DSP rate, so the channel consumes exactly one input block per
    // pass (mirror of Hl2RxDsp::configure).
    WdspChannel::Config c;
    c.direction = WdspChannel::Direction::Transmit;
    c.inputSampleRate = m_config.inputSampleRateHz;
    c.dspSampleRate = m_config.outputSampleRateHz;
    c.outputSampleRate = m_config.outputSampleRateHz;
    c.inputBlockSize = static_cast<std::size_t>(m_config.dspBlockSize);
    c.dspBlockSize = c.inputBlockSize * static_cast<std::size_t>(m_upsample);
    c.mode = m_config.mode;
    // blockForOutput = false (the default, and what the figures above were
    // measured at). Setting it true would block the audio I/O thread and make
    // fexchange2's underrun report unreachable; fix underruns by pacing.
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
        // Reported to the GUI via Hl2Backend::beginDspSetup's txOk/txErr.
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
    // Both calls, every time: SetTXAMode does not pick an SSB sideband (the
    // passband sign does), so a mode-only USB->LSB change would stay on USB.
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

    // Started on the first block of an over (reset() is the only transition
    // this stage sees); WDSP's mute envelope ramps up from there.
    if (!m_modulatorRunning) {
        if (!m_channel->setRunning(true)) {
            qCWarning(lcTxMod) << "HL2 TXA modulator: start refused; this over "
                                  "will not reach the wire";
            return;
        }
        m_modulatorRunning = true;
    }

    for (std::size_t off = 0; off + block <= audio.size(); off += block) {
        // Mono audio in I, zeros in Q: xpanel inselect = 2 zeroes Q (measured).
        const WdspChannel::ProcessResult r =
            m_channel->processIq(audio.subspan(off, block), m_zeroQ,
                                 m_outI, m_outQ);
        ++m_txBlocks;
        if (r != WdspChannel::ProcessResult::Ok) {
            ++m_txFaultBlocks;
            // Rate-limited, but the first fault is always logged.
            if (m_txFaultBlocks == 1 || m_txFaultBlocks % 64 == 0) {
                qCWarning(lcTxMod).nospace()
                    << "HL2 TXA modulator: block not placed on the wire ("
                    << (r == WdspChannel::ProcessResult::Underrun
                            ? "underrun" : "engine refused")
                    << "), " << m_txFaultBlocks << " of " << m_txBlocks
                    << " blocks so far. The caller is feeding faster than the "
                       "channel can drain, or the channel has stalled.";
            }
            // Dropped, not zero-filled: an underrun already slips the output
            // by a whole DSP buffer, so those samples are gone either way.
            continue;
        }
        for (std::size_t k = 0; k < outBlock; ++k) {
            // No conjugation (opposite of the phasing build): the signed
            // passband already gives the HPSDR wire's handedness. Adding -imag()
            // here would put every SSB mode on the wrong sideband.
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

        // Quadrature filter = imaginary part of the SAME analytic bandpass, not
        // a wideband Hilbert (2/(pi*k)): that is all-pass in magnitude, so
        // out-of-band audio reaches Q only and goes out double-sideband
        // (measured: 5 kHz tone vs 2700 Hz filter at +/-5 kHz, 6 dB down). One
        // prototype ha[k] = (exp(j*2*pi*hi*k) - exp(j*2*pi*lo*k)) / (j*2*pi*k)
        // gives I and Q the same passband and group delay.
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

            // Conjugate for the wire: HPSDR wire order has the opposite
            // handedness to the analytic convention (RX conjugates too).
            // Without it TX goes out on the wrong sideband, invisibly to our
            // own panadapter. A TXA channel must NOT do this.
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
    // The TXA build can be refused a channel here (pool of 32 shared with the
    // receivers); m_configured then stays false.
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
    // A control call on the open channel, not a rebuild (that would be FFTW
    // planning on the I/O thread). A running channel keeps producing across
    // it, and an inverted pair is refused.
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

    // Residue carried from the previous call is not this block's to level:
    // micGain below follows THIS block's source, and EngineGenerated bypasses
    // m_micGain (up to 40 dB apart). AudioEngine should never interleave
    // sources in one transmission (startWsprPump sets DAX TX mode; feedDaxTxAudio
    // returns while the beacon is active), but if it does, the residue
    // (<= dspBlockSize-1 samples, ~21 ms) is dropped rather than mislevelled.
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

    // ALC, protection only: peak tracking, instantaneous reduction, smoothed
    // release, unity ceiling on every path, then a hard limit below full scale.
    // No makeup gain and no hold: the operator's mic gain (+40 dB,
    // Hl2TxLevelPolicy.h) closes the speech gap, as WDSP's create_txa runs
    // `alc` at max_gain 1.0 with `leveler` off. Below alcTargetPeak output is
    // proportional to input. micGain (not applied to EngineGenerated) is chosen
    // once per block so the ALC measures what the modulator receives.
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
            // Reduction is instantaneous; only the release is smoothed. Any
            // attack constant leaves part of a step above the hard clamp, and
            // whether it is short enough depends on dspBlockSize and the input
            // rate. hl2_txdsp_test pins speech shapes settling at 0.859 with
            // nothing clipped. The slow release keeps it from pumping.
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
        // ALC off: unity, and the hard clamp is the only over-level backstop.
        m_alcGain = 1.0;
    }
    // Published unconditionally (even a flat 0 dB) so TX:ALCGAIN never looks
    // stuck. TX:ALC is the post-ALC level, from alcPeak below.
    emit alcGain(static_cast<float>(alcGainDb()));

    for (std::size_t s = 0; s < consumed; ++s) {
        // Mic peak is pre-ALC: a post-ALC meter would sit at the target. ALC
        // effort is reported separately (TX:ALCGAIN).
        const float preAlc = static_cast<float>(m_inBuffer[s] * micGain);
        peak = std::max(peak, std::fabs(preAlc));

        // Hard limit after the ALC. With the ALC on, instantaneous reduction
        // keeps the level at or below alcTargetPeak; this clamp is the backstop
        // for the ALC-off path.
        const float in = std::clamp(static_cast<float>(preAlc * m_alcGain),
                                    -1.0f, 1.0f);
        postAlcPeak = std::max(postAlcPeak, std::fabs(in));
        m_levelled[s] = in;
    }

    // Everything above is the level chain (mic gain, ALC, hard clamp, meters),
    // shared by both builds; the modulator below is the only thing
    // AETHER_HL2_TX_TXA selects.
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
