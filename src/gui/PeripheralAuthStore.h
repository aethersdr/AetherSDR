#pragma once

#include <QString>
#include <functional>

class QObject;

namespace AetherSDR {

// Desktop Keychain adapter for the three direct 4O3A accessory connections.
// This stays in the UI shell: the engine owns the handshake, while the desktop
// owns OS credential access. Never persisted in AppSettings configuration;
// without QtKeychain the codes stay in its session vault.
class PeripheralAuthStore {
public:
    enum class Device { Tgxl, Pgxl, AntennaGenius };
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
    static void load(Device device, const QString& endpoint, QObject* context,
                     std::function<void(const LoadResult&)> callback);
    // The callback reports persistence. A session-only save returns false for
    // a nonempty code; clearing the session value returns true.
    static void save(Device device, const QString& endpoint, const QString& code,
                     QObject* context,
                     std::function<void(bool)> callback = {});
    static bool persistentStoreAvailable();
    static bool validCode(const QString& code);
};

} // namespace AetherSDR
