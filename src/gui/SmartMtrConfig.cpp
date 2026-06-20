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

    // -40..0 dB in 10 dB steps, all large + labeled. The 0-dB end is red.
    for (int db = -40; db <= 0; db += 10) {
        ScaleMarker m;
        m.position = mapMic(db, kMicMindB, kMicMaxdB);
        m.size = MarkerSize::Large;
        m.color = (db >= 0) ? MarkerColor::High : MarkerColor::Normal;
        m.label = QString::number(db);
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
