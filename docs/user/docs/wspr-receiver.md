---
title: "WSPR Receiver"
slug: "/wspr-receiver"
description: "A receive-only WSPR decoder that runs on a FlexRadio itself, with no PC, and feeds its spots to SpotHub, the panadapter, AetherMap and WSPRnet."
status: "Experimental"
applies_to: ["FlexRadio"]
platforms: ["Linux", "macOS", "Windows"]
---

:::info[Status]

**Status:** Experimental · **Applies to:** FlexRadio · **Platforms:** Linux, macOS, Windows

:::

The WSPR receiver is a small program that runs on the radio and decodes WSPR around the clock, whether or not a computer is connected. It listens on a slice of its own, decodes each two-minute cycle with the WSPR decoder from WSJT-X, and can upload its spots to [WSPRnet](https://www.wsprnet.org). AetherSDR installs it, configures it and shows what it hears.

It is receive-only: it never transmits, and its slice is never the transmit slice.

## Requirements

- A FLEX-8000 series or Aurora radio, where it runs as a container and so needs the radio's Waveform Processor ready. A FLEX-6000 can install it as a legacy waveform, but this hasn't been tested: its single processor core may not finish decoding a busy band within the two-minute cycle, and the receiver records only once the radio's clock is synchronized, which may not happen on a radio with no time source.
- AetherSDR connected to the radio over its **local network** or through [Remote Access](./tailscale-remote-access.md) version 0.5.2 or later. SmartLink can't reach the receiver.
- One free slice. The receiver takes one of the radio's slices for as long as it runs.

## Installing it

1. Open **Tools → Waveforms...**
2. Click **Install** and choose **WSPR Receiver**.

The receiver ships inside AetherSDR, so nothing is downloaded. It takes less than 500 KB on the radio. It starts within seconds, and listens on 20 m until you change that.

To remove it, click **Remove** (FLEX-8000 and Aurora) or **Uninstall** (FLEX-6000) on its **AetherWSPR** row. This also removes its settings.

## Configuring it

Click **Configure...** on the **AetherWSPR** row in the Waveforms window, or in **SpotHub → WSPR**. The receiver keeps these settings itself, in a file on the radio, so they apply with no computer connected and survive a reboot. Click **Apply** to send your changes to the radio.

The first computer to click **Apply** claims the receiver: AetherSDR keeps the receiver's admin token in your system keychain, and only that computer can change the settings afterward. On another computer, reinstall the receiver to start over.

### Station

**Callsign** and **Grid** name the reporting station. Leave them empty to use the radio's callsign and its GPS grid; the empty fields show what those are. A grid is needed for band hopping and for uploading.

### Receive

| Setting | What it does |
|---|---|
| **Band** | The band the receiver stays on when band hopping is off |
| **Antenna** | The receive antenna for its slice; **Radio default** leaves the radio's choice |
| **Decode depth** | How hard the decoder works, below |
| **DAX channel** | The DAX audio channel its slice uses. Pick one you don't use for anything else |

| Decode depth | What it does |
|---|---|
| **Standard** | The decoder's own settings. It decoded the most weak signals in testing, and is the default |
| **Fast** | WSJT-X's Fast: two passes, the quickest |
| **Normal** | WSJT-X's Normal: three passes and the ordered-statistics decoder |
| **Deep** | WSJT-X's Deep: as Normal, with more candidate signals |

### Band hopping

Tick **Hop bands** to follow WSJT-X's coordinated band hopping. Every two minutes the worldwide schedule names one of ten bands, 160 m to 10 m. The grid sets which bands the receiver may use at each time of day: **Sunrise gray line**, **Day**, **Sunset gray line** and **Night**, worked out from your grid.

- If the scheduled band is ticked for this time of day, the receiver goes there.
- If it isn't, the receiver goes to another band ticked for this time of day, at random.
- If nothing is ticked for this time of day, it stays where it is.

**Gray time** sets how long each gray line period lasts, 1 to 120 minutes (60 by default). Nothing is ticked until you tick it.

### Upload

**Upload spots to WSPRnet** sends each cycle's spots to wsprnet.org under your callsign and grid, the way WSJT-X does. This makes them public. The receiver uploads on its own, and holds spots while wsprnet can't be reached, for up to 24 hours. The line below the checkbox shows how many it has sent.

## Seeing what it hears

Open **Settings → SpotHub...** and choose the **WSPR** tab, then click **Start**, or turn on **Auto-Start** to start reading whenever you connect to a radio. See [SpotHub](./spothub.md#wspr).

- The tab's console lists every decode, with its distance from your grid.
- Each station appears on the panadapter as a spot, when its frequency is in view.
- [AetherMap](./psk-reporter-map.md) shows the stations you heard, along with PSK Reporter's reports. Choose your own callsign in **Call:**, or tick **All Callsigns**.

## Good to know

- The radio removes the receiver's slice when its last GUI client disconnects, so closing SmartSDR or AetherSDR can lose the cycle in progress. The receiver makes a new slice at once and carries on with the next cycle.
- It waits for the radio's clock to be set before it starts recording, up to about a minute and a half after the radio boots.
- Removing or updating the receiver starts it over with default settings, and any computer can claim it again.
- Decoding takes one processor core for about 10 to 25 seconds in each two-minute cycle, at low priority, so SmartSDR always comes first.

## Related pages

- [SpotHub](./spothub.md)
- [AetherMap](./psk-reporter-map.md)
- [Radio Health Tools](./radio-health-tools.md)
- [Tailscale Remote Access](./tailscale-remote-access.md)
