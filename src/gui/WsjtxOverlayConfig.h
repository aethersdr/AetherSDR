#pragma once

// Principle V: single owned configuration object for the WSJT-X Rx/Tx
// overlay feature (#4526). Every new overlay setting lives under one root
// AppSettings key as a nested JSON value — one place to default, one place
// to migrate, one atomic write. The legacy flat keys (WsjtxAutoStart,
// WsjtxAddress, WsjtxPort, etc.) are grandfathered and not touched here.

#include "AppSettings.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace AetherSDR {

constexpr const char* kWsjtxOverlayKey = "WsjtxOverlay";

// Read/write the whole config object.
inline QJsonObject wsjtxOverlayRead()
{
    return QJsonDocument::fromJson(
        AppSettings::instance()
            .value(kWsjtxOverlayKey, "{}")
            .toString().toUtf8()).object();
}

inline void wsjtxOverlayWrite(const QJsonObject& obj)
{
    auto& s = AppSettings::instance();
    s.setValue(kWsjtxOverlayKey,
               QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    s.save();
}

// Per-pan "Show WSJT-X Rx/Tx Frequency" toggle.
// showFreq is a JSON array; index == pan index, missing entries default false.
inline bool wsjtxOverlayShowFreq(int panIndex)
{
    if (panIndex < 0) return false;
    return wsjtxOverlayRead().value("showFreq").toArray().at(panIndex).toBool(false);
}

inline void wsjtxOverlaySetShowFreq(int panIndex, bool on)
{
    if (panIndex < 0) return;
    auto obj = wsjtxOverlayRead();
    auto arr = obj.value("showFreq").toArray();
    while (arr.size() <= panIndex)
        arr.append(false);
    arr[panIndex] = on;
    obj.insert("showFreq", arr);
    wsjtxOverlayWrite(obj);
}

// Returns true if any pan has the overlay enabled.
inline bool wsjtxOverlayAnyPanEnabled()
{
    for (const auto& v : wsjtxOverlayRead().value("showFreq").toArray())
        if (v.toBool(false)) return true;
    return false;
}

// App-global "Post decoded stations as spots" toggle. Default true so
// existing installs keep today's behavior unless the box is unchecked.
inline bool wsjtxOverlayPostSpots()
{
    return wsjtxOverlayRead().value("postSpots").toBool(true);
}

inline void wsjtxOverlaySetPostSpots(bool on)
{
    auto obj = wsjtxOverlayRead();
    obj.insert("postSpots", on);
    wsjtxOverlayWrite(obj);
}

}  // namespace AetherSDR
