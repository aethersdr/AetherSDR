#pragma once

#include <QMetaType>
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
    int     sourceIndex{0};
    QString name;         // "LEVEL", "FWDPWR", "SWR", "PATEMP", ...
    QString unit;         // "dBm", "dB", "dBFS", "SWR", "Volts", "Amps", "degC", "degF", "Watts", "Percent"
    double  low{0.0};
    double  high{0.0};
    QString description;
};

}  // namespace AetherSDR

// Registered so MeterDef can ride IRadioBackend::meterDefined across a queued
// connection and be captured by QSignalSpy in tests.
Q_DECLARE_METATYPE(AetherSDR::MeterDef)
