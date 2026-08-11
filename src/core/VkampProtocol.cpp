#include "VkampProtocol.h"

#include <cmath>

#include <QRegularExpression>

namespace AetherSDR {
namespace Vkamp {

namespace {

// Matches the companion project's own STATUS_RE exactly: temp_c and
// volts_x10 and band are multi-digit; antenna/error_code/tx/cooling/bypass
// are always single digits on the wire.
const QRegularExpression& statusRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"((\d+),(\d+),(\d+),(\d),(\d),(\d),(\d),(\d))"));
    return re;
}

Status statusFromMatch(const QRegularExpressionMatch& m)
{
    Status s;
    s.temp_c = m.captured(1).toInt();
    s.volts = m.captured(2).toInt() / 10.0f;
    s.band = m.captured(3).toInt();
    s.antenna = m.captured(4).toInt();
    s.error_code = m.captured(5).toInt();
    s.tx = m.captured(6).toInt() != 0;
    s.cooling_override = m.captured(7).toInt() != 0;
    s.bypass = m.captured(8).toInt() != 0;
    return s;
}

// Midpoint between the two confirmed supply-rail targets (~41.6V low,
// ~57.8-57.9V high -- design doc Section 3.1/Section 5).
constexpr float kVoltageRailMidpoint = 49.7f;

// Calibration constants, ported verbatim from the companion project's own
// fitted curves (design doc Section 3.2) -- least-squares quadratic
// (output/reflected/input) or linear (current) fits against external
// reference measurements. See that project's vkamp_client.py for the full
// point-by-point provenance.
constexpr float kOutputCalA = 0.004512816228704022f;
constexpr float kOutputCalB = -0.1747779854596575f;
constexpr float kOutputCalC = 25.69141235049288f;
// Floor extended down from the companion project's original 175.6 (their
// lowest confirmed point, raw~175.6 -> true 133W) after a live capture on
// this codebase's own hardware: 121 telemetry frames during a steady 1W-
// drive transmission clustered at raw 157-166 (mode 165, 58/121 frames),
// user-confirmed true output ~111W. NOT extended further than this
// steady-state cluster: the same capture's tail-off through 134 and 102 as
// the transmission ended is transient, not a new steady point, and is
// deliberately excluded.
//
// Anchored at 157 -- the LOW edge of that cluster -- not 165, its mode. An
// earlier version of this fix anchored at the mode instead, which put most
// of the real observed jitter (157-164) inside the ramp below it: a linear
// taper multiplies against the full ~120W target, so even a 1-raw-count
// blip swung the ramp's own fraction by 1/rampWidth =33%, i.e. ~40W --
// looking exactly like the value "flickering" even though it was a stable
// transmission the whole time. The quadratic above is actually well-
// resolved through this whole range (~1.3W of true slope per raw count at
// raw~160, confirmed by evaluating it directly) -- the flicker was 100% an
// artifact of running real, already-noisy data through the taper meant for
// smoothing a RARE undershoot blip below the confirmed cluster, not for
// interpolating across the bulk of it. Anchoring at the cluster's own low
// edge instead means raw 157-166 all resolve through the smooth quadratic
// directly, and the (unchanged, still 3-count-wide) ramp only ever
// activates for the rare sub-157 dip actually observed in this same
// capture's tail-off -- exactly the boundary-smoothing role it was
// originally designed for (see kOutputCalRampWidth's own doc comment).
constexpr float kOutputCalMinRaw = 157.0f;
constexpr float kOutputCalRampWidth = 3.0f;

constexpr float kCurrentCalA = 0.4052965787643523f;
constexpr float kCurrentCalB = 0.3049154652689805f;

constexpr float kReflectedCalA = 0.005033946087463804f;
constexpr float kReflectedCalB = -0.09293657447890188f;
constexpr float kReflectedCalC = 2.4337837601262087f;

constexpr float kInputCalA = 0.0008018661524991072f;
constexpr float kInputCalB = 0.1595133745143524f;
constexpr float kInputCalC = 1.004316538777762f;

}  // namespace

float ratedWatts(Variant v)
{
    switch (v) {
        case Variant::W600:  return 600.0f;
        case Variant::W1000: return 1000.0f;
        case Variant::W2000: return 2000.0f;
    }
    return 2000.0f;
}

float meterFullScaleWatts(Variant v)
{
    return ratedWatts(v) * 1.25f;
}

QString variantLabel(Variant v)
{
    return QStringLiteral("%1 W").arg(static_cast<int>(ratedWatts(v)));
}

QString bandName(int f1)
{
    switch (f1) {
        case 1: return QStringLiteral("160");
        case 2: return QStringLiteral("80");
        case 3: return QStringLiteral("40");
        case 4: return QStringLiteral("30");
        case 5: return QStringLiteral("20");
        case 6: return QStringLiteral("17-15");
        case 7: return QStringLiteral("12-10");
        case 8: return QStringLiteral("6");
        default: return QString();
    }
}

bool Status::voltageLow() const
{
    return volts < kVoltageRailMidpoint;
}

std::optional<Status> parseStatus(const QByteArray& text)
{
    const QRegularExpressionMatch m = statusRegex().match(QString::fromLatin1(text));
    if (!m.hasMatch()) {
        return std::nullopt;
    }
    return statusFromMatch(m);
}

void StatusStreamParser::feed(const QByteArray& bytes)
{
    m_buf.append(bytes);

    const QString text = QString::fromLatin1(m_buf);
    QRegularExpressionMatchIterator it = statusRegex().globalMatch(text);

    int lastEnd = -1;
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        if (m_onStatus) {
            m_onStatus(statusFromMatch(m));
        }
        lastEnd = m.capturedEnd(0);
    }

    if (lastEnd >= 0) {
        // Mirrors the companion project's own buffer-trim: keep only what
        // follows the LAST match, not a fresh re-scan of a growing buffer
        // next feed() -- an earlier, since-fixed bug in that project was
        // exactly the opposite of this (see design doc Section 1).
        m_buf = m_buf.mid(lastEnd);
    } else if (m_buf.size() > kMaxBufferBytes) {
        // No match at all this round and the buffer is growing without
        // bound (a non-matching/garbage stream) -- cap it rather than
        // absorb memory indefinitely. Same fallback shape as the companion
        // project's own serial-status parser.
        m_buf = m_buf.right(kMaxBufferBytes);
    }
}

float Telemetry::outputWatts() const
{
    const float raw = static_cast<float>(output);
    const float rampStart = kOutputCalMinRaw - kOutputCalRampWidth;
    if (raw < rampStart) {
        return 0.0f;
    }
    const float value = std::max(0.0f, kOutputCalA * raw * raw + kOutputCalB * raw + kOutputCalC);
    if (raw < kOutputCalMinRaw) {
        // Linear taper from 0 (at rampStart) up to the curve's own value at
        // kOutputCalMinRaw -- continuous, not a step (design doc Section 3.2
        // via the companion project's own _OUTPUT_CAL_RAMP_WIDTH comment).
        return value * (raw - rampStart) / kOutputCalRampWidth;
    }
    return value;
}

float Telemetry::reflectedWatts() const
{
    const float raw = static_cast<float>(reflected);
    return std::max(0.0f, kReflectedCalA * raw * raw + kReflectedCalB * raw + kReflectedCalC);
}

float Telemetry::currentAmps() const
{
    return std::max(0.0f, static_cast<float>(current) * kCurrentCalA + kCurrentCalB);
}

float Telemetry::inputWatts() const
{
    const float raw = static_cast<float>(input_raw);
    return std::max(0.0f, kInputCalA * raw * raw + kInputCalB * raw + kInputCalC);
}

float Telemetry::swr() const
{
    const float fwd = outputWatts();
    const float refl = reflectedWatts();
    if (fwd <= 0.0f || refl <= 0.0f) {
        return 1.0f;  // no carrier / no reflected -- matches the amp's own idle default
    }
    const float rho = std::min(0.99f, std::sqrt(refl / fwd));  // guard refl>=fwd from noise/calibration error
    return (1.0f + rho) / (1.0f - rho);
}

std::optional<Telemetry> parseTelemetry(const QByteArray& payload)
{
    const int nul = payload.indexOf('\0');
    const QByteArray trimmed = (nul >= 0) ? payload.left(nul) : payload;
    const QStringList parts = QString::fromLatin1(trimmed).split(QLatin1Char(','));
    if (parts.size() != 4) {
        return std::nullopt;
    }
    bool ok = true;
    Telemetry t;
    t.output = parts.at(0).toInt(&ok);
    if (!ok) return std::nullopt;
    t.reflected = parts.at(1).toInt(&ok);
    if (!ok) return std::nullopt;
    t.current = parts.at(2).toInt(&ok);
    if (!ok) return std::nullopt;
    t.input_raw = parts.at(3).toInt(&ok);
    if (!ok) return std::nullopt;
    return t;
}

QByteArray buildBypass(bool on)
{
    return on ? QByteArrayLiteral("21") : QByteArrayLiteral("22");
}

QByteArray buildCooling(bool on)
{
    return on ? QByteArrayLiteral("45") : QByteArrayLiteral("46");
}

QByteArray buildVoltage(bool low)
{
    return low ? QByteArrayLiteral("41") : QByteArrayLiteral("42");
}

QByteArray buildSelectAntenna(int port)
{
    return QStringLiteral("3%1").arg(port).toLatin1();
}

QByteArray buildPoll()
{
    return QByteArrayLiteral("11");
}

QByteArray buildResetHold()
{
    return QByteArrayLiteral("23");
}

}  // namespace Vkamp
}  // namespace AetherSDR
