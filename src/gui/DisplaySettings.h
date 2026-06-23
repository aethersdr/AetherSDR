#pragma once

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace AetherSDR {

// Persistence helper for display-related UI toggles (#3283 Lean Mode is the
// first; future display-feature toggles like frameless / theme variants land
// here as additional fields).
//
// Stored as a nested JSON blob under AppSettings["Display"], per the
// nested-JSON-per-feature convention (constitution Principle V).  The legacy
// flat key "LeanMode" is migrated into this blob by migrateLegacy(), called
// once at startup (before the first read), so existing users keep their
// behavior.
class DisplaySettings {
public:
    static bool leanMode() { return readObj().value("leanMode").toString("False") == "True"; }

    static void setLeanMode(bool on)
    {
        QJsonObject o = readObj();
        o["leanMode"] = on ? QStringLiteral("True") : QStringLiteral("False");
        write(o);
    }

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
    // signal scale (don't switch on TX); MicLevel = swap to the mic-level (dBFS)
    // scale for the duration of TX. Default None.
    enum class TxMeter { None, MicLevel };

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
        case TxMeter::None: break;
        }
        return QStringLiteral("None");
    }

    // One-shot migration from the legacy "LeanMode" flat key.  Run at app
    // startup before any caller reads the new blob.  Safe to call repeatedly:
    // returns immediately if the new blob already exists.
    static void migrateLegacy()
    {
        auto& s = AppSettings::instance();
        if (s.contains("Display")) return;
        const bool legacyLean =
            s.value("LeanMode", "False").toString() == "True";
        QJsonObject o;
        o["leanMode"] = legacyLean ? QStringLiteral("True") : QStringLiteral("False");
        write(o);
        // Leave the legacy flat key in place — harmless after migration, and a
        // future cleanup PR can drop it once we're confident no other reader
        // still touches it.
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
