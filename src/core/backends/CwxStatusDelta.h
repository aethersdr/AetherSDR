#pragma once

#include <QMap>
#include <QMetaType>
#include <QString>

#include <optional>
#include <utility>

namespace AetherSDR {

// One CWX status ("cwx …"), present-only. Macros are decoded already
// (FlexLib CWX.cs substitutions) and keyed by their wire key ("macro1" …
// "macro12"), so they apply in the order the radio's keys sort.
struct CwxStatusDelta {
    std::optional<int> breakInDelay;
    std::optional<std::pair<int, int>> erase;   // start, stop
    QMap<QString, std::pair<int, QString>> macros;  // key → (index 0..11, text)
    std::optional<bool> qsk;
    bool queueDrained{false};                   // queue= empty or 0
    std::optional<int> sent;
    std::optional<int> wpm;
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::CwxStatusDelta)
