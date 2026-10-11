---
title: "Manual Connection"
slug: "/manual-connection"
description: "Connect to a radio by its IP address when automatic discovery cannot reach it, such as across a VPN or a routed network."
---

Connect to a radio by its IP address when automatic discovery cannot reach it, such as across a VPN or a routed network.

Use **Connect by IP** for:

- A radio on a different VLAN or subnet
- A radio behind a VPN, including a FLEX-8000/Aurora on your tailnet (see [Tailscale Remote Access](./tailscale-remote-access.md))
- Remote access via a public IP with port forwarding (without SmartLink)
- A networked Icom, which is always reached this way (see [Networked Icom](./networked-icom.md))

## Setup

1. Open the **Connect to Radio** window: **File → Connect to Radio...**, or the **+** button beside the title bar's radio tabs → **Connect manually…**.
2. Choose the **Connect by IP** card ("Best for VPN or routed station access when you already know the radio IP").

The **Radio IP address** group starts with two rows:

| Row | What to enter |
|-----|---------------|
| **Radio type:** | The radio family to look for: **FlexRadio**, **Hermes-Lite 2**, **ANAN-G2**, **Icom (network)** or **RTL-SDR (USB)**. Only this family is probed. |
| **Radio IP:** | The IP address or host name, for example `10.0.0.25`. |

**RTL-SDR (USB)** appears only in builds that include the RTL-SDR backend. Choosing **Icom (network)** or **ANAN-G2** adds that family's own rows; see [Networked Icom](./networked-icom.md) and [ANAN-G2](./anan-g2.md). For every family's status, see [Supported Radios](./supported-radios.md).

## Using Connect by IP

### Connecting to a FlexRadio

1. Set **Radio type:** to **FlexRadio**.
2. Enter the radio's IP address in **Radio IP:**.
3. Click **Connect by IP**. AetherSDR probes the radio on TCP 4992.
4. If the probe fails, the panel shows the reason. **Network Diagnostics**, beside the button, helps you check the path.

If your VPN creates more than one active network adapter, open **Advanced: choose the VPN source path** and pick the **Source path:**. Most users can leave this on Auto.

### Port forwarding (FlexRadio remote access)

For FlexRadio remote access without SmartLink, forward these ports on your router:
- **TCP 4992** → radio's LAN IP (command channel)
- **UDP 4991** → radio's LAN IP (VITA-49 audio/spectrum)

Then enter your public IP in **Radio IP:**.

For a networked Icom behind NAT, see the **Icom ports:** row on [Networked Icom](./networked-icom.md).

### Slower links

**Use low bandwidth mode** and **Enable adaptive frame-rate throttle** are in the same window, under "Connection options for slower links". See [Low Bandwidth Connections](./low-bandwidth-connections.md).

### Auto-reconnect

Connect by IP remembers the radio type you chose and the addresses you have used. With **Connect to last radio on start up** checked, AetherSDR reconnects to the last radio on the next launch. If startup auto-connect gives up, the Connect to Radio window opens again.

## Reference

### Connect by IP compared with SmartLink

This comparison applies to FlexRadio. Other radio families do not use SmartLink.

| Feature | Connect by IP | SmartLink |
|---------|---------------|-----------|
| Authentication | None | FlexRadio account |
| Port forwarding | TCP 4992 + UDP 4991 | TCP 4994 + UDP 4993 |
| TLS encryption | No | Yes |
| NAT traversal | No (needs a static IP or port forward) | Built-in |
| FlexRadio account | Not required | Required (SmartSDR+) |

See [SmartLink Setup](./smartlink-setup.md).

## Troubleshooting

### The probe fails

AetherSDR could not reach the radio on TCP 4992. The panel shows the reason.

1. Check that **Radio type:** matches the radio; only that family is probed.
2. Click **Network Diagnostics** beside the button to check the path.
3. For access from outside your network, check that TCP 4992 and UDP 4991 are forwarded to the radio's LAN IP.

### The connection fails over a VPN with several network adapters

AetherSDR may be using the wrong adapter to reach the radio.

1. Open **Advanced: choose the VPN source path**.
2. Pick the VPN's adapter in **Source path:** and connect again.

### Audio works over a VPN, but the spectrum and waterfall stay empty

Spectrum and waterfall packets are larger than the tunnel allows, so they are dropped while the smaller audio packets get through.

1. Lower **Network MTU**; see [Low Bandwidth Connections](./low-bandwidth-connections.md).

## See also

- [Tailscale Remote Access](./tailscale-remote-access.md)
- [SmartLink Setup](./smartlink-setup.md)
- [Low Bandwidth Connections](./low-bandwidth-connections.md)
- [First Connection](./first-connection.md)
- [Supported Radios](./supported-radios.md)
