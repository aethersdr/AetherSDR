#include "VkampProtocol.h"

#include <algorithm>
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
// Documentation only -- no code branches on this. It records the lowest raw
// count the output fit has ever been confirmed against on real hardware: the
// companion project's own lowest confirmed point was raw~175.6 -> true 133W,
// and a later live capture on this codebase's own hardware added 121 frames
// of a steady 1W-drive transmission clustered at raw 157-166 (mode 165),
// user-confirmed ~111W true. The same capture's tail-off through 134 and 102
// as the transmission ended is transient, not a steady point.
constexpr float kOutputLowestConfirmedRaw = 157.0f;

constexpr float kCurrentCalA = 0.4052965787643523f;
constexpr float kCurrentCalB = 0.3049154652689805f;

constexpr float kReflectedCalA = 0.005033946087463804f;
constexpr float kReflectedCalB = -0.09293657447890188f;
constexpr float kReflectedCalC = 2.4337837601262087f;

constexpr float kInputCalA = 0.0008018661524991072f;
constexpr float kInputCalB = 0.1595133745143524f;
constexpr float kInputCalC = 1.004316538777762f;

// One rule for every quadratic power curve: a least-squares fit is only
// meaningful where increasing, so
//   raw >= vertex : evaluate the quadratic directly.
//   raw <  vertex : linear taper from 0 W at 0 counts to the curve's value at
//                   the vertex (continuous, monotonic, exactly 0 at raw 0).
// Output follows the quadratic down to its vertex (~1.3 W/count near raw 160)
// rather than a hard floor, so a 1-count ADC dip doesn't jump the reading.
// Reflected's curve bottoms at ~2.0 W (vertex raw ~9.2), so the taper is what
// lets a matched load read 0 W, and swr() needs no special case. Input's
// vertex is at negative raw (kInputCalB > 0), so the taper never runs for it.
float calibratedPower(float raw, float a, float b, float c)
{
    const auto curve = [&](float r) { return std::max(0.0f, a * r * r + b * r + c); };
    if (raw <= 0.0f) {
        return 0.0f;
    }
    const float vertexRaw = -b / (2.0f * a);
    if (vertexRaw <= 0.0f || raw >= vertexRaw) {
        return curve(raw);
    }
    return curve(vertexRaw) * (raw / vertexRaw);
}

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
    return calibratedPower(static_cast<float>(output), kOutputCalA, kOutputCalB, kOutputCalC);
}

float Telemetry::reflectedWatts() const
{
    return calibratedPower(static_cast<float>(reflected),
                           kReflectedCalA, kReflectedCalB, kReflectedCalC);
}

float Telemetry::currentAmps() const
{
    return std::max(0.0f, static_cast<float>(current) * kCurrentCalA + kCurrentCalB);
}

float Telemetry::inputWatts() const
{
    return calibratedPower(static_cast<float>(input_raw), kInputCalA, kInputCalB, kInputCalC);
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
