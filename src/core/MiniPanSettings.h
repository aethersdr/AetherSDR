#pragma once

// Mini-pan window persistence. Per Constitution Principle V the feature's
// configuration is ONE nested JSON object under the single AppSettings key
// "MiniPan" (the AetherClockSettings / AutomationBridgeSettings pattern) —
// never a spray of flat keys across the shared namespace. One place to
// default, one to migrate, one value written atomically.
//
// Everything here is client-side display state. Nothing the radio owns is
// persisted (Constitution Principle III): the pan's centre, bandwidth and
// dBm range are re-derived from the followed slice and the radio's own
// echo on every connect.

#include <QJsonObject>
#include <QByteArray>

namespace AetherSDR {

class MiniPanSettings {
public:
    // Base64 QWidget::saveGeometry() blob; empty when never placed.
    static QByteArray geometryBase64();
    static void setGeometryBase64(const QByteArray& base64);

    // Window open at last shutdown — drives the startup re-open.
    static bool open();          // default false
    static void setOpen(bool on);

    // Total displayed span in kHz: 10.0 (±5 kHz) or 20.0 (±10 kHz).
    // Anything else in the store falls back to the ±5 kHz default.
    static double spanKHz();     // default 10.0
    static void setSpanKHz(double kHz);

    static bool alwaysOnTop();   // default false
    static void setAlwaysOnTop(bool on);

    // The one legal set of spans, shared by the settings validator and the
    // window's context menu so they cannot drift apart.
    static constexpr double kSpanNarrowKHz = 10.0;   // ±5 kHz
    static constexpr double kSpanWideKHz   = 20.0;   // ±10 kHz

private:
    static QJsonObject readObj();
    static void write(const QJsonObject& o);
};

} // namespace AetherSDR
