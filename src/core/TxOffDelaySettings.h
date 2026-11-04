#pragma once

// TX off delay persistence: one JSON object under AppSettings "TxOffDelay".
// Client-side by necessity: the radio's interlock stores key-on delays only,
// so there is no radio-persisted unkey hold to defer to.

#include <QJsonObject>

namespace AetherSDR {

class TxOffDelaySettings {
public:
    static constexpr int kMinMs = 0;
    static constexpr int kMaxMs = 10000;

    // Milliseconds to keep transmitting after a phone-mode PTT release.
    // A missing, malformed or out-of-range stored value reads as 0 (off).
    static int delayMs();
    static void setDelayMs(int ms);

    // Clamps into [kMinMs, kMaxMs]; shared by the store and TransmitModel.
    static int clampMs(int ms);

private:
    static QJsonObject readObj();
    static void write(const QJsonObject& o);
};

} // namespace AetherSDR
