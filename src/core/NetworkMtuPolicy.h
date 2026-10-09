#pragma once

#include "TailnetAddress.h"

#include <QHostAddress>

#include <algorithm>

namespace AetherSDR {

// The largest network_mtu AetherSDR asks for when its connection to the radio
// runs over Tailscale. A tailnet carries 1280-byte packets, so a VITA-49
// datagram sized for the saved setting (1450 by default) can't cross it: the
// panadapter, waterfall and often the audio never arrive (#5949). 1200 is the
// in-radio shim's own clamp (flex-tailnet-shim -max-mtu), leaving room for
// the IP and UDP headers.
inline constexpr int kTailnetNetworkMtu = 1200;

// Whether the path to the radio runs over Tailscale: the radio is at a
// tailnet address (the in-radio shim), or this computer reaches it from its
// own tailnet address (a Tailscale subnet router in front of the radio's
// LAN, where the radio keeps its LAN address).
inline bool reachedOverTailnet(const QHostAddress& radio, const QHostAddress& local)
{
    return isTailnetAddress(radio) || isTailnetAddress(local);
}

// The network_mtu to send: the operator's saved value, capped over a tailnet.
// A smaller saved value is kept. The setting itself is never changed, so the
// LAN gets the full value back on the next connect.
inline int networkMtuFor(int saved, bool overTailnet)
{
    return overTailnet ? std::min(saved, kTailnetNetworkMtu) : saved;
}

}  // namespace AetherSDR
