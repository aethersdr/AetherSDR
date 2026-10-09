---
title: "Low Bandwidth Connections"
slug: "/low-bandwidth-connections"
description: "Reduce network traffic and smooth out problems when connecting over metered, congested or bandwidth-limited links such as LTE, satellite, hotel Wi-Fi, SmartLink, or VPN tunnels (ZeroTier, WireGuard, Tailscale)."
status: "Supported"
applies_to: ["FlexRadio"]
---

:::info[Status]

**Status:** Supported · **Applies to:** FlexRadio

:::

Reduce network traffic and smooth out problems when connecting over metered, congested or bandwidth-limited links such as LTE, satellite, hotel Wi-Fi, SmartLink, or VPN tunnels (ZeroTier, WireGuard, Tailscale).

A normal connection to a FlexRadio carries several UDP streams:

| Stream | Format | Approx. Bandwidth |
|--------|--------|--------------------|
| RX Audio | uncompressed stereo, 24 kHz | ~1.5 Mbit/s |
| FFT Spectrum | uint16 bins | ~0.5–1.0 Mbit/s |
| Waterfall Tiles | uint16 bins | ~0.3–0.5 Mbit/s |
| Meters | uint16 pairs | negligible |

On a home LAN this is fine. Over a cellular or VPN link the combined 2–3 Mbit/s can cause dropouts, latency and data charges. AetherSDR has several independent controls. Use whichever your link needs.

## Using the Low-Bandwidth Options

### Audio compression (Opus)

**Location:** **Settings → Radio Setup... → Audio → Audio Compression (SmartLink / tailnet)**

| Setting | Behaviour |
|---------|-----------|
| **Uncompressed** (default) | Uncompressed on every connection |
| **Auto** | Opus on SmartLink and on a radio reached over a tailnet, uncompressed on the local network |
| **Opus** | Opus on every connection |

Until you choose, a radio reached over a tailnet gets Opus.

Opus reduces the audio stream from about 1.5 Mbit/s to about 64–128 kbit/s. It applies to both receive audio and PC-microphone transmit audio. On a LAN connection the radio may ignore the request and send uncompressed audio anyway.

**When to use:** choose **Auto** if you use SmartLink, or **Opus** on any metered or limited link.

### Low bandwidth mode

**Location:** connect panel → *Connection options for slower links* → **Use low bandwidth mode**

The *Connection options for slower links* box appears when you choose **Remote with SmartLink** or **Connect by IP**. With the box ticked, AetherSDR asks the radio for a low-bandwidth session (`client low_bw_connect`) as it connects, and the radio reduces the FFT, waterfall and meter data it sends.

- It takes effect when you connect. To change it, disconnect and reconnect.
- The choice is remembered for SmartLink and Connect-by-IP connections. Connecting to a FlexRadio found **On This Network** always uses full bandwidth.
- On a [Hermes-Lite 2](./hermes-lite-2.md), the same option caps the panadapter span at 96 kHz (about 6 Mbit/s).

AetherSDR exposes this as a manual choice because overlay networks such as ZeroTier, WireGuard and Tailscale can make a radio look local even when the real path is cellular or satellite.

### Adaptive frame-rate throttle

**Location:** connect panel → **Enable adaptive frame-rate throttle** (off by default)

When enabled, AetherSDR automatically lowers the FFT and waterfall frame rate when network quality degrades, reducing the chance of a disconnect on a congested link. It applies to every connection type.

- Toggling it while connected takes effect at the next quality update. To lift a cap that has already been applied, reconnect.
- The **Stream Rates** page of **Tools → Network Diagnostics...** charts the frame-rate cap — see [Runtime Monitor](./runtime-monitor.md).

### Network MTU

**Location:** **Settings → Radio Setup... → Network → Advanced → Network MTU**

Network MTU sets the largest VITA-49 UDP packet the radio sends. AetherSDR's default is **1450 bytes** (the radio default is 1500), which suits most VPN and SD-WAN tunnels. The range is 576–9000.

VITA-49 FFT and waterfall packets are about 1436 bytes at an MTU of 1500. Tunnels add overhead that reduces the effective MTU (see [Reference](#reference)). A packet larger than the tunnel MTU is fragmented (adding latency) or silently dropped (losing spectrum and waterfall). Audio packets are smaller and usually get through — which is why you can hear audio but see no spectrum over a VPN.

AetherSDR sends `client set enforce_network_mtu=1 network_mtu=<value>` to the radio on each connect. It applies to this client's session only and does not permanently change the radio or affect other clients.

### VITA-49 RX buffer

**Location:** **Settings → Radio Setup... → Network → Advanced → VITA-49 RX buffer**

The operating-system receive buffer for the radio's data stream. The slider snaps to 256 KB, 512 KB, 1 MB, 2 MB or **4 MB (default)**. A larger buffer absorbs bursts of panadapter and waterfall data so they are not dropped — dropped bursts show up as dips in the network statistics and can trigger the adaptive throttle.

The **granted** label shows what the operating system actually allowed. On Linux the size is capped by `net.core.rmem_max`.

### Audio smoothing

Two audio settings in **Settings → Radio Setup... → Audio** help on lossy links even when bandwidth is not the problem:

- **Smooth packet loss** (on by default) fades over lost audio packets instead of producing a click.
- **Audio Buffer** (default 100 ms, range 50–1000 ms) — increase it if audio stutters because of jitter.

See [Audio Settings](./audio-settings.md).

### Reducing panadapter traffic

You can reduce traffic further from the panadapter's **Display** panel (right-click the spectrum):

- **Lower FFT FPS** — PANADAPTER section, **FFT FPS** (5–60, default 25). Try 5–10 on a slow link.
- **Slow the waterfall** — WATERFALL section, **WtrFall Rate**.
- **Zoom in** — a narrower span sends fewer FFT bins.
- **Shrink the waterfall** — if you only need the spectrum trace, a minimal waterfall height reduces tile traffic.

### Diagnosing a link

**Tools → Network Diagnostics...** shows latency, jitter, packet loss by stream, stream rates and audio-buffer health over time, with symptom search. See [Runtime Monitor](./runtime-monitor.md).

## Reference

### Recommended settings by connection type

| Connection | Audio Compression | Low bandwidth mode | Adaptive throttle |
|------------|-------------------|--------------------|-------------------|
| Home LAN (Ethernet/Wi-Fi) | Uncompressed | — | Off |
| SmartLink over broadband | Auto | Optional | Optional |
| SmartLink over LTE / hotel Wi-Fi | Auto | On | On |
| VPN over broadband | Opus | Optional | Optional |
| VPN over LTE/cellular | Opus | On | On |
| VPN over satellite | Opus | On | On |

### Typical tunnel MTUs

| Tunnel Type | Typical Effective MTU |
|-------------|----------------------|
| WireGuard | 1420 bytes |
| OpenVPN (UDP) | 1400 bytes |
| Cisco SD-WAN | 1400–1450 bytes |
| ZeroTier | ~1400 bytes |
| Tailscale | 1280–1420 bytes |

### Recommended MTU settings

| Connection | MTU Setting |
|------------|-------------|
| Home LAN | 1500 (or leave at 1450) |
| VPN / SD-WAN | 1450 (default) |
| Satellite / high-overhead tunnels | 1300–1400 |

## Known issues

- On a remote link, receive audio can drop out briefly several times a minute when packets arrive out of order, although none are lost ([#6051](https://github.com/aethersdr/AetherSDR/issues/6051)).
- The network-quality indicator in the status bar is averaged, so it can stay green while the link is bad; use **Tools → Network Diagnostics...** for live conditions ([#2825](https://github.com/aethersdr/AetherSDR/issues/2825)).

## Troubleshooting

### Audio works, but the spectrum and waterfall stay empty over a VPN

Spectrum and waterfall packets are larger than the tunnel's MTU, so they are dropped while the smaller audio packets get through.

1. Open **Settings → Radio Setup... → Network → Advanced**.
2. Lower **Network MTU** to suit your tunnel (see [Recommended MTU settings](#recommended-mtu-settings)).
3. Reconnect; AetherSDR sends the MTU to the radio on each connect.

### The VITA-49 RX buffer says it was capped

On Linux, the operating system limits the buffer to `net.core.rmem_max`.

1. Raise `net.core.rmem_max` to use a larger buffer.
2. Check the **granted** label again.

### Audio stutters or clicks on a remote link

Network jitter or lost packets are reaching the audio buffer.

1. Raise **Audio Buffer** in **Settings → Radio Setup... → Audio** a little at a time.
2. Leave **Smooth packet loss** on.
3. Use Opus compression and low bandwidth mode.

### Audio drops out or the waterfall has gaps

Packets from the radio are lost, or arrive out of order, on the way to the computer. Wi-Fi, a busy switch or a VPN are the usual causes. The log then shows `PanadapterStream: VITA-49 sequence errors on <stream> stream`, at most once every 10 seconds for each stream.

1. Open **Tools → Network Diagnostics...** to see which streams lose packets.
2. Connect the computer to the network by cable instead of Wi-Fi.
3. If the VITA-49 RX buffer says it was capped, raise `net.core.rmem_max` (see [The VITA-49 RX buffer says it was capped](#the-vita-49-rx-buffer-says-it-was-capped)).
4. On a remote link, use Opus compression and low bandwidth mode.

### Low bandwidth mode has no effect

It takes effect only when you connect, and never on a FlexRadio found **On This Network**.

1. Disconnect.
2. Connect again with **Remote with SmartLink** or **Connect by IP**, with **Use low bandwidth mode** ticked.

## See also

- [SmartLink Setup](./smartlink-setup.md)
- [Tailscale Remote Access](./tailscale-remote-access.md)
- [Manual Connection](./manual-connection.md)
- [Audio Settings](./audio-settings.md)
- [Runtime Monitor](./runtime-monitor.md)
