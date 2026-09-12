#include "OperaRadarImage.h"
#include <QHash>
#include <zlib.h>
#include <QVector>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace AetherSDR {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kRadius = 6378137.0;
// Bounded reader for the documented OPERA CIRRUS COG profile, not a general
// TIFF decoder. Unsupported compression/sample/CRS profiles fail explicitly.
struct Tiff {
    const QByteArray& b;
    bool ok{true};
    bool little{true};
    struct Field { quint16 type; quint32 count; quint32 offset; };
    QHash<int, Field> fields;
    quint32 integer(quint64 at, int size) {
        if (at + size > quint64(b.size())) { ok = false; return 0; }
        const auto* p = reinterpret_cast<const uchar*>(b.constData() + at);
        return size == 2 ? (little ? qFromLittleEndian<quint16>(p) : qFromBigEndian<quint16>(p))
                         : (little ? qFromLittleEndian<quint32>(p) : qFromBigEndian<quint32>(p));
    }
    double value(int tag, int index = 0) {
        if (!fields.contains(tag)) { ok = false; return 0; }
        const Field f = fields.value(tag);
        const int size = f.type == 3 ? 2 : f.type == 4 ? 4 : f.type == 12 ? 8 : 0;
        if (!size || index < 0 || quint32(index) >= f.count) { ok = false; return 0; }
        const quint64 at = quint64(f.offset) + quint64(index) * size;
        if (at + size > quint64(b.size())) { ok = false; return 0; }
        if (size != 8) { return integer(at, size); }
        const auto* p = reinterpret_cast<const uchar*>(b.constData() + at);
        const quint64 raw = little ? qFromLittleEndian<quint64>(p) : qFromBigEndian<quint64>(p);
        double result;
        std::memcpy(&result, &raw, 8);
        if (!std::isfinite(result)) { ok = false; return 0; }
        return result;
    }
    quint32 unsignedValue(int tag, int index = 0) {
        const double v = value(tag, index);
        if (v < 0 || v > std::numeric_limits<quint32>::max() || std::floor(v) != v) {
            ok = false; return 0;
        }
        return quint32(v);
    }
    bool read() {
        if (b.size() < 8 || b.size() > 16 * 1024 * 1024) { return false; }
        little = b.startsWith("II");
        if (!little && !b.startsWith("MM")) { return false; }
        if (integer(2, 2) != 42) { return false; }
        const quint32 ifd = integer(4, 4);
        const quint32 count = integer(ifd, 2);
        if (!ok || count > 128 || quint64(ifd) + 2 + quint64(count) * 12 > quint64(b.size())) { return false; }
        for (quint32 i = 0; i < count; ++i) {
            const quint64 at = quint64(ifd) + 2 + i * 12;
            const int tag = integer(at, 2);
            const quint16 type = integer(at + 2, 2);
            const quint32 n = integer(at + 4, 4);
            const int size = type == 3 ? 2 : type == 4 ? 4 : type == 12 ? 8 : 1;
            const quint32 off = quint64(n) * size <= 4 ? quint32(at + 8) : integer(at + 8, 4);
            if (fields.contains(tag) || quint64(off) + quint64(n) * size > quint64(b.size())) { return false; }
            fields.insert(tag, {type, n, off});
        }
        return ok;
    }
};
double authalicQ(double phi)
{
    constexpr double f = 1.0 / 298.257223563;
    constexpr double e2 = 2 * f - f * f;
    const double e = std::sqrt(e2), s = std::sin(phi);
    return (1 - e2) * (s / (1 - e2 * s * s)
        - std::log((1 - e * s) / (1 + e * s)) / (2 * e));
}
}

QPointF OperaRadarImage::projectLaea(double latitude, double longitude)
{
    // Ellipsoidal Lambert azimuthal equal area, EPSG method 9820.
    // OPERA: WGS84, lat_0=55, lon_0=10, x_0=1950000, y_0=-2100000.
    static const double qp = authalicQ(kPi / 2);
    static const double beta0 = std::asin(authalicQ(55 * kPi / 180) / qp);
    const double beta = std::asin(std::clamp(authalicQ(latitude * kPi / 180) / qp, -1.0, 1.0));
    static const double rq = kRadius * std::sqrt(qp / 2);
    constexpr double f = 1.0 / 298.257223563;
    constexpr double e2 = 2 * f - f * f;
    static const double sin0 = std::sin(55 * kPi / 180);
    static const double d = kRadius * std::cos(55 * kPi / 180)
        / (std::sqrt(1 - e2 * sin0 * sin0) * rq * std::cos(beta0));
    const double delta = (longitude - 10) * kPi / 180;
    const double b = rq * std::sqrt(2 / (1 + std::sin(beta0) * std::sin(beta)
        + std::cos(beta0) * std::cos(beta) * std::cos(delta)));
    return {1950000 + b * d * std::cos(beta) * std::sin(delta),
        -2100000 + b / d * (std::cos(beta0) * std::sin(beta)
            - std::sin(beta0) * std::cos(beta) * std::cos(delta))};
}

QColor OperaRadarImage::colorForDbz(float dbz)
{
    if (!std::isfinite(dbz) || dbz < -30 || dbz > 95) { return Qt::transparent; }
    // Blue through cyan, green, yellow and red, fading to white at 80 dBZ.
    const float v = std::clamp((dbz + 30) / 110, 0.0F, 1.0F);
    return QColor::fromHsvF((1 - v) * 0.67, v > 0.9 ? (1 - v) * 10 : 1, 1);
}

OperaRadarImage OperaRadarImage::decode(const QByteArray& bytes)
{
    const auto failure = [](const char* message) { return OperaRadarImage{{}, {}, QString::fromLatin1(message)}; };
    Tiff t{bytes, true, true, {}};
    if (!t.read()) { return failure("Invalid OPERA TIFF header"); }
    constexpr int width = 3800, height = 4400, tw = 512, th = 512;
    if (t.unsignedValue(256) != width || t.unsignedValue(257) != height
        || t.unsignedValue(322) != tw || t.unsignedValue(323) != th
        || t.value(258) != 32 || t.value(339) != 3 || t.value(277) != 2
        || t.value(259) != 8 || t.value(284) != 1 || t.value(317) != 1) {
        return failure("Unsupported OPERA TIFF sample or compression profile");
    }
    // Validate georeferencing before assigning the published projection.
    QHash<int, double> keys;
    if (t.fields.value(34735).type != 3) { return failure("Invalid OPERA CRS key type"); }
    const int keyCount = int(t.value(34735, 3));
    if (keyCount > 64) { return failure("Invalid OPERA CRS keys"); }
    for (int i = 0; i < keyCount; ++i) {
        const int key = int(t.value(34735, 4 + i * 4));
        const int location = int(t.value(34735, 5 + i * 4));
        const int n = int(t.value(34735, 6 + i * 4));
        const int offset = int(t.value(34735, 7 + i * 4));
        if (n == 1 && (location == 0 || location == 34736)) {
            keys[key] = location == 0 ? offset : t.value(34736, offset);
        }
    }
    if (!t.ok || keys.value(3075) != 10 || keys.value(3076) != 9001
        || keys.value(3088) != 10 || keys.value(3089) != 55
        || keys.value(3082) != 1950000 || keys.value(3083) != -2100000
        || std::abs(keys.value(2057) - kRadius) > 0.01
        || std::abs(keys.value(2059) - 298.257223563) > 0.001
        || t.value(33550) != 1000 || t.value(33550, 1) != 1000
        || t.value(33922) != 0 || t.value(33922, 1) != 0
        || std::abs(t.value(33922, 3) + 500) > 1 || std::abs(t.value(33922, 4) - 500) > 1) {
        return failure("Unsupported OPERA georeferencing");
    }
    if (!t.ok) { return failure("Invalid OPERA metadata values"); }
    const int nx = (width + tw - 1) / tw, ny = (height + th - 1) / th;
    if (t.fields.value(324).count != quint32(nx * ny) || t.fields.value(325).count != quint32(nx * ny)) {
        return failure("Invalid OPERA tile table");
    }
    QVector<float> reflectivity(width * height, std::numeric_limits<float>::quiet_NaN());
    for (int tile = 0; tile < nx * ny; ++tile) {
        const quint32 offset = t.unsignedValue(324, tile), length = t.unsignedValue(325, tile);
        if (!t.ok || length > 3 * 1024 * 1024 || quint64(offset) + length > quint64(bytes.size())) {
            return failure("Invalid OPERA tile bounds");
        }
        QByteArray raw(tw * th * 8, char(0));
        uLongf decodedSize = raw.size();
        if (uncompress(reinterpret_cast<Bytef*>(raw.data()), &decodedSize,
                reinterpret_cast<const Bytef*>(bytes.constData() + offset), length) != Z_OK
            || decodedSize != uLongf(raw.size())) { return failure("Invalid OPERA tile payload"); }
        for (int y = 0; y < th && tile / nx * th + y < height; ++y) {
            for (int x = 0; x < tw && tile % nx * tw + x < width; ++x) {
                const auto* p = reinterpret_cast<const uchar*>(raw.constData() + (y * tw + x) * 8);
                const quint32 bits = t.little ? qFromLittleEndian<quint32>(p) : qFromBigEndian<quint32>(p);
                float value; std::memcpy(&value, &bits, 4);
                reflectivity[(tile / nx * th + y) * width + tile % nx * tw + x] = value;
            }
        }
    }
    // Native display raster, bounded independently of requested viewport size.
    // Alpha preserves no-data/undetect. Numeric dBZ remains in the legend.
    const QRectF bounds(-5009377.0857, 3503549.8435, 11131949.0793, 12035161.2528);
    QImage image(2560, 3072, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    const double originX = t.value(33922, 3), originY = t.value(33922, 4);
    for (int y = 0; y < image.height(); ++y) {
        const double northing = bounds.bottom() - (y + 0.5) / image.height() * bounds.height();
        const double lat = (2 * std::atan(std::exp(northing / kRadius)) - kPi / 2) * 180 / kPi;
        uchar* row = image.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            const double lon = (bounds.left() + (x + 0.5) / image.width() * bounds.width()) / kRadius * 180 / kPi;
            const QPointF p = projectLaea(lat, lon);
            const int sx = int(std::floor((p.x() - originX) / 1000));
            const int sy = int(std::floor((originY - p.y()) / 1000));
            if (sx < 0 || sx >= width || sy < 0 || sy >= height) { continue; }
            const float dbz = reflectivity[sy * width + sx];
            if (!std::isfinite(dbz) || dbz < -30 || dbz > 95) { continue; }
            const QColor c = colorForDbz(dbz);
            row[x * 4] = c.red(); row[x * 4 + 1] = c.green();
            row[x * 4 + 2] = c.blue(); row[x * 4 + 3] = 255;
        }
    }
    return {image, bounds, {}};
}
}
