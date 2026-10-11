#pragma once

#include "TailnetAddress.h"

#include <QHostAddress>

#include <algorithm>

namespace AetherSDR {

// The largest network_mtu AetherSDR asks for when its connection to the radio
// runs over Tailscale. A tailnet carries 1280-byte packets (Tailscale's TUN
// MTU unless TS_DEBUG_MTU overrides it), so a VITA-49 datagram sized for the
// saved setting (1450 by default) can't cross it: the panadapter, waterfall
// and often the audio never arrive (#5949). 1200 leaves room for the IP and
// UDP headers. It matches the in-radio shim's -max-mtu default, but it can't
// follow a shim configured otherwise: the shim reports its clamp only in link
// telemetry, which arrives after the MTU has been sent. On the shim's path the
// shim clamps anyway; this value matters most on a subnet route.
inline constexpr int kTailnetNetworkMtu = 1200;

// The Network MTU setting's range and default, as Radio Setup offers them.
inline constexpr int kMinNetworkMtu = 576;
inline constexpr int kMaxNetworkMtu = 9000;
inline constexpr int kDefaultNetworkMtu = 1450;

// Whether the path to the radio runs over Tailscale: the radio is at a
// tailnet address (the in-radio shim), or this computer reaches it from its
// own tailnet address (a Tailscale subnet router in front of the radio's
// LAN, where the radio keeps its LAN address). 100.64.0.0/10 is also carrier
// NAT space, so a computer handed such an address on the radio's own segment
// is capped too: conservative, and the cap is logged.
inline bool reachedOverTailnet(const QHostAddress& radio, const QHostAddress& local)
{
    return isTailnetAddress(radio) || isTailnetAddress(local);
}

// The network_mtu to send: the operator's saved value, capped over a tailnet.
// A smaller saved value is kept. The setting itself is never changed, so the
// LAN gets the full value back on the next connect. A saved value outside the
// setting's range (an empty or damaged store reads as 0) is replaced by the
// default rather than sent to the radio.
inline int networkMtuFor(int saved, bool overTailnet)
{
    const int valid = (saved >= kMinNetworkMtu && saved <= kMaxNetworkMtu) ? saved
                                                                            : kDefaultNetworkMtu;
    return overTailnet ? std::min(valid, kTailnetNetworkMtu) : valid;
}

}  // namespace AetherSDR
