#include "SmartMtrConfig.h"

#include "SmartMtrStyle.h"

#include <algorithm>

namespace AetherSDR {

using namespace SmartMtrUnits;

namespace {

// Linear map of v in [a,b] onto [pa,pb] (unclamped). Degenerate range -> pa.
double lerp(double v, double a, double b, double pa, double pb)
{
    if (a == b)
        return pa;
    return pa + (v - a) / (b - a) * (pb - pa);
}

// Midpoint of the scale band — where S9 is pinned for the signal meter.
constexpr double kScaleMid = (kScaleMin + kScaleMax) / 2.0; // 120

// ── Signal (received) ───────────────────────────────────────────────────────
// S-units are 6 dB apart; S9 = -73 dBm (HF convention). S0 sits 9 units below,
// the top of the scale is +60 dB over S9.
constexpr double kSignalS0dBm = -127.0; // S9 - 9*6
constexpr double kSignalS9dBm = -73.0;
constexpr double kSignalMaxdBm = -13.0; // S9 + 60

// Piecewise so S9 lands exactly at the scale midpoint: S0..S9 fills the lower
// half, S9..+60 the upper half (each segment linear in dBm, different slopes).
double mapSignal(double v, double min, double max)
{
    if (v <= kSignalS9dBm)
        return lerp(v, min, kSignalS9dBm, kScaleMin, kScaleMid);
    return lerp(v, kSignalS9dBm, max, kScaleMid, kScaleMax);
}

// ── Mic level (transmit) ────────────────────────────────────────────────────
// Linear dBFS scale from -40 (bottom) to 0 (full scale / clip, top). -20 lands
// naturally at the midpoint; the top of the scale (-5 and 0) is drawn in the
// "high" colour as the clip-warning zone.
constexpr double kMicMindB = -40.0;
constexpr double kMicMaxdB = 0.0;

double mapMic(double v, double min, double max)
{
    return lerp(v, min, max, kScaleMin, kScaleMax);
}

// ── Marker tables ───────────────────────────────────────────────────────────
// Authored by value and placed through the same mapping fn at the canonical
// range, so ticks line up with the indicator curve. The stored position is in
// hole-local UNITS — markers are static.

MeterConfig buildSignalConfig()
{
    MeterConfig cfg;
    cfg.valueToPosition = mapSignal;

    // S0..S9: odd S-units large + labeled, even ones small ticks. All blue.
    for (int s = 0; s <= 9; ++s) {
        const double dBm = kSignalS0dBm + s * 6.0;
        ScaleMarker m;
        m.position = mapSignal(dBm, kSignalS0dBm, kSignalMaxdBm);
        m.color = MarkerColor::Normal;
        if (s % 2 == 1) {
            m.size = MarkerSize::Large;
            m.label = QString::number(s);
            if (s == 9) // S9 is the only strong S-meter label
                m.labelStyle = LabelStyle::Strong;
        } else {
            m.size = MarkerSize::Small;
        }
        cfg.markers.push_back(m);
    }

    // +dB over S9: large+labeled at +20/+40/+60, small ticks at +10/+30/+50.
    // All red ("high").
    for (int db = 10; db <= 60; db += 10) {
        const double dBm = kSignalS9dBm + db;
        ScaleMarker m;
        m.position = mapSignal(dBm, kSignalS0dBm, kSignalMaxdBm);
        m.color = MarkerColor::High;
        if (db % 20 == 0) {
            m.size = MarkerSize::Large;
            m.label = QStringLiteral("+") + QString::number(db);
            // Shift the "+NN" label left so the tick falls between the two
            // digits rather than under the leading "+". Tune in UNITS.
            m.labelOffset = -4.0;
        } else {
            m.size = MarkerSize::Small;
        }
        cfg.markers.push_back(m);
    }

    return cfg;
}

MeterConfig buildMicConfig()
{
    MeterConfig cfg;
    cfg.valueToPosition = mapMic;

    // Authored by value and placed through the linear map, so ticks line up with
    // the indicator curve. -40..-10 are blue, -5 and 0 are red as the clip-warning
    // zone. -5 is a small tick, the rest large; all are labeled. labelOffset
    // shifts the label horizontally (UNITS): the two-digit labels use -4.0 so the
    // tick falls between the digits (like the signal +dB labels); -5 uses -2.0 so
    // the "5" digit (not the leading "-") centers on the tick.
    struct MicTick {
        double db;
        MarkerSize size;
        MarkerColor color;
        bool labeled;
        LabelStyle style;
        double labelOffset;
    };
    static const MicTick ticks[] = {
        { -40.0, MarkerSize::Large, MarkerColor::Normal, true, LabelStyle::Normal, -4.0 },
        { -30.0, MarkerSize::Large, MarkerColor::Normal, true, LabelStyle::Normal, -4.0 },
        { -20.0, MarkerSize::Large, MarkerColor::Normal, true, LabelStyle::Normal, -4.0 },
        { -10.0, MarkerSize::Large, MarkerColor::Normal, true, LabelStyle::Normal, -4.0 },
        {  -5.0, MarkerSize::Small, MarkerColor::High,   true, LabelStyle::Normal, -2.0 },
        {   0.0, MarkerSize::Large, MarkerColor::High,   true, LabelStyle::Normal,  0.0 },
    };
    for (const MicTick& t : ticks) {
        ScaleMarker m;
        m.position = mapMic(t.db, kMicMindB, kMicMaxdB);
        m.size = t.size;
        m.color = t.color;
        m.labelStyle = t.style;
        m.labelOffset = t.labelOffset;
        if (t.labeled) {
            // Positive dBFS values get a leading "+", like the signal +dB labels.
            m.label = (t.db > 0.0 ? QStringLiteral("+") : QString())
                      + QString::number(int(t.db));
        }
        cfg.markers.push_back(m);
    }

    return cfg;
}

} // namespace

const MeterConfig& meterConfig(MeterKind kind)
{
    static const MeterConfig signalCfg = buildSignalConfig();
    static const MeterConfig micCfg = buildMicConfig();
    switch (kind) {
    case MeterKind::MicLevel:
        return micCfg;
    case MeterKind::Signal:
        break;
    }
    return signalCfg;
}

double indicatorPosition(const MeterInput& in)
{
    if (!in.hasValue)
        return kScaleMin;
    const double pos = meterConfig(in.kind).valueToPosition(in.value, in.min, in.max);
    return std::clamp(pos, kScaleMin, kScaleMax);
}

} // namespace AetherSDR
