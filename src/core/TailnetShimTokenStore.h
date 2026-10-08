#pragma once

#include <QString>

#include <functional>

class QObject;

namespace AetherSDR {

// The admin token the in-radio tailnet shim issues on first provisioning.
// It is the only proof AetherSDR has that it may change that radio's tailnet
// key, so it lives in the OS keychain (service "AetherSDR", one entry per
// radio serial), never in the settings database. Builds without QtKeychain
// keep it for the session only and say so.
//
// The Tailscale auth key itself is never stored: it is single-use, and the
// shim spends it on the spot.
class TailnetShimTokenStore {
public:
    // Calls back on context's thread with the stored token, or an empty
    // string when none is stored or the keychain is unavailable.
    static void load(const QString& radioSerial, QObject* context,
                     std::function<void(const QString&)> callback);
    // An empty token deletes the entry.
    static void save(const QString& radioSerial, const QString& token);
    [[nodiscard]] static bool persistentStoreAvailable();
};

}  // namespace AetherSDR
