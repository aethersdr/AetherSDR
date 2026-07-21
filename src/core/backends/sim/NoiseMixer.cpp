#include "core/backends/sim/NoiseMixer.h"

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
}

std::vector<NoiseMixer::Notch> NoiseMixer::autoNotchTones() const
{
    std::vector<Notch> tones;
    for (Channel c : {Channel::Birdie, Channel::Cw}) {
        const ChannelState& s = m_ch.at(c);
        if (s.enabled) tones.push_back({s.hz, 120.0});
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
    // A keyed CW id (dit/dah pattern) with raised-cosine edges (no key clicks).
    const double hz = c.hz > 0 ? c.hz : 700.0;
    const double wpm = 18.0, dit = 1.2 / wpm;
    static const int elems[] = {1, 3, 1, 1, 3, 0, 0};  // ·—·· + gaps
    double total = 0.0;
    for (int e : elems) total += (e ? e : 1) * dit;
    for (int i = 0; i < kFrameLen; ++i) {
        const double tt = static_cast<double>(m_cwPhase + i) / kSampleRate;
        const double pos = std::fmod(tt, total);
        double acc = 0.0, key = 0.0;
        for (int e : elems) {
            const double dur = (e ? e : 1) * dit;
            if (pos >= acc && pos < acc + dur) {
                if (e) {
                    key = 1.0;
                    const double edge = 0.005, into = pos - acc, left = acc + dur - pos;
                    if (into < edge)      key = 0.5 - 0.5 * std::cos((kTwoPi / 2.0) * into / edge);
                    else if (left < edge) key = 0.5 - 0.5 * std::cos((kTwoPi / 2.0) * left / edge);
                }
                break;
            }
            acc += dur;
        }
        out[i] = static_cast<float>(key * std::sin(kTwoPi * hz * tt));
    }
    m_cwPhase += kFrameLen;
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
    // Continuous phase accumulation: advance by 2π·hz/rate per sample. A pitch
    // change (VFO tuning) then continues smoothly from the current phase instead
    // of jumping (which caused an audible warble as the pitch tracked the VFO).
    const double hz = c.hz > 0 ? c.hz : 1000.0;
    const double dphi = kTwoPi * hz / kSampleRate;
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
        double u = nextUniform();
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
            if (nextUniform() < std::min(0.9, (s.rate > 0 ? s.rate : 12.0) / 20.0)) {
                const double top = floorDbm + 90.0 + lvl + 6.0;
                for (int b = 0; b < n; ++b) bump(b, top + (nextUniform() * 8.0 - 6.0));
            }
            break;
        case Channel::Crashes:
            if (nextUniform() < 0.15) {
                const double top = floorDbm + 90.0 + lvl;
                for (int b = 0; b < n; ++b) bump(b, top + (nextUniform() * 10.0 - 8.0));
            }
            break;
        case Channel::Woodpecker:
            if (nextUniform() < 0.5) {
                const double top = floorDbm + 90.0 + lvl;
                for (int b = 0; b < n; ++b) bump(b, top + (nextUniform() * 8.0 - 5.0));
            }
            break;
        case Channel::Birdie: {
            const int b = binOf(s.hz > 0 ? s.hz : 1000.0);
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
    return {Channel::Cw, Channel::White, Channel::Pink, Channel::Qrn,
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
