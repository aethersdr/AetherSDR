#pragma once

#include "core/PeripheralRemovalGuard.h"

#include <QString>
#include <functional>
#include <optional>

class QObject;

namespace AetherSDR {

// Desktop Keychain adapter for the three direct 4O3A accessory connections.
// This stays in the UI shell: the engine owns the handshake, while the desktop
// owns OS credential access. Never persisted in AppSettings configuration;
// without QtKeychain the codes stay in its session vault.
class PeripheralAuthStore {
public:
    using Device = PeripheralRemovalGuard::Device;
    enum class LoadStatus { Found, Missing, Unavailable };
    struct LoadResult {
        QString code;
        LoadStatus status{LoadStatus::Missing};
    };

    // The identity a saved code is bound to. Empty until a socket is actually
    // connected (peerAddress must be a real address), so a discovery claim
    // never binds a code.
    //  - Host given as a literal IP: "ip:port", the connected peer address.
    //  - Host given as a name (typically DDNS): "host:name:port". The code
    //    follows the name, so a residential IP change does not ask the
    //    operator to retype it. The trade is deliberate: whatever the name
    //    resolves to receives the saved code, so a name the operator does not
    //    control is a name they should not save a code for.
    static QString endpoint(const QString& configuredHost, const QString& peerAddress,
                            quint16 port);
    // The same identity from what the operator typed, for the setup dialog's
    // status and Clear, which have no socket to ask. Read-only: save and load
    // still go through endpoint(), which needs a connected peer. Equal to
    // endpoint() for the same host and port once connected.
    static QString configuredEndpoint(const QString& configuredHost, quint16 port);
    static void load(Device device, const QString& endpoint, QObject* context,
                     std::function<void(const LoadResult&)> callback);
    // The callback reports persistence. A session-only save returns false for
    // a nonempty code; clearing the session value returns true.
    static void save(Device device, const QString& endpoint, const QString& code,
                     QObject* context,
                     std::function<void(bool)> callback = {});
    enum class ClearResult { Cleared, SessionCleared, Failed, UnknownOwner };
    // SessionCleared means the OS backend is unavailable: local state is
    // cleared, but deletion of a previously persisted secret is unconfirmed.
    static void clear(Device device, QObject* context,
                      std::function<void(ClearResult)> callback);
    // Shared AG/ShackSwitch slot: delete only a matching peer endpoint. Unknown
    // ownership fails closed; another endpoint's record is preserved.
    static void clearForEndpoint(Device device, const QString& endpoint, QObject* context,
                                 std::function<void(ClearResult)> callback);
    // Metadata only: never prompts the OS vault or exposes a saved secret.
    struct CodeAvailability {
        LoadStatus status;
        bool persistent;
    };
    static std::optional<CodeAvailability> cachedStatus(Device device, const QString& endpoint);
    static bool persistentStoreAvailable();
    static bool validCode(const QString& code);
};

} // namespace AetherSDR
