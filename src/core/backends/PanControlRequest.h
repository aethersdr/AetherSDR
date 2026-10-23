#pragma once

#include <QSet>
#include <QMetaType>
#include <QString>
#include <variant>

namespace AetherSDR {

enum class PanControlField { BandZoom, SegmentZoom, ReceiveAntenna, Count };
struct PanControl {
    QSet<PanControlField> fields;
    bool supports(PanControlField field) const { return fields.contains(field); }
};
struct PanControlRequest {
    PanControlField field{PanControlField::Count};
    std::variant<bool, QString> value;
    bool valid() const
    {
        if (field == PanControlField::BandZoom || field == PanControlField::SegmentZoom) {
            return std::holds_alternative<bool>(value);
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
