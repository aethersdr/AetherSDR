#pragma once

#include <QMetaType>
#include <QtGlobal>
#include <QString>

namespace AetherSDR {

// A single meter definition decoded from the radio's meter-status wire message
// (aetherd RFC 2.3 — MeterModel touchpoint). Pure data: it lives in the core
// backend layer (not a model header) so the vendor-neutral IRadioBackend can
// carry it on meterDefined() without depending on any model — the backend fills
// it, RadioModel hands it straight to MeterModel::defineMeter(). Fields not
// reported by the wire keep their defaults (present-only on the decode side).
//
// The meter *values* stream separately on the VITA-49 data plane; this is only
// the catalog/definition.
struct MeterDef {
    int     index{-1};
    QString source;       // "SLC", "RAD", "AMP", "TX", ...
    // The wire value is a uint32; a Flex amp/tuner handle above INT_MAX is kept
    // bit for bit, so publish it through publishedSourceIndex().
    int     sourceIndex{0};
    QString name;         // "LEVEL", "FWDPWR", "SWR", "PATEMP", ...
    QString unit;         // "dBm", "dB", "dBFS", "SWR", "Volts", "Amps", "degC", "degF", "Watts", "Percent"
    double  low{0.0};
    double  high{0.0};
    QString description;
    QString unavailableReason; // Empty unless measurement is explicitly unavailable.

    [[nodiscard]] qint64 publishedSourceIndex() const
    {
        return static_cast<quint32>(sourceIndex);
    }
};

}  // namespace AetherSDR

// Registered so MeterDef can ride IRadioBackend::meterDefined across a queued
// connection and be captured by QSignalSpy in tests.
Q_DECLARE_METATYPE(AetherSDR::MeterDef)
