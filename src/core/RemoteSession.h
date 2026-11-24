#pragma once

#include "core/RadioDiscovery.h"
#include "core/TailnetAddress.h"

namespace AetherSDR {

// Remote for the network-quality thresholds, the NTP gate, the discovery
// socket and the low-bandwidth default: SmartLink (WAN), a manual-IP/routed
// connection, or a radio at a tailnet address (RFC #6271 ruling D4).
// 100.64.0.0/10 is also carrier-grade NAT space, so a LAN numbered there
// counts as remote here; that is accepted for these defaults only. Radio
// Setup's IP Configuration uses isRemoteForIpConfiguration() instead.
inline bool isRemoteRadioSession(bool wan, const RadioInfo& info)
{
    return wan || info.isRouted || isTailnetAddress(info.address);
}

// Whether Radio Setup's IP Configuration is dimmed: SmartLink, a routed
// connection, or a confirmed Tailscale session (the connection runs through
// Tailscale's interface, core/TailscaleInterface.h). No address match: a LAN
// numbered in 100.64.0.0/10 keeps IP Configuration, and a radio reached over
// a WireGuard or OpenVPN relay counts as local.
inline bool isRemoteForIpConfiguration(bool wan, bool routed, bool overTailscale)
{
    return wan || routed || overTailscale;
}

}  // namespace AetherSDR
