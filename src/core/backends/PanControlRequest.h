#pragma once

#include <QSet>
#include <QMetaType>
#include <QString>
#include <cmath>
#include <variant>

namespace AetherSDR {

enum class PanControlField { BandZoom, SegmentZoom, ReceiveAntenna, NoiseFloorRange, Count };
// How far the measured noise floor may move, in dB, before a radio scaling the
// pan to its noise floor repositions it (SmartSDR 4.3.16 release notes).
inline constexpr double kNoiseFloorRangeMaximumDb = 10.0;
inline constexpr double kNoiseFloorRangeStepDb = 0.5;
struct PanControl {
    QSet<PanControlField> fields;
    bool supports(PanControlField field) const { return fields.contains(field); }
};
struct PanControlRequest {
    PanControlField field{PanControlField::Count};
    std::variant<bool, QString, double> value;
    bool valid() const
    {
        if (field == PanControlField::BandZoom || field == PanControlField::SegmentZoom) {
            return std::holds_alternative<bool>(value);
        }
        if (field == PanControlField::NoiseFloorRange) {
            const double* db = std::get_if<double>(&value);
            if (!db || !std::isfinite(*db) || *db < 0.0 || *db > kNoiseFloorRangeMaximumDb) {
                return false;
            }
            const double steps = *db / kNoiseFloorRangeStepDb;
            return std::abs(steps - std::round(steps)) < 1e-6;
        }
        const QString* text = std::get_if<QString>(&value);
        if (field != PanControlField::ReceiveAntenna || !text || text->isEmpty() || text->size() > 64) {
            return false;
        }
        for (const QChar ch : *text) {
            if (ch.unicode() > 127 || (!ch.isLetterOrNumber()
                && ch != QLatin1Char('_') && ch != QLatin1Char('-') && ch != QLatin1Char('/'))) { return false; }
        }
        return true;
    }
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::PanControlRequest)
