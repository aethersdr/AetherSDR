#pragma once

// Mini-pan persistence. Per Constitution Principle V the feature's
// configuration is ONE nested JSON object under the single AppSettings key
// "MiniPan" (the AetherClockSettings / AutomationBridgeSettings pattern) —
// never a spray of flat keys across the shared namespace.
//
// Only ONE field is genuinely feature-owned: the ±5/±10 kHz span. Everything
// the standalone window used to persist by hand — geometry, open state,
// always-on-top — now belongs to the container framework the applet is wrapped
// in (ContainerManager / FloatingContainerWindow), which already owns that
// state for every other applet. Duplicating it here would be a second source of
// truth for the same window.
//
// Nothing the radio owns is persisted (Constitution Principle III): the pan's
// centre, bandwidth and dBm range are re-derived from the followed slice and
// the radio's own echo on every connect.

#include <QJsonObject>

namespace AetherSDR {

class MiniPanSettings {
public:
    // Total displayed span in kHz: 10.0 (±5 kHz) or 20.0 (±10 kHz).
    // Anything else in the store falls back to the ±5 kHz default, so a
    // hand-edited value can never reach "display pan set … bandwidth=".
    static double spanKHz();     // default 10.0
    static void setSpanKHz(double kHz);

    // The one legal set of spans, shared by the settings validator and the
    // applet's context menu so they cannot drift apart.
    static constexpr double kSpanNarrowKHz = 10.0;   // ±5 kHz
    static constexpr double kSpanWideKHz   = 20.0;   // ±10 kHz

private:
    static QJsonObject readObj();
    static void write(const QJsonObject& o);
};

} // namespace AetherSDR
