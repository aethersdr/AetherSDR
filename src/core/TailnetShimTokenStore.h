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
    // Which radio agent's token: the shim's, or the WSPR receiver's
    // (tools/wspr-agent), which issues one the same way on first use. Each
    // has its own keychain entry per radio serial.
    enum class Entry { TailnetShim, Wspr };

    // Calls back on context's thread with the stored token, or an empty
    // string when none is stored or the keychain is unavailable.
    static void load(const QString& radioSerial, QObject* context,
                     std::function<void(const QString&)> callback,
                     Entry entry = Entry::TailnetShim);
    // An empty token deletes the entry. `done` (optional) runs on context's
    // thread with whether the token reached persistent storage and, if not,
    // why: losing the only admin token without a word is worse than saying so.
    static void save(const QString& radioSerial, const QString& token,
                     QObject* context = nullptr,
                     std::function<void(bool persisted, const QString& error)> done = {},
                     Entry entry = Entry::TailnetShim);
    [[nodiscard]] static bool persistentStoreAvailable();
    // What a finished keychain read yields: the read, unless a save happened
    // while it ran, in which case that save's (newer) token, or none if the
    // save deleted it. Public for its test.
    [[nodiscard]] static QString pickLoadedToken(bool savedSinceReadStarted, const QString& read,
                                                 const QString& sessionNow);
};

}  // namespace AetherSDR
