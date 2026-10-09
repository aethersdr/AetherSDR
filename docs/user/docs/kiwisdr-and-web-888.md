---
title: "KiwiSDR and Web-888"
slug: "/kiwisdr-and-web-888"
description: "AetherSDR can use a remote KiwiSDR or Web-888 web receiver as a receive-only \"virtual RX antenna\" for a slice."
---

AetherSDR can use a remote **KiwiSDR** or **Web-888** web receiver as a receive-only "virtual RX antenna" for a slice. While a slice uses one, its audio, S-meter and (optionally) its spectrum and waterfall come from that receiver instead of your radio. Public KiwiSDRs around the world can be browsed from inside AetherSDR.

These receivers never transmit, and AetherSDR inhibits transmit on a panadapter that is showing one.

## Where to go next

- [Diversity and ESC](./diversity-and-esc.md): pair a KiwiSDR with your radio for diversity receive.
- [SpotHub](./spothub.md): the KiwiSDR DX Community spot overlay.
- [Memory Channels](./memory-channels.md): KiwiSDR receivers use the bank stored on this computer.
- [Support and Logging](./support-and-logging.md): KiwiSDR log categories for problem reports.

## Setup

### Adding a receiver

Open **Tools → Configure KiwiSDR...**. It opens **Settings → Radio Setup... → Antennas**, where the **KiwiSDR RX Antennas** group lists your receivers.

Each receiver has:

<img src="/img/screens/radio-setup-kiwi.png" width="649" alt="KiwiSDR RX Antennas group. A note says passwords are kept only for the current session. Configured Receivers reads No KiwiSDR receivers configured. Under Add Receiver, the Name field holds Example Kiwi, the Server field kiwi.example.org:8073, the Password field is empty and Type is KiwiSDR, with an Auto-connect checkbox and Browse public… and Add receiver buttons. Import… and Export… buttons sit at the bottom right." />

*The KiwiSDR RX Antennas group on the Antennas page, with a receiver typed in but not yet added.*

| Field | Use |
|-------|-----|
| **Name** | Required display name. It is how the receiver appears in antenna menus. |
| **Server** | `hostname` or `hostname:port` (the default port is 8073). |
| **Password** | Optional. Stored in the operating system's credential store. The status beneath it reads "Stored securely", "Current session only" (builds without a credential store) or "No password stored". To change it, type over the masked value; you don't need the old one. |
| **Type** | **KiwiSDR** or **Web-888**. Entries saved before Web-888 support default to KiwiSDR. |
| **Auto-connect** | Connect to this receiver automatically. |
| **Keep audio during TX** | Off (default): the receiver's audio is silenced while you transmit and resumes immediately at unkey. The stream stays warm, so there is no gap or re-sync. On: keep listening while you transmit. |
| **Resume audio after TX delay** | After unkey, wait out this receiver's stream delay before unmuting, so you don't hear your own delayed transmission. No effect while **Keep audio during TX** is on. |

Each row also has **Connect** / **Disconnect** and a **Remove** (bin) button. To add a receiver by hand, fill in the empty row at the bottom (name, `host:8073`, optional password and type) and click **Add receiver**.

### Import and export

**Import...** and **Export...** move the receiver list to and from a CSV file. Passwords are never exported; they stay in the credential store. Import merges on a matching server, so importing the same file twice changes nothing and keeps existing passwords. An optional `RECEIVER_TYPE` column sets KiwiSDR or Web-888.

### Browsing public KiwiSDRs

Click **Browse public…** to open **Browse public KiwiSDR receivers**:

- A filter box ("Filter by name, location, or host…") and **Refresh list**.
- Columns **Receiver**, **Location**, **Users**, **API** and **Limits**.
- Select one and click **Add selected**.

The directory comes from AetherSDR's own mirror at `cdn.aethersdr.com`, which the KiwiSDR maintainer asked for. It is refreshed hourly; AetherSDR never contacts kiwisdr.com for the list. Opening the browser reuses the list for 30 minutes; **Refresh list** always fetches. If the mirror is unreachable, the browser shows the list it already has and points to `kiwi-status.aethersdr.com`. Once a list is more than 6 hours old, its age is added to the status line, but it is still shown in full.

Out of respect for each operator's choice, receivers whose operator disabled API access ("web-only"), receivers that publish no policy, and receivers the directory flags are **not offered**. The status line counts how many are hidden for each reason.

Web-888 receivers are not in the public directory; add them by host name.

## Using a Receiver on a Slice

Choose the receiver by name in the slice's RX antenna list: the RX antenna selector in the RX applet, or the antenna menu in the panadapter overlay. Choose a radio antenna again to return to your radio.

- Tuning the slice tunes the receiver. The receiver stays assigned across band changes, and the slice's previous mute state comes back when you stop using it.
- In CW the carrier is audible, offset by your CW pitch.
- The connection is released when no slice is using it.

The **KiwiSDR** (KSDR) applet lists each configured receiver with its type (a **Web-888** badge where it applies), its state and the slice it is assigned to. States include Disconnected, Connecting, Connected, Busy, Waiting, Monitoring, Camp ended and Error.

### Busy receivers

Public receivers have a limited number of channels. When every channel is busy, AetherSDR waits for a free slot ("Waiting for a free KiwiSDR receiver slot"), can offer a monitor (camp) session on another user's channel, and says clearly when the receiver refuses you.

### Flex / Kiwi display toggle

A small **Flex** / **Kiwi** button at the bottom-left of a panadapter with a KiwiSDR slice chooses where the spectrum and waterfall come from. It changes only the display; audio and meters are unchanged. While the Kiwi display is shown, transmit is inhibited on that panadapter: "Transmit is disabled because this panadapter is displaying a KiwiSDR receiver."

The 3D stacked-trace spectrum works on KiwiSDR sources too.

## Receive Sync and Diversity

A remote receiver's audio and waterfall arrive later than your own radio's. **Settings → Receive Sync ▸** time-aligns them:

| Item | Use |
|------|-----|
| **Sync Kiwi Audio & Display** | Turns alignment on. |
| **Manual Offset** / **Auto Assist** | Set the offset yourself, or let AetherSDR measure it by cross-correlating the two receivers' audio and spectrum. |
| **Delay KiwiSDR 50 ms** / **Delay Flex 50 ms** / **Reset Offset** | Nudge the manual offset. |
| **Latency ▸** | **Normal (360 ms)**, **More Stable (520 ms)** or **High Jitter (1000 ms)**. |

The menu also shows the current offset and a status line.

A KiwiSDR can take one side of a diversity pair, so you hear your radio and the remote receiver together. See [Diversity and ESC](./diversity-and-esc.md).

## KiwiSDR DX Community Spots

The **Kiwi DX** toggle in **SpotHub → Display** overlays the KiwiSDR DX Community database on the panadapter. It is off by default. See [SpotHub](./spothub.md).

## Known issues

- Buzzing, crackling or popping can be heard while a KiwiSDR is a slice's RX antenna ([#4993](https://github.com/aethersdr/AetherSDR/issues/4993)).
- On receivers that share their waterfall across all channels, AetherSDR can show "KiwiSDR waterfall unavailable" although the receiver sends one ([#5656](https://github.com/aethersdr/AetherSDR/issues/5656)).

## Troubleshooting

### The receiver shows Busy or Waiting

Every channel on the receiver is in use.

1. Leave the slice on the receiver: AetherSDR waits for a free slot and connects when one opens.
2. Accept a monitor (camp) session if one is offered.
3. Or choose another receiver with **Browse public…**.

### The receiver shows Error

The receiver refused the connection. Error messages name their source: the receiver's own refusal codes and messages.

1. Read the message in the **KiwiSDR** applet.
2. Before reporting a problem, turn on the KiwiSDR logging categories in **Help → Support & Diagnostics...**; see [Support and Logging](./support-and-logging.md).

### The public list is stale or will not refresh

AetherSDR's directory mirror is unreachable, so the browser shows the list it already has.

1. Check `kiwi-status.aethersdr.com`.
2. Click **Refresh list** again later.

### Transmit is disabled on a panadapter

The panadapter is showing the KiwiSDR's spectrum and waterfall.

1. Click the **Flex** / **Kiwi** button at the bottom-left of the panadapter to switch the display back to your radio.

### You hear your own transmission after unkeying

The receiver's audio is delayed, so it still carries your signal for a moment after unkey.

1. Open **Tools → Configure KiwiSDR...**.
2. Tick **Resume audio after TX delay** for that receiver, and leave **Keep audio during TX** off.

### The KiwiSDR's audio is out of step with your radio

A remote receiver's audio and waterfall arrive later than your own radio's.

1. Open **Settings → Receive Sync ▸** and tick **Sync Kiwi Audio & Display**.
2. Choose **Auto Assist**, or set **Manual Offset** and nudge it with **Delay KiwiSDR 50 ms** / **Delay Flex 50 ms**.
3. On an unsteady link, choose a longer **Latency ▸** setting.

## See also

- [Diversity and ESC](./diversity-and-esc.md)
- [SpotHub](./spothub.md)
- [Supported Radios](./supported-radios.md)
- [`docs/kiwisdr-public-directory.md`](https://github.com/aethersdr/AetherSDR/blob/main/docs/kiwisdr-public-directory.md)
- [`docs/kiwisdr-cleanroom-design.md`](https://github.com/aethersdr/AetherSDR/blob/main/docs/kiwisdr-cleanroom-design.md)
- [`docs/web888-cleanroom-design.md`](https://github.com/aethersdr/AetherSDR/blob/main/docs/web888-cleanroom-design.md)
