#pragma once

#include <QByteArray>
#include <QJsonObject>

#include <functional>
#include <optional>

namespace AetherSDR::control {

// Client-side verification of the connected native handle, never the requested
// endpoint name. This proves the OS-account boundary, not a particular binary.
[[nodiscard]] bool localServerIsCurrentUser(qintptr descriptor);

using LocalPeerCheck = std::function<bool()>;
using LocalHelloExchange = std::function<std::optional<QJsonObject>(const QJsonObject&)>;

// The production credential-bearing write gate. Trusted composition supplies
// the native peer check and exchange for the SAME still-connected transport.
// A failed/unavailable check never constructs or sends a credential frame.
[[nodiscard]] std::optional<QJsonObject> exchangeCredentialHello(
    const QByteArray& secret, const LocalPeerCheck& verifyPeer, const LocalHelloExchange& exchange);

} // namespace AetherSDR::control
