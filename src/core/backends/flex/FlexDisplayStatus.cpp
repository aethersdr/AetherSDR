#include "FlexDisplayStatus.h"

#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <limits>

namespace AetherSDR {
namespace {

std::optional<quint32> identity(QString text, bool hexadecimal = false)
{
    if (text.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        hexadecimal = true;
        text.remove(0, 2);
    }
    hexadecimal = hexadecimal || text.contains(QRegularExpression(QStringLiteral("[a-fA-F]")));
    if (text.isEmpty() || text.size() > (hexadecimal ? 8 : 10)
        || !std::all_of(text.cbegin(), text.cend(), [](QChar c) {
            return (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f')
                || (c >= u'A' && c <= u'F');
        })) {
        return std::nullopt;
    }
    bool ok = false;
    const quint32 value = text.toUInt(&ok, hexadecimal ? 16 : 10);
    return ok ? std::optional(value) : std::nullopt;
}

QString resourceId(quint32 id)
{
    return id == 0 ? QString() : QStringLiteral("0x%1").arg(id, 8, 16, QLatin1Char('0'));
}

} // namespace

std::optional<DisplayObservation> decodeFlexDisplayStatus(
    const QString& object, const QMap<QString, QString>& fields,
    const std::shared_ptr<const std::atomic_bool>& transportEpoch)
{
    // Bound retained strings as well as record count; unknown fields never
    // escape the decoder. Identity errors reject the whole observation.
    if (object.size() > 80 || fields.size() > 128) {
        return std::nullopt;
    }
    qsizetype size = 0;
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
        size += it.key().size() + it.value().size();
        if (size > 16384) { return std::nullopt; }
    }
    static const QRegularExpression pattern(
        QStringLiteral("^display (pan|waterfall) ([^ ]+)( removed)?$"));
    const QRegularExpressionMatch match = pattern.match(object);
    if (!match.hasMatch()) { return std::nullopt; }
    const std::optional<quint32> id = identity(match.captured(2), true);
    if (!id || *id == 0) { return std::nullopt; }

    DisplayObservation result;
    result.kind = match.captured(1) == QLatin1String("pan")
        ? DisplayObservation::Kind::Panadapter : DisplayObservation::Kind::Waterfall;
    result.id = resourceId(*id);
    result.transportEpoch = transportEpoch;
    result.removed = !match.captured(3).isEmpty();
    if (fields.contains(QStringLiteral("removed"))) {
        if (fields.value(QStringLiteral("removed")) != QLatin1String("1")) {
            return std::nullopt;
        }
        result.removed = true;
    }
    if (fields.contains(QStringLiteral("client_handle"))) {
        result.ownerHandle = identity(fields.value(QStringLiteral("client_handle")));
        if (!result.ownerHandle) { return std::nullopt; }
    }
    const QString pairKey = result.kind == DisplayObservation::Kind::Panadapter
        ? QStringLiteral("waterfall") : QStringLiteral("panadapter");
    if (fields.contains(pairKey)) {
        const std::optional<quint32> paired = identity(fields.value(pairKey));
        if (!paired) { return std::nullopt; }
        result.pairedId = resourceId(*paired);
    }
    if (result.removed) { return result; }

    DisplayValues& v = result.values;
    const auto real = [&fields](const char* key, std::optional<double>& target) {
        const auto it = fields.constFind(QLatin1String(key));
        if (it == fields.cend()) { return; }
        bool ok = false;
        const double value = it->toDouble(&ok);
        if (ok && std::isfinite(value)
            && std::abs(value) <= std::numeric_limits<float>::max()) {
            target = value;
        }
    };
    const auto integer = [&fields](const char* key, std::optional<int>& target,
                                  int minimum = std::numeric_limits<int>::min(),
                                  int maximum = std::numeric_limits<int>::max()) {
        const auto it = fields.constFind(QLatin1String(key));
        if (it == fields.cend()) { return; }
        bool ok = false;
        const int value = it->toInt(&ok);
        if (ok && value >= minimum && value <= maximum) { target = value; }
    };
    const auto flag = [&fields](const char* key, std::optional<bool>& target, uint maximum = 1) {
        const auto it = fields.constFind(QLatin1String(key));
        if (it == fields.cend()) { return; }
        bool ok = false;
        const uint value = it->toUInt(&ok);
        if (ok && value <= maximum) { target = value != 0; }
    };
    const auto string = [&fields](const char* key, std::optional<QString>& target) {
        const auto it = fields.constFind(QLatin1String(key));
        if (it != fields.cend() && it->size() <= 256) { target = *it; }
    };
    real("center", v.centerMhz);
    real("bandwidth", v.bandwidthMhz);
    if (v.centerMhz && (*v.centerMhz < 0 || *v.centerMhz > 9'007'199'254.0)) { v.centerMhz.reset(); }
    if (v.bandwidthMhz && (*v.bandwidthMhz <= 0 || *v.bandwidthMhz > 9'007'199'254.0)) { v.bandwidthMhz.reset(); }
    if (result.kind == DisplayObservation::Kind::Waterfall) {
        integer("line_duration", v.waterfallRate);
        return result;
    }
    real("min_dbm", v.minDbm);
    real("max_dbm", v.maxDbm);
    real("noise_floor_range", v.noiseFloorRange);
    integer("rfgain", v.rfGain);
    integer("fps", v.fps);
    integer("average", v.average);
    integer("daxiq_channel", v.daxIqChannel, 0);
    integer("wnb_level", v.wnbLevel, 0, 100);
    integer("x_pixels", v.xPixels, 0);
    integer("y_pixels", v.yPixels, 0);
    flag("wide", v.wide);
    flag("loopa", v.loopA);
    flag("loopb", v.loopB);
    flag("weighted_average", v.weightedAverage, 255);
    flag("band_zoom", v.bandZoom);
    flag("segment_zoom", v.segmentZoom);
    flag("wnb", v.wnb);
    flag("wnb_updating", v.wnbUpdating);
    string("pre", v.preamp);
    string("rxant", v.rxAntenna);
    if (fields.contains(QStringLiteral("ant_list"))) {
        const QString raw = fields.value(QStringLiteral("ant_list"));
        if (raw.size() <= 1024) {
            v.antennas = raw.split(QLatin1Char(','), Qt::SkipEmptyParts);
        }
    }
    return result;
}

} // namespace AetherSDR
