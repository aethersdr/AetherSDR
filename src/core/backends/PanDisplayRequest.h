#pragma once

#include <QMetaType>
#include <QSet>
#include <QString>
#include <cmath>
#include <optional>
#include <variant>

namespace AetherSDR {

struct PanGeometryRequest {
    std::optional<double> centerMhz;
    std::optional<double> bandwidthMhz;
    enum class Intent { Range, Drag };
    Intent intent{Intent::Range};
};
struct PanLevelRange { double minimum; double maximum; };
struct PanPixelDimensions { int width; int height; };

enum class PanDisplayField {
    Geometry, Band, LevelRange, Pixels, SpectrumPoints, Average, FrameRate,
    WeightedAverage, RfGain, Wnb, WnbLevel, LoopA, LoopB, NoiseFloorPosition, NoiseFloorEnabled,
    WaterfallRate, WaterfallColorGain, WaterfallBlackLevel, WaterfallAutoBlack,
    WaterfallDefaults, Count
};
enum class PanGeometryComponent { Center, Bandwidth };

struct PanDisplayControl {
    QSet<PanDisplayField> fields;
    QSet<PanGeometryComponent> geometry;
    bool supports(PanDisplayField field) const { return fields.contains(field); }
    bool declaresSpan() const { return geometry.contains(PanGeometryComponent::Bandwidth); }
};

struct PanDisplayRequest {
    PanDisplayField field{PanDisplayField::Count};
    std::variant<int, bool, QString, PanGeometryRequest, PanLevelRange, PanPixelDimensions> value;

    bool targetsWaterfall() const
    {
        return field >= PanDisplayField::WaterfallRate && field <= PanDisplayField::WaterfallDefaults;
    }
    bool valid() const
    {
        using Field = PanDisplayField;
        if (field == Field::Geometry) {
            const auto* geometry = std::get_if<PanGeometryRequest>(&value);
            return geometry && (geometry->centerMhz || geometry->bandwidthMhz)
                && (!geometry->centerMhz || (std::isfinite(*geometry->centerMhz)
                    && *geometry->centerMhz >= 0.0 && *geometry->centerMhz <= 1.0e6))
                && (!geometry->bandwidthMhz || (std::isfinite(*geometry->bandwidthMhz)
                    && *geometry->bandwidthMhz > 0.0 && *geometry->bandwidthMhz <= 1.0e6));
        }
        if (field == Field::LevelRange) {
            const auto* range = std::get_if<PanLevelRange>(&value);
            return range && std::isfinite(range->minimum) && std::isfinite(range->maximum)
                && range->minimum >= -1000.0 && range->maximum <= 1000.0
                && range->minimum < range->maximum;
        }
        if (field == Field::Pixels) {
            const auto* pixels = std::get_if<PanPixelDimensions>(&value);
            return pixels && pixels->width > 0 && pixels->width <= 65535
                && pixels->height > 0 && pixels->height <= 65535;
        }
        if (field == Field::Band) {
            const QString* band = std::get_if<QString>(&value);
            if (!band || band->isEmpty() || band->size() > 64) { return false; }
            for (const QChar character : *band) {
                if (character.unicode() > 127 || (!character.isLetterOrNumber()
                    && character != QLatin1Char('_') && character != QLatin1Char('-')
                    && character != QLatin1Char('.'))) { return false; }
            }
            return true;
        }
        if (field == Field::WaterfallDefaults) {
            const bool* initialize = std::get_if<bool>(&value);
            return initialize && *initialize;
        }
        if (field == Field::WeightedAverage || field == Field::Wnb || field == Field::LoopA || field == Field::LoopB
            || field == Field::NoiseFloorEnabled || field == Field::WaterfallAutoBlack) {
            return std::holds_alternative<bool>(value);
        }
        const int* number = std::get_if<int>(&value);
        if (!number) { return false; }
        switch (field) {
        case Field::SpectrumPoints: return *number > 0 && *number <= 65535;
        case Field::FrameRate: return *number > 0 && *number <= 100;
        case Field::Average: case Field::WnbLevel: case Field::WaterfallColorGain:
        case Field::WaterfallBlackLevel: return *number >= 0 && *number <= 100;
        case Field::WaterfallRate: return *number >= 1 && *number <= 100;
        case Field::RfGain: return *number >= -1000 && *number <= 1000;
        case Field::NoiseFloorPosition: return *number >= 0 && *number <= 100;
        default: return false;
        }
    }
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::PanDisplayRequest)
