#include "core/backends/sim/NoiseMixer.h"

#include <QFile>
#include <QtGlobal>   // qWarning

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {
constexpr double kTwoPi     = 6.283185307179586;
constexpr double kNoiseRef  = 0.18;   // unity-reference RMS-ish amplitude (summing headroom)

double db2lin(double db) { return std::pow(10.0, db / 20.0); }

// Power-sum two dBm contributions into one bin (so overlapping sources add).
double addDb(double a, double x)
{
    const double hi = std::max(a, x);
    const double lo = std::min(a, x);
    return hi + 10.0 * std::log10(1.0 + std::pow(10.0, (lo - hi) / 10.0));
}
}  // namespace

NoiseMixer::NoiseMixer()
{
    // Defaults mirror flex-sim's channel table (level + per-channel knobs).
    m_ch[Channel::Cw]         = {false, -16.0, 700.0, 0.0, 60.0, 0.0};
    m_ch[Channel::Voice]      = {false, -14.0, 0.0,   0.0, 60.0, 0.0};
    m_ch[Channel::White]      = {false, -26.0, 0.0,   0.0, 60.0, 0.0};
    m_ch[Channel::Pink]       = {false, -24.0, 0.0,   0.0, 60.0, 0.0};
    m_ch[Channel::Qrn]        = {false, -18.0, 0.0,   12.0, 60.0, 0.0};
    m_ch[Channel::Powerline]  = {false, -22.0, 0.0,   0.0, 60.0, 0.0};
    m_ch[Channel::Crashes]    = {false, -16.0, 0.0,   0.4,  60.0, 0.0};
    m_ch[Channel::Birdie]     = {false, -28.0, 1000.0, 0.0, 60.0, 0.0};
    m_ch[Channel::Hash]       = {false, -24.0, 0.0,   0.0, 60.0, 120.0};
    m_ch[Channel::Woodpecker] = {false, -22.0, 0.0,   0.0, 60.0, 10.0};
    m_pinkRows.fill(0.0);
}

// ---- RNG: a small SplitMix64, then Box-Muller for Gaussian --------------------
double NoiseMixer::nextUniform()
{
    m_rng += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = m_rng;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    // 53-bit mantissa -> [0,1)
    return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
}

// Same SplitMix64, but on the DISPLAY stream so drawing a spectrum row cannot
// advance the audio generators' sequence (see the header for why that mattered).
double NoiseMixer::nextDisplayUniform()
{
    m_displayRng += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = m_displayRng;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
}

double NoiseMixer::nextGauss()
{
    if (m_haveSpare) { m_haveSpare = false; return m_spare; }
    double u1, u2, s;
    do {
        u1 = 2.0 * nextUniform() - 1.0;
        u2 = 2.0 * nextUniform() - 1.0;
        s = u1 * u1 + u2 * u2;
    } while (s >= 1.0 || s == 0.0);
    const double m = std::sqrt(-2.0 * std::log(s) / s);
    m_spare = u2 * m;
    m_haveSpare = true;
    return u1 * m;
}

// ---- channel control ----------------------------------------------------------
void NoiseMixer::setEnabled(Channel c, bool on) { m_ch[c].enabled = on; }
void NoiseMixer::setLevelDb(Channel c, double db) { m_ch[c].levelDb = db; }
void NoiseMixer::setKnob(Channel c, const QString& knob, double v)
{
    ChannelState& s = m_ch[c];
    if (knob == QLatin1String("hz"))   s.hz = v;
    else if (knob == QLatin1String("rate")) s.rate = v;
    else if (knob == QLatin1String("freq")) s.freq = v;
    else if (knob == QLatin1String("prf"))  s.prf = v;
}
bool NoiseMixer::anyEnabled() const
{
    for (const auto& kv : m_ch) if (kv.second.enabled) return true;
    return false;
}
const NoiseMixer::ChannelState& NoiseMixer::channel(Channel c) const
{
    return m_ch.at(c);
}

// ---- notches (TNF + ANF) ------------------------------------------------------
void NoiseMixer::setNotches(const std::vector<Notch>& notches)
{
    m_notchHz.clear();
    m_notchWidthHz.clear();
    for (const Notch& n : notches) {
        if (std::abs(n.hz) < kSampleRate / 2.0) {
            m_notchHz.push_back(n.hz);
            m_notchWidthHz[std::lround(n.hz)] = n.widthHz;
        }
    }
    // Drop biquad history for notches that are no longer active. m_notchState is
    // keyed by round(hz) and was never pruned: with ANF tracking a tuned VFO it
    // grew without bound, and re-using a frequency resurrected that notch's stale
    // x1/x2/y1/y2 as a transient. Erase-by-key rather than clear(), so a notch
    // that survives this call keeps its filter state and does not click.
    for (auto it = m_notchState.begin(); it != m_notchState.end(); ) {
        it = m_notchWidthHz.count(it->first) ? std::next(it)
                                             : m_notchState.erase(it);
    }
}

std::vector<NoiseMixer::Notch> NoiseMixer::autoNotchTones() const
{
    std::vector<Notch> tones;
    for (Channel c : {Channel::Birdie, Channel::Cw}) {
        const ChannelState& s = m_ch.at(c);
        // hz <= 0 is an inaudible carrier (see genBirdie) — nothing to notch.
        if (s.enabled && s.hz > 0.0) tones.push_back({s.hz, 120.0});
    }
    return tones;
}

// One-pole-pair biquad notch at fHz, per-notch state carried across frames.
QVector<float> NoiseMixer::applyNotch(const QVector<float>& in, double fHz, double q)
{
    const double w0 = kTwoPi * fHz / kSampleRate;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cw = std::cos(w0);
    double b0 = 1.0, b1 = -2.0 * cw, b2 = 1.0;
    const double a0 = 1.0 + alpha, a1c = -2.0 * cw, a2c = 1.0 - alpha;
    b0 /= a0; b1 /= a0; b2 /= a0;
    const double a1 = a1c / a0, a2 = a2c / a0;

    auto& st = m_notchState[std::lround(fHz)];   // {x1,x2,y1,y2}, zero-init
    double x1 = st[0], x2 = st[1], y1 = st[2], y2 = st[3];
    QVector<float> out(in.size());
    for (int i = 0; i < in.size(); ++i) {
        const double x0 = in[i];
        const double y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        out[i] = static_cast<float>(y0);
        x2 = x1; x1 = x0; y2 = y1; y1 = y0;
    }
    st = {x1, x2, y1, y2};
    return out;
}

// ---- generators ---------------------------------------------------------------
void NoiseMixer::genCw(const ChannelState& c, float* out)
{
    // Keys the CW id in real Morse — 18 WPM, standard 1/3/7-dit
    // element/character/word spacing, raised-cosine edges (no key clicks) — so
    // the demo CW channel is decodable by ear, by CwDecoder, or by an external
    // decoder fed over TCI. The schedule is built from kMsg, so a longer id
    // (e.g. "CQ CQ DE AETHER") is a one-line change.
    const double hz = c.hz > 0 ? c.hz : 700.0;
    const double wpm = 18.0, dit = 1.2 / wpm;
    struct Span { double on, off; };            // one key-down interval, in dits
    struct Schedule { std::vector<Span> spans; double total = 0.0; };
    static const Schedule sched = [] {
        static const char* kMsg = "L";              // ·—·· — the original id
        auto code = [](char ch) -> const char* {
            switch (ch) {
                case 'A': return ".-";   case 'C': return "-.-.";
                case 'D': return "-..";  case 'E': return ".";
                case 'H': return "...."; case 'L': return ".-..";
                case 'Q': return "--.-"; case 'R': return ".-.";
                case 'T': return "-";
                default:  return "";
            }
        };
        Schedule s;
        double t = 0.0;
        for (const char* p = kMsg; *p; ++p) {
            if (*p == ' ') { t += 4.0; continue; }     // char gap 3 + 4 = 7-dit word gap
            for (const char* e = code(*p); *e; ++e) {
                const double dur = (*e == '-') ? 3.0 : 1.0;
                s.spans.push_back({t, t + dur});
                t += dur + 1.0;                        // element + 1-dit element gap
            }
            t += 2.0;                                  // element gap 1 + 2 = 3-dit char gap
        }
        s.total = t + 4.0;                             // loop restart = 7-dit word gap
        return s;
    }();
    const int nSpans = static_cast<int>(sched.spans.size());
    for (int i = 0; i < kFrameLen; ++i) {
        const double tt = static_cast<double>(m_cwPhase + i) / kSampleRate;
        const double pos = std::fmod(tt, sched.total * dit);
        if (pos < m_cwPos) m_cwSpan = 0;               // new message cycle
        m_cwPos = pos;
        while (m_cwSpan < nSpans && pos >= sched.spans[m_cwSpan].off * dit) ++m_cwSpan;
        double key = 0.0;
        if (m_cwSpan < nSpans && pos >= sched.spans[m_cwSpan].on * dit) {
            const double start = sched.spans[m_cwSpan].on * dit;
            const double end   = sched.spans[m_cwSpan].off * dit;
            key = 1.0;
            const double edge = 0.005, into = pos - start, left = end - pos;
            if (into < edge)      key = 0.5 - 0.5 * std::cos((kTwoPi / 2.0) * into / edge);
            else if (left < edge) key = 0.5 - 0.5 * std::cos((kTwoPi / 2.0) * left / edge);
        }
        out[i] = static_cast<float>(key * std::sin(kTwoPi * hz * tt));
    }
    m_cwPhase += kFrameLen;
}

void NoiseMixer::loadVoiceClip()
{
    // Read the bundled 24 kHz mono 16-bit PCM clip once, into float samples.
    m_voiceLoaded = true;   // set even on failure so we don't retry every frame
    QFile f(QStringLiteral(":/demo_voice.wav"));
    if (!f.open(QIODevice::ReadOnly)) return;
    const QByteArray raw = f.readAll();

    // Minimal RIFF/WAVE parse: WALK the chunk list to find "data", rather than
    // scanning for the literal bytes anywhere in the file. A bare indexOf("data")
    // matches the first occurrence wherever it lands — inside a LIST/INFO chunk,
    // or even in PCM data — and ignoring the declared chunk size means trailing
    // chunks after the samples get decoded as audio. The `fmt ` chunk is validated
    // too, so an unsupported replacement asset is rejected outright instead of
    // being played as noise at the wrong pitch.
    auto u32 = [&raw](int off) -> quint32 {
        return static_cast<quint32>(static_cast<quint8>(raw[off]))
             | (static_cast<quint32>(static_cast<quint8>(raw[off + 1])) << 8)
             | (static_cast<quint32>(static_cast<quint8>(raw[off + 2])) << 16)
             | (static_cast<quint32>(static_cast<quint8>(raw[off + 3])) << 24);
    };

    // "RIFF" <size> "WAVE", then a sequence of <id><size><payload>, each chunk
    // padded to an even length.
    if (raw.size() < 12 || !raw.startsWith("RIFF") || raw.mid(8, 4) != "WAVE")
        return;

    auto u16 = [&raw](int off) -> quint16 {
        return static_cast<quint16>(static_cast<quint8>(raw[off]))
             | (static_cast<quint16>(static_cast<quint8>(raw[off + 1])) << 8);
    };

    int dataOff = -1;
    qint64 dataLen = 0;
    bool haveFmt = false;
    for (int pos = 12; pos + 8 <= raw.size(); ) {
        const QByteArray id = raw.mid(pos, 4);
        const qint64 len = static_cast<qint64>(u32(pos + 4));
        const int payload = pos + 8;
        if (payload + len > raw.size())
            break;                       // truncated or malformed — take nothing
        if (id == "fmt ") {
            // Validate the format instead of assuming it. The loop below decodes
            // interleaved 16-bit PCM at kSampleRate; a stereo, 8/24/32-bit or
            // differently-sampled clip would otherwise be read as garbage or play
            // at the wrong pitch rather than being rejected. Bundled asset today,
            // but this is the code path any replacement asset lands on.
            if (len < 16) return;
            const quint16 format   = u16(payload);        // 1 == WAVE_FORMAT_PCM
            const quint16 channels = u16(payload + 2);
            const quint32 rate     = u32(payload + 4);
            const quint16 bits     = u16(payload + 14);
            if (format != 1 || channels != 1 || bits != 16
                || rate != static_cast<quint32>(kSampleRate)) {
                qWarning("NoiseMixer: voice clip must be mono 16-bit PCM @ %d Hz "
                         "(got format=%u ch=%u bits=%u rate=%u) - not loaded",
                         kSampleRate, format, channels, bits, rate);
                return;
            }
            haveFmt = true;
        } else if (id == "data") {
            dataOff = payload;
            dataLen = len;
            break;                       // fmt always precedes data in a valid WAVE
        }
        pos = payload + static_cast<int>(len + (len & 1));   // chunks are word-aligned
    }
    if (!haveFmt || dataOff < 0 || dataLen < 2) return;

    const int nSamples = static_cast<int>(dataLen / 2);
    m_voiceSamples.resize(nSamples);
    const auto* s = reinterpret_cast<const qint16*>(raw.constData() + dataOff);
    for (int i = 0; i < nSamples; ++i)
        m_voiceSamples[i] = static_cast<float>(s[i]) / 32768.0f;
}

void NoiseMixer::genVoice(const ChannelState&, float* out)
{
    // Play the bundled speech clip (real words), looped. 24 kHz mono == our rate,
    // so no resample. Silent if the clip failed to load.
    if (!m_voiceLoaded) loadVoiceClip();
    if (m_voiceSamples.empty()) {
        for (int i = 0; i < kFrameLen; ++i) out[i] = 0.0f;
        return;
    }
    for (int i = 0; i < kFrameLen; ++i) {
        out[i] = m_voiceSamples[m_voicePos] * 3.0f;   // scale to a healthy level
        if (++m_voicePos >= m_voiceSamples.size()) m_voicePos = 0;   // loop
    }
}

void NoiseMixer::genWhite(const ChannelState&, float* out)
{
    for (int i = 0; i < kFrameLen; ++i) out[i] = static_cast<float>(nextGauss() * kNoiseRef);
}

void NoiseMixer::genPink(const ChannelState&, float* out)
{
    // Voss-McCartney: octave rows updated by trailing-zero of a counter.
    for (int i = 0; i < kFrameLen; ++i) {
        m_pinkCtr = (m_pinkCtr + 1) & ((1u << kPinkRows) - 1);
        const std::uint32_t diff = m_pinkKey ^ m_pinkCtr;
        for (int r = 0; r < kPinkRows; ++r)
            if (diff & (1u << r)) m_pinkRows[r] = nextUniform() * 2.0 - 1.0;
        m_pinkKey = m_pinkCtr;
        double sum = 0.0;
        for (double v : m_pinkRows) sum += v;
        out[i] = static_cast<float>(sum / kPinkRows * kNoiseRef * 3.0);
    }
}

void NoiseMixer::genQrn(const ChannelState& c, float* out)
{
    const double p = (c.rate > 0 ? c.rate : 12.0) / kSampleRate;   // per-sample trigger
    for (int i = 0; i < kFrameLen; ++i) {
        if (nextUniform() < p) {
            m_qrnEnv = 1.0;
            m_qrnSign = nextUniform() < 0.5 ? 1.0 : -1.0;
        }
        out[i] = static_cast<float>(m_qrnSign * m_qrnEnv * (0.6 + 0.4 * nextUniform())
                                    * kNoiseRef * 5.0);
        m_qrnEnv *= 0.86;   // fast exponential tail
    }
}

void NoiseMixer::genPowerline(const ChannelState& c, float* out)
{
    const double f0 = c.freq > 0 ? c.freq : 60.0;
    for (int i = 0; i < kFrameLen; ++i) {
        const double tt = static_cast<double>(m_plPhase + i) / kSampleRate;
        double v = 0.0;
        for (int h : {1, 3, 5, 7, 9, 11}) v += (1.0 / h) * std::sin(kTwoPi * f0 * h * tt);
        out[i] = static_cast<float>(v * kNoiseRef * 0.5);
    }
    m_plPhase += kFrameLen;
}

void NoiseMixer::genCrashes(const ChannelState& c, float* out)
{
    const double p = (c.rate > 0 ? c.rate : 0.4) / kSampleRate;
    for (int i = 0; i < kFrameLen; ++i) {
        if (nextUniform() < p) m_crashEnv = 1.0;
        m_crashLp = 0.6 * m_crashLp + 0.4 * nextGauss();     // low-passed rush
        out[i] = static_cast<float>(m_crashLp * m_crashEnv * kNoiseRef * 4.0);
        m_crashEnv *= 0.9995;   // long decay (~150 ms)
    }
}

void NoiseMixer::genBirdie(const ChannelState& c, float* out)
{
    // hz <= 0 means SILENT. Callers use it to say "this carrier is not audible"
    // — the wrong sideband, or an offset outside the audio passband. This used to
    // fall back to 1000.0, so "silence" was really a fixed 1 kHz tone that no
    // amount of tuning could get rid of. The ctor seeds a sane default pitch
    // (1 kHz), so a zero here is always deliberate.
    if (c.hz <= 0.0) {
        std::fill(out, out + kFrameLen, 0.0f);
        return;
    }
    // Continuous phase accumulation: advance by 2π·hz/rate per sample. A pitch
    // change (VFO tuning) then continues smoothly from the current phase instead
    // of jumping (which caused an audible warble as the pitch tracked the VFO).
    const double dphi = kTwoPi * c.hz / kSampleRate;
    for (int i = 0; i < kFrameLen; ++i) {
        out[i] = static_cast<float>(std::sin(m_birdiePhaseRad) * kNoiseRef * 1.2);
        m_birdiePhaseRad += dphi;
    }
    // wrap to keep the accumulator bounded (precision + no overflow over hours)
    if (m_birdiePhaseRad > kTwoPi) m_birdiePhaseRad = std::fmod(m_birdiePhaseRad, kTwoPi);
}

void NoiseMixer::genHash(const ChannelState& c, float* out)
{
    const double prf = c.prf > 0 ? c.prf : 120.0;
    for (int i = 0; i < kFrameLen; ++i) {
        const double tt = static_cast<double>(m_hashPhase + i) / kSampleRate;
        const double gate = std::fmod(tt * prf, 1.0) < 0.4 ? 1.0 : 0.15;
        out[i] = static_cast<float>(nextGauss() * gate * kNoiseRef * 2.0);
    }
    m_hashPhase += kFrameLen;
}

void NoiseMixer::genWoodpecker(const ChannelState& c, float* out)
{
    const double prf = c.prf > 0 ? c.prf : 10.0;
    for (int i = 0; i < kFrameLen; ++i) {
        const double tt = static_cast<double>(m_woodPhase + i) / kSampleRate;
        const bool on = std::fmod(tt * prf, 1.0) < 0.5;
        out[i] = static_cast<float>((on ? nextGauss() : 0.0) * kNoiseRef * 3.0);
    }
    m_woodPhase += kFrameLen;
}

// ---- mix ----------------------------------------------------------------------
QVector<float> NoiseMixer::mixFrame()
{
    QVector<float> acc(kFrameLen, 0.0f);
    float scratch[kFrameLen];
    auto add = [&](Channel c, void (NoiseMixer::*gen)(const ChannelState&, float*)) {
        const ChannelState& s = m_ch.at(c);
        if (!s.enabled) return;
        (this->*gen)(s, scratch);
        const float g = static_cast<float>(db2lin(s.levelDb));
        for (int i = 0; i < kFrameLen; ++i) acc[i] += scratch[i] * g;
    };
    add(Channel::Cw,         &NoiseMixer::genCw);
    add(Channel::Voice,      &NoiseMixer::genVoice);
    add(Channel::White,      &NoiseMixer::genWhite);
    add(Channel::Pink,       &NoiseMixer::genPink);
    add(Channel::Qrn,        &NoiseMixer::genQrn);
    add(Channel::Powerline,  &NoiseMixer::genPowerline);
    add(Channel::Crashes,    &NoiseMixer::genCrashes);
    add(Channel::Birdie,     &NoiseMixer::genBirdie);
    add(Channel::Hash,       &NoiseMixer::genHash);
    add(Channel::Woodpecker, &NoiseMixer::genWoodpecker);

    // TNF/ANF notches: remove each notched audio frequency (biquad).
    for (double fHz : m_notchHz) acc = applyNotch(acc, fHz, 8.0);

    // Noise blanker: detect impulses (QRN/crash) as samples that spike well above
    // the STEADY background level, and blank them. Key: the background estimate is
    // a SLOW average that impulses barely move — a fast tracker would chase the
    // impulse up and never fire (the bug that made NB do nothing). A short hold
    // after a hit blanks the impulse's exponential tail too, not just the peak.
    if (m_nbOn) {
        for (int i = 0; i < kFrameLen; ++i) {
            const double a = std::abs(acc[i]);
            const double thresh = 4.0 * m_nbEnv + 1e-3;
            if (a > thresh || m_nbHold > 0) {
                if (a > thresh) m_nbHold = 12;   // ~0.5 ms hold covers the QRN tail
                acc[i] *= 0.05f;                 // blank
                if (m_nbHold > 0) --m_nbHold;
            } else {
                // update the slow background ONLY from non-impulse samples, so a
                // burst can't inflate the level and hide itself.
                m_nbEnv = 0.999 * m_nbEnv + 0.001 * a;
            }
        }
    }

    // soft clip (tanh) — keep within [-1,1] without hard edges.
    for (int i = 0; i < kFrameLen; ++i) acc[i] = static_cast<float>(std::tanh(acc[i]));
    return acc;
}

// ---- spectrum render (waterfall follows the audio) ----------------------------
QVector<float> NoiseMixer::spectrum(int n, double floorDbm, double spanHz, int center)
{
    QVector<float> out(n, static_cast<float>(floorDbm));
    const double hzPerBin = n ? spanHz / n : 1.0;
    auto binOf = [&](double hz) { return static_cast<int>(center + hz / hzPerBin); };
    auto bump  = [&](int b, double dbm) {
        if (b >= 0 && b < n) out[b] = static_cast<float>(addDb(out[b], dbm));
    };
    // Real per-FFT-bin noise deviation: power ~ exponential, so dB ~ 10log10(exp)
    // — grassy, downward-tailed. Noise BECOMES THE FLOOR: its grass bottoms out on
    // floorDbm and rises up (louder = taller), not a band floating above it.
    auto noiseBinDb = [&]() {
        double u = nextDisplayUniform();
        if (u <= 0.0) u = 1e-9;
        return 10.0 * std::log10(-std::log(u));
    };
    auto grass = [&](double levelDb) {
        const double height = 8.0 + (levelDb + 60.0) * 0.6;   // -60->8dB .. 0->44dB
        return floorDbm + std::max(0.0, height + noiseBinDb());
    };

    for (const auto& kv : m_ch) {
        const Channel c = kv.first;
        const ChannelState& s = kv.second;
        if (!s.enabled) continue;
        const double lvl = s.levelDb;
        switch (c) {
        case Channel::White:
            for (int b = 0; b < n; ++b) bump(b, grass(lvl));
            break;
        case Channel::Pink:
            for (int b = 0; b < n; ++b)
                bump(b, grass(lvl) - 14.0 * (double(b) / std::max(1, n - 1)));
            break;
        case Channel::Hash:
            for (int b = 0; b < n; ++b) bump(b, grass(lvl));
            break;
        case Channel::Qrn:
            if (nextDisplayUniform() < std::min(0.9, (s.rate > 0 ? s.rate : 12.0) / 20.0)) {
                const double top = floorDbm + 90.0 + lvl + 6.0;
                for (int b = 0; b < n; ++b) bump(b, top + (nextDisplayUniform() * 8.0 - 6.0));
            }
            break;
        case Channel::Crashes:
            if (nextDisplayUniform() < 0.15) {
                const double top = floorDbm + 90.0 + lvl;
                for (int b = 0; b < n; ++b) bump(b, top + (nextDisplayUniform() * 10.0 - 8.0));
            }
            break;
        case Channel::Woodpecker:
            if (nextDisplayUniform() < 0.5) {
                const double top = floorDbm + 90.0 + lvl;
                for (int b = 0; b < n; ++b) bump(b, top + (nextDisplayUniform() * 8.0 - 5.0));
            }
            break;
        case Channel::Birdie: {
            if (s.hz <= 0.0) break;   // inaudible: draw nothing (see genBirdie)
            const int b = binOf(s.hz);
            bump(b, floorDbm + 90.0 + lvl + 10.0);
            bump(b - 1, floorDbm + 90.0 + lvl - 6.0);
            bump(b + 1, floorDbm + 90.0 + lvl - 6.0);
            break;
        }
        case Channel::Powerline: {
            const double f0 = s.freq > 0 ? s.freq : 60.0;
            for (int h = 1; h < 12; h += 2)
                bump(binOf(f0 * h), floorDbm + 90.0 + lvl - 3.0 * (h / 2));
            break;
        }
        case Channel::Cw: {
            const int b = binOf(s.hz > 0 ? s.hz : 700.0);
            bump(b, floorDbm + 90.0 + lvl + 8.0);
            bump(b - 1, floorDbm + 90.0 + lvl - 8.0);
            bump(b + 1, floorDbm + 90.0 + lvl - 8.0);
            break;
        }
        case Channel::Voice: {
            // SSB-ish energy band 300..2700 Hz above the VFO, with two brighter
            // formant clumps so the waterfall reads like on-air speech.
            const double top = floorDbm + 90.0 + lvl;
            for (int b = binOf(300); b < binOf(2700) && b < n; ++b) {
                if (b < 0) continue;
                bump(b, top - 8.0 + nextDisplayUniform() * 6.0);
            }
            bump(binOf(600), top);      // formant-ish peaks
            bump(binOf(1800), top - 4.0);
            break;
        }
        }
    }

    // Notch display: carve each notched offset to the floor (line disappears).
    for (double fHz : m_notchHz) {
        const int nb = binOf(fHz);
        const double w = m_notchWidthHz.count(std::lround(fHz))
                             ? m_notchWidthHz.at(std::lround(fHz)) : 100.0;
        const int half = std::max(1, static_cast<int>(w / (2 * hzPerBin)));
        for (int b = nb - half; b <= nb + half; ++b)
            if (b >= 0 && b < n) out[b] = static_cast<float>(floorDbm);
    }
    return out;
}

// ---- names --------------------------------------------------------------------
QString NoiseMixer::name(Channel c)
{
    switch (c) {
    case Channel::Cw: return QStringLiteral("cw");
    case Channel::Voice: return QStringLiteral("voice");
    case Channel::White: return QStringLiteral("white");
    case Channel::Pink: return QStringLiteral("pink");
    case Channel::Qrn: return QStringLiteral("qrn");
    case Channel::Powerline: return QStringLiteral("powerline");
    case Channel::Crashes: return QStringLiteral("crashes");
    case Channel::Birdie: return QStringLiteral("birdie");
    case Channel::Hash: return QStringLiteral("hash");
    case Channel::Woodpecker: return QStringLiteral("woodpecker");
    }
    return QStringLiteral("white");
}

NoiseMixer::Channel NoiseMixer::fromName(const QString& n, bool* ok)
{
    for (Channel c : allChannels())
        if (name(c) == n) { if (ok) *ok = true; return c; }
    if (ok) *ok = false;
    return Channel::White;
}

QVector<NoiseMixer::Channel> NoiseMixer::allChannels()
{
    return {Channel::Cw, Channel::Voice, Channel::White, Channel::Pink, Channel::Qrn,
            Channel::Powerline, Channel::Crashes, Channel::Birdie,
            Channel::Hash, Channel::Woodpecker};
}

namespace {
// Named scenes: {channel, level dB}. Mirrors flex-sim's presets.
struct PresetEntry { NoiseMixer::Channel ch; double level; };
const std::map<QString, std::vector<PresetEntry>>& presetTable()
{
    using C = NoiseMixer::Channel;
    static const std::map<QString, std::vector<PresetEntry>> t = {
        {QStringLiteral("quiet-20m"),  {{C::Pink, -30}}},
        {QStringLiteral("night-40m"),  {{C::Pink, -22}, {C::Qrn, -24}, {C::Crashes, -22}}},
        {QStringLiteral("storm"),      {{C::Pink, -20}, {C::Qrn, -12}, {C::Crashes, -10}}},
        {QStringLiteral("noisy-qth"),  {{C::Pink, -24}, {C::Powerline, -16}, {C::Hash, -18}}},
        {QStringLiteral("birdie-hell"),{{C::Pink, -28}, {C::Birdie, -18}, {C::Powerline, -22}}},
        {QStringLiteral("cw-in-noise"),{{C::Cw, -16}, {C::Pink, -24}, {C::Qrn, -22}}},
    };
    return t;
}
}  // namespace

void NoiseMixer::loadPreset(const QString& presetName)
{
    const auto& table = presetTable();
    auto it = table.find(presetName);
    if (it == table.end()) return;                 // unknown — leave scene as-is
    for (auto& kv : m_ch) kv.second.enabled = false;   // presets are absolute
    for (const PresetEntry& e : it->second) {
        m_ch[e.ch].enabled = true;
        m_ch[e.ch].levelDb = e.level;
    }
}

QStringList NoiseMixer::allPresetNames()
{
    QStringList out;
    for (const auto& kv : presetTable()) out << kv.first;
    return out;
}

}  // namespace AetherSDR
