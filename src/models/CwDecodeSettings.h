#pragma once

#include "core/AppSettings.h"
#include "CwRxModel.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <optional>

namespace AetherSDR {

// Persistence helper for the two independent CW decode toggles (#2417).
//
// Stored as a nested JSON blob under AppSettings["CwDecoder"], per the
// nested-JSON-per-feature convention (constitution Principle V).  The
// legacy flat key "CwDecodeOverlay" is migrated into this blob on first
// read so existing users keep their behavior — its old value becomes the
// initial value of the rx toggle.
class CwDecodeSettings {
public:
    static QString backend() { return readObj().value("backend").toString("ggmorse"); }
    static bool neuralSelected()
    {
        const QString key = backend();
        return key != QLatin1String("ggmorse") && CwRxModel::availableBackends().contains(key);
    }
    static void setBackend(const QString& backend)
    {
        QJsonObject o = readObj();
        o["backend"] = backend;
        ensureToggles(o);
        write(o);
    }
    static bool rxEnabled() { return readObj().value("rx").toString("True") == "True"; }
    static bool txEnabled() { return readObj().value("tx").toString("False") == "True"; }
    static bool liveContactsEnabled()
    {
        return readObj().value("liveContacts").toString("False") == "True";
    }
    static QByteArray liveContactsGeometry()
    {
        return QByteArray::fromBase64(
            readObj().value("liveContactsGeometry").toString().toLatin1());
    }
    static void setLiveContactsEnabled(bool on, const QByteArray& geometry = {})
    {
        QJsonObject o = readObj();
        o["liveContacts"] = on ? QStringLiteral("True") : QStringLiteral("False");
        if (!geometry.isEmpty()) {
            o["liveContactsGeometry"] = QString::fromLatin1(geometry.toBase64());
        }
        ensureToggles(o);
        write(o);
    }

    static void setRxEnabled(bool on)
    {
        QJsonObject o = readObj();
        o["rx"] = on ? QStringLiteral("True") : QStringLiteral("False");
        if (!o.contains("tx"))
            o["tx"] = QStringLiteral("False");
        write(o);
    }
    static void setTxEnabled(bool on)
    {
        QJsonObject o = readObj();
        o["tx"] = on ? QStringLiteral("True") : QStringLiteral("False");
        if (!o.contains("rx"))
            o["rx"] = QStringLiteral("True");
        write(o);
    }

    // True if either toggle is on — used to decide if the CW text panel
    // should be visible at all (regardless of which direction is fed).
    static bool anyEnabled() { return rxEnabled() || txEnabled(); }

    // Decoded-text display font size (px) and panel height (px), persisted
    // so operators can tune readability and history depth (#3628).  Stored
    // in the same nested blob alongside the rx/tx toggles.
    static int  fontPx()      { return readObj().value("fontPx").toInt(13); }
    static int  panelHeight() { return readObj().value("panelHeight").toInt(80); }

    static void setFontPx(int px)
    {
        QJsonObject o = readObj();
        o["fontPx"] = px;
        ensureToggles(o);
        write(o);
    }
    static void setPanelHeight(int h)
    {
        QJsonObject o = readObj();
        o["panelHeight"] = h;
        ensureToggles(o);
        write(o);
    }

    // Decoder pitch (Hz) and speed (WPM) search ranges, shared by every pan
    // and kept across restarts.  Reads clamp to the panel sliders' spans and
    // fall back to the default when a value is missing, non-numeric or
    // inverted, so a stale or hand-edited blob never reaches the decoder.
    struct Range { int low; int high; };
    static constexpr Range kPitchSpan{300, 1200};
    static constexpr Range kPitchDefault{500, 700};
    static constexpr Range kSpeedSpan{5, 60};
    static constexpr Range kSpeedDefault{15, 40};

    static Range pitchRange() { return readRange("pitchLow", "pitchHigh", kPitchSpan, kPitchDefault); }
    static Range speedRange() { return readRange("speedLow", "speedHigh", kSpeedSpan, kSpeedDefault); }
    static void setPitchRange(int low, int high) { writeRange("pitchLow", "pitchHigh", low, high); }
    static void setSpeedRange(int low, int high) { writeRange("speedLow", "speedHigh", low, high); }

    // The pitch range centred on the CW pitch: 300 Hz wide, shifted to stay
    // inside the slider span (a pitch beyond the span gets the edge window).
    static constexpr int kPitchHalfWidth = 150;
    static constexpr Range pitchRangeAround(int pitchHz)
    {
        const int width = 2 * kPitchHalfWidth;
        const int low = std::clamp(pitchHz - kPitchHalfWidth, kPitchSpan.low, kPitchSpan.high - width);
        return Range{low, low + width};
    }

    // The range to apply for CW pitch `pitchHz`, or nothing to keep `current`:
    // a range that already contains the pitch is kept (it may be the operator's),
    // one that misses it is re-centred on the pitch.
    static constexpr std::optional<Range> pitchRangeFor(int pitchHz, Range current)
    {
        if (current.low <= pitchHz && pitchHz <= current.high) return std::nullopt;
        return pitchRangeAround(pitchHz);
    }

    // One-shot migration from the legacy "CwDecodeOverlay" flat key.  Run
    // at startup before any caller reads the new blob.  Safe to call
    // repeatedly: returns immediately if the new blob already exists.
    static void migrateLegacy()
    {
        auto& s = AppSettings::instance();
        if (s.contains("CwDecoder")) return;
        const bool legacyRx =
            s.value("CwDecodeOverlay", "True").toString() == "True";
        QJsonObject o;
        o["rx"] = legacyRx ? QStringLiteral("True") : QStringLiteral("False");
        o["tx"] = QStringLiteral("False");
        write(o);
    }

private:
    // Keep the rx/tx toggles present when writing display-only keys so a
    // fontPx/panelHeight change never drops a user's enable state.
    static void ensureToggles(QJsonObject& o)
    {
        if (!o.contains("rx")) o["rx"] = QStringLiteral("True");
        if (!o.contains("tx")) o["tx"] = QStringLiteral("False");
    }
    static Range readRange(const char* lowKey, const char* highKey, Range span, Range fallback)
    {
        const QJsonObject o = readObj();
        const QJsonValue low = o.value(QLatin1String(lowKey));
        const QJsonValue high = o.value(QLatin1String(highKey));
        if (!low.isDouble() || !high.isDouble()) return fallback;
        const int lo = std::clamp(low.toInt(), span.low, span.high);
        const int hi = std::clamp(high.toInt(), span.low, span.high);
        return lo <= hi ? Range{lo, hi} : fallback;
    }
    static void writeRange(const char* lowKey, const char* highKey, int low, int high)
    {
        QJsonObject o = readObj();
        if (o.value(QLatin1String(lowKey)).toInt(-1) == low
            && o.value(QLatin1String(highKey)).toInt(-1) == high) {
            return;
        }
        o[QLatin1String(lowKey)] = low;
        o[QLatin1String(highKey)] = high;
        ensureToggles(o);
        write(o);
    }
    static QJsonObject readObj()
    {
        const QString json =
            AppSettings::instance().value("CwDecoder", QString{}).toString();
        if (json.isEmpty()) return {};
        return QJsonDocument::fromJson(json.toUtf8()).object();
    }
    static void write(const QJsonObject& o)
    {
        auto& s = AppSettings::instance();
        s.setValue("CwDecoder",
                   QString::fromUtf8(
                       QJsonDocument(o).toJson(QJsonDocument::Compact)));
        s.save();
    }
};

} // namespace AetherSDR
