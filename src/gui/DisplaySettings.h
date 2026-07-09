#pragma once

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace AetherSDR {

// Persistence helper for display-related UI toggles (the SmartMTR meter view
// and its options; future display-feature toggles land here as additional
// fields).
//
// Stored as a nested JSON blob under AppSettings["Display"], per the
// nested-JSON-per-feature convention (constitution Principle V).
//
// RETIRED KEYS — do not reuse. Pre-removal installs still carry these values,
// so a new feature reusing the name would inherit stale state (e.g. a
// leftover "True" force-enabling itself):
//   - "leanMode" (nested, this blob) — Lean Mode, removed with #3283's
//     mitigation retirement; ex-lean users have "True" persisted.
//   - "LeanMode" (legacy flat AppSettings key) — pre-blob spelling, was
//     migrated by the now-removed migrateLegacy().
class DisplaySettings {
public:
    // VFO meter view: false = standard S-meter, true = SmartMTR component.
    // Global (not per-slice) — see MeterViewController for the live-broadcast
    // layer that fans this choice out to every open VFO flag.
    static bool smartMtr() { return readObj().value("smartMtr").toString("False") == "True"; }

    static void setSmartMtr(bool on)
    {
        QJsonObject o = readObj();
        o["smartMtr"] = on ? QStringLiteral("True") : QStringLiteral("False");
        write(o);
    }

    // ── SmartMTR-only display options ───────────────────────────────────────
    // These apply only to the SmartMTR meter view (not the standard S-meter).
    // Persisted here, surfaced in the VFO meter-view selector; consumed by the
    // SmartMTR rendering layer via VfoWidget::pushSmartMtrOptions().

    // Extremes-speed and shown-values choices, as typed enums so consumers get
    // compile-time exhaustiveness rather than stringly-typed comparisons.
    enum class ExtremesSpeed { Slow, Medium, Fast };
    enum class MeterValues { None, Signal, Extremes };

    // What the SmartMTR meter shows while transmitting. None = keep the RX
    // signal scale (don't switch on TX); the rest swap to a TX scale for the
    // duration of TX: MicLevel (dBFS), SWR (ratio), Power (forward watts,
    // radio-aware full scale), Compression (dB). Default None. Appended values
    // keep their ordinals; deserialisation is token-based so old configs and
    // downgrades fall back to None on an unknown token.
    enum class TxMeter { None, MicLevel, SWR, Power, Compression };

    // Show the peak/trough "extremes" markers on the SmartMTR meter.
    static bool showExtremes()
    {
        return readObj().value("showExtremes").toString("False") == "True";
    }
    static void setShowExtremes(bool on)
    {
        QJsonObject o = readObj();
        o["showExtremes"] = on ? QStringLiteral("True") : QStringLiteral("False");
        write(o);
    }

    // Show the meter-type label (MIC/SWR/PWR/COMP) inside the SmartMTR hole while a
    // TX meter is active. Default False.
    static bool showTxMeterType()
    {
        return readObj().value("showTxMeterType").toString("False") == "True";
    }
    static void setShowTxMeterType(bool on)
    {
        QJsonObject o = readObj();
        o["showTxMeterType"] = on ? QStringLiteral("True") : QStringLiteral("False");
        write(o);
    }

    // How fast the extremes markers decay / track. Default Medium.
    static ExtremesSpeed extremesSpeed()
    {
        const QString s = readObj().value("extremesSpeed").toString("Medium");
        if (s == QStringLiteral("Slow")) return ExtremesSpeed::Slow;
        if (s == QStringLiteral("Fast")) return ExtremesSpeed::Fast;
        return ExtremesSpeed::Medium;
    }
    static void setExtremesSpeed(ExtremesSpeed v)
    {
        QJsonObject o = readObj();
        o["extremesSpeed"] = extremesSpeedToken(v);
        write(o);
    }

    // Which numeric value(s) to overlay on the SmartMTR meter. Default None.
    static MeterValues showValues()
    {
        const QString s = readObj().value("showValues").toString("None");
        if (s == QStringLiteral("Signal")) return MeterValues::Signal;
        if (s == QStringLiteral("Extremes")) return MeterValues::Extremes;
        return MeterValues::None;
    }
    static void setShowValues(MeterValues v)
    {
        QJsonObject o = readObj();
        o["showValues"] = meterValuesToken(v);
        write(o);
    }

    // Which meter to show while transmitting. Default None (stay on RX signal).
    static TxMeter txMeter()
    {
        const QString s = readObj().value("txMeter").toString("None");
        if (s == QStringLiteral("MicLevel")) return TxMeter::MicLevel;
        if (s == QStringLiteral("SWR")) return TxMeter::SWR;
        if (s == QStringLiteral("Power")) return TxMeter::Power;
        if (s == QStringLiteral("Compression")) return TxMeter::Compression;
        return TxMeter::None;
    }
    static void setTxMeter(TxMeter v)
    {
        QJsonObject o = readObj();
        o["txMeter"] = txMeterToken(v);
        write(o);
    }

    static QString extremesSpeedToken(ExtremesSpeed v)
    {
        switch (v) {
        case ExtremesSpeed::Slow: return QStringLiteral("Slow");
        case ExtremesSpeed::Fast: return QStringLiteral("Fast");
        case ExtremesSpeed::Medium: break;
        }
        return QStringLiteral("Medium");
    }
    static QString meterValuesToken(MeterValues v)
    {
        switch (v) {
        case MeterValues::Signal: return QStringLiteral("Signal");
        case MeterValues::Extremes: return QStringLiteral("Extremes");
        case MeterValues::None: break;
        }
        return QStringLiteral("None");
    }
    static QString txMeterToken(TxMeter v)
    {
        switch (v) {
        case TxMeter::MicLevel: return QStringLiteral("MicLevel");
        case TxMeter::SWR: return QStringLiteral("SWR");
        case TxMeter::Power: return QStringLiteral("Power");
        case TxMeter::Compression: return QStringLiteral("Compression");
        case TxMeter::None: break;
        }
        return QStringLiteral("None");
    }

private:
    static QJsonObject readObj()
    {
        const QString json =
            AppSettings::instance().value("Display", QString{}).toString();
        if (json.isEmpty()) return {};
        return QJsonDocument::fromJson(json.toUtf8()).object();
    }
    static void write(const QJsonObject& o)
    {
        auto& s = AppSettings::instance();
        s.setValue("Display",
                   QString::fromUtf8(
                       QJsonDocument(o).toJson(QJsonDocument::Compact)));
        s.save();
    }
};

} // namespace AetherSDR
