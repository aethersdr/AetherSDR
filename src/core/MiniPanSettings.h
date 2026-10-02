#pragma once

// Mini-pan persistence: one JSON object under AppSettings "MiniPan". Only the
// +/-5 / +/-10 kHz span is feature-owned; geometry, open state and always-on-top
// belong to the container framework (ContainerManager /
// FloatingContainerWindow). Nothing radio-owned is persisted: centre, bandwidth
// and dBm range are re-derived from the followed slice on every connect.

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
