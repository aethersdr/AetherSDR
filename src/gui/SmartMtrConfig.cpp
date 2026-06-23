#include "SmartMtrConfig.h"

#include "SmartMtrStyle.h"

#include <algorithm>
#include <cmath>
#include <vector>

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

// Plain linear map of the value's [min,max] onto the scale band. Used by every
// kind whose scale is linear in its own units (mic dBFS, compression dB, forward
// power watts); the per-kind range is supplied through MeterInput at push time.
double mapLinear(double v, double min, double max)
{
    return lerp(v, min, max, kScaleMin, kScaleMax);
}

// ── Mic level (transmit) ────────────────────────────────────────────────────
// Linear dBFS scale from -40 (bottom) to 0 (full scale / clip, top). -20 lands
// naturally at the midpoint; the top of the scale (-5 and 0) is drawn in the
// "high" colour as the clip-warning zone.
constexpr double kMicMindB = -40.0;
constexpr double kMicMaxdB = 0.0;

// ── SWR (transmit) ──────────────────────────────────────────────────────────
// Nonlinear so the operating-critical 1.0..2.0 band gets most of the scale and
// the 2.0..3.0 tail is compressed (mirrors the signal meter pinning S9 mid). The
// knee (2.0) is where SWR turns "bad" — the red zone — pinned at 70% of the band.
constexpr double kSwrMin = 1.0;
constexpr double kSwrKnee = 2.0; // red zone begins
constexpr double kSwrMax = 3.0;
constexpr double kSwrKneePos = kScaleMin + 0.70 * (kScaleMax - kScaleMin);

double mapSwr(double v, double min, double max)
{
    if (v <= kSwrKnee)
        return lerp(v, min, kSwrKnee, kScaleMin, kSwrKneePos);
    return lerp(v, kSwrKnee, max, kSwrKneePos, kScaleMax);
}

// ── Compression (transmit) ──────────────────────────────────────────────────
// Linear gain-reduction scale: 0 dB = no compression (top/right), -25 dB = max
// compression (bottom/left). The radio reports a positive amount; VfoWidget
// negates it into this display domain. No danger zone.
constexpr double kCompMindB = -25.0;
constexpr double kCompMaxdB = 0.0;

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
    cfg.valueToPosition = mapLinear;

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
        m.position = mapLinear(t.db, kMicMindB, kMicMaxdB);
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

MeterConfig buildSwrConfig()
{
    MeterConfig cfg;
    cfg.valueToPosition = mapSwr;

    // 1.0 / 1.5 (good, blue) and 2.0 / 3.0 (the red "high SWR" zone). 2.0 is the
    // knee — the boundary where the scale compresses and the colour turns red.
    struct SwrTick { double swr; MarkerColor color; QString label; };
    static const SwrTick ticks[] = {
        { 1.0, MarkerColor::Normal, QStringLiteral("1") },
        { 1.5, MarkerColor::Normal, QStringLiteral("1.5") },
        { 2.0, MarkerColor::High,   QStringLiteral("2") },
        { 3.0, MarkerColor::High,   QStringLiteral("3") },
    };
    for (const SwrTick& t : ticks) {
        ScaleMarker m;
        m.position = mapSwr(t.swr, kSwrMin, kSwrMax);
        m.size = MarkerSize::Large;
        m.color = t.color;
        m.label = t.label;
        // labelOffset 0 -> the renderer centers the label on its tick.
        cfg.markers.push_back(m);
    }
    return cfg;
}

MeterConfig buildCompressionConfig()
{
    MeterConfig cfg;
    cfg.valueToPosition = mapLinear;
    // Gain-reduction face: bar grows from the 0 (right) end toward -25 as
    // compression rises, so idle reads empty (see SmartMtrWidget::drawIndicator).
    cfg.reversed = true;

    // A labeled tick every 5 dB across -25..0. Compression has no danger zone, so
    // every tick is the normal colour.
    for (int db = int(kCompMindB); db <= int(kCompMaxdB); db += 5) {
        ScaleMarker m;
        m.position = mapLinear(db, kCompMindB, kCompMaxdB);
        m.size = MarkerSize::Large;
        m.color = MarkerColor::Normal;
        m.label = QString::number(db);
        // Offset like the mic labels: put the tick between the two digits,
        // ignoring the leading minus (two-digit -> -4.0; the single-digit "-5"
        // centers its digit -> -2.0; "0" needs none).
        m.labelOffset = (db <= -10) ? -4.0 : (db < 0 ? -2.0 : 0.0);
        cfg.markers.push_back(m);
    }
    return cfg;
}

// Round "nice" tick step (~6 divisions) for a watts full scale.
double powerTickStep(double fullScaleW)
{
    const double raw = fullScaleW / 6.0;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double norm = raw / mag; // [1, 10)
    double step = 10.0;
    if (norm < 1.5) step = 1.0;
    else if (norm < 3.0) step = 2.0;
    else if (norm < 7.0) step = 5.0;
    return step * mag;
}

// Compact watt label: 1500 -> "1.5K", 600 -> "600".
QString formatWatts(double w)
{
    if (w >= 1000.0) {
        QString s = QString::number(w / 1000.0, 'f', 1);
        if (s.endsWith(QStringLiteral(".0")))
            s.chop(2);
        return s + QStringLiteral("K");
    }
    return QString::number(qRound(w));
}

} // namespace

const MeterConfig& meterConfig(MeterKind kind)
{
    static const MeterConfig signalCfg = buildSignalConfig();
    static const MeterConfig micCfg = buildMicConfig();
    static const MeterConfig swrCfg = buildSwrConfig();
    static const MeterConfig compCfg = buildCompressionConfig();
    // Power markers are radio-aware, so the widget rebuilds them per radio via
    // buildPowerConfig(); this registry copy exists only to carry the (linear)
    // valueToPosition for indicatorPosition(). A barefoot default scale is fine.
    static const MeterConfig powerCfg = buildPowerConfig(120.0);
    switch (kind) {
    case MeterKind::MicLevel:
        return micCfg;
    case MeterKind::SWR:
        return swrCfg;
    case MeterKind::Power:
        return powerCfg;
    case MeterKind::Compression:
        return compCfg;
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

MeterConfig buildPowerConfig(double fullScaleW)
{
    MeterConfig cfg;
    cfg.valueToPosition = mapLinear;
    if (fullScaleW <= 0.0)
        return cfg;

    const double redStart = fullScaleW / kPowerHeadroom; // the radio's rated power
    const double step = powerTickStep(fullScaleW);
    const double eps = step * 0.01;

    // Ticks at the round grid, plus the scale top and the rated (red) boundary so
    // both are always marked even when off-grid.
    std::vector<double> values;
    for (double w = 0.0; w <= fullScaleW + eps; w += step)
        values.push_back(w);
    auto addUnique = [&](double w) {
        for (double v : values)
            if (std::abs(v - w) < eps)
                return;
        values.push_back(w);
    };
    addUnique(fullScaleW);
    addUnique(redStart);
    std::sort(values.begin(), values.end());

    for (double w : values) {
        ScaleMarker m;
        m.position = mapLinear(w, 0.0, fullScaleW);
        m.size = MarkerSize::Large;
        // "Over rated" reads red: the rated tick and everything above it.
        m.color = (w >= redStart - eps) ? MarkerColor::High : MarkerColor::Normal;
        m.label = formatWatts(w);
        // labelOffset 0 -> the renderer centers the label on its tick.
        cfg.markers.push_back(m);
    }
    return cfg;
}

} // namespace AetherSDR
