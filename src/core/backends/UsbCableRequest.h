#pragma once

#include "core/backends/SetupTextPolicy.h"

#include <QMetaType>
#include <QString>
#include <cmath>
#include <variant>

namespace AetherSDR {

struct UsbCableRequest {
    enum class Field {
        Type, Name, Enabled, AutoReport, Source, SerialSpeed, DataBits,
        Parity, StopBits, FlowControl, ActiveHigh, Band, Preamp,
        SourceRxAntenna, SourceTxAntenna, SourceSlice, Output,
        LowFrequencyMhz, HighFrequencyMhz, PttDependent,
        PttDelayMs, TxDelayMs, Remove, Count
    };
    QString serial;
    int bit{-1}; // -1 is the cable, 0..7 is one independent bit.
    Field field;
    std::variant<std::monostate, bool, int, double, QString> value;
    bool valid() const
    {
        if (serial.isEmpty() || serial.size() > 128 || bit < -1 || bit > 7
            || field < Field::Type || field >= Field::Count) { return false; }
        for (const QChar ch : serial) {
            if (!ch.isPrint() || ch.isSpace() || ch == QLatin1Char('=') || ch == QLatin1Char('|')) { return false; }
        }
        switch (field) {
        case Field::Remove: return bit == -1 && std::holds_alternative<std::monostate>(value);
        case Field::Enabled: case Field::AutoReport: case Field::ActiveHigh:
        case Field::Preamp: case Field::PttDependent: return std::holds_alternative<bool>(value);
        case Field::SerialSpeed: case Field::DataBits: case Field::StopBits:
        case Field::PttDelayMs: case Field::TxDelayMs: {
            const int* number = std::get_if<int>(&value);
            return number && *number >= 0;
        }
        case Field::LowFrequencyMhz: case Field::HighFrequencyMhz: {
            const double* mhz = std::get_if<double>(&value);
            return mhz && std::isfinite(*mhz) && *mhz >= 0;
        }
        default: {
            const QString* text = std::get_if<QString>(&value);
            return text && setupTextValid(*text);
        }
        }
    }
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::UsbCableRequest)
