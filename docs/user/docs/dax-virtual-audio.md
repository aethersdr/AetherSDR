---
title: "DAX Virtual Audio"
slug: "/dax-virtual-audio"
description: "DAX (Digital Audio eXchange) creates virtual audio devices on your computer that digital-mode programs (WSJT-X, fldigi, JS8Call, VARA and others) use as their sound card."
status: "Supported"
applies_to: ["FlexRadio", "RTL-SDR (experimental, receive-only)"]
platforms: ["Linux", "macOS", "Windows"]
---

:::info[Status]

**Status:** Supported · **Applies to:** FlexRadio, RTL-SDR (experimental, receive-only) · **Platforms:** Linux, macOS, Windows (DAX TX stream ownership only)

:::

DAX (Digital Audio eXchange) creates virtual audio devices on your computer that digital-mode programs (WSJT-X, fldigi, JS8Call, VARA and others) use as their sound card. Receive audio flows from a slice to the program, and the program's transmit audio flows back to the radio, all through AetherSDR with no cables.

FlexRadio provides receive and transmit DAX. AetherSDR hosts virtual audio devices on Linux and macOS; its DAX TX stream ownership controls are also available on Windows. RTL-SDR provides experimental **receive-only** DAX on Linux and macOS; see [RTL-SDR receive audio](#rtl-sdr-receive-audio). On radios without an audio-export route, use the [TCI Server](./tci-server.md).

## Requirements

| Platform | DAX |
|---|---|
| **Linux** | Built in. Needs PipeWire. Receive channels are native PipeWire streams; they also show up through PipeWire's PulseAudio and JACK layers. |
| **macOS** | Built in, through the **DAX Virtual Audio Driver** that ships in the AetherSDR DMG; install it from there. If it is missing, enabling DAX shows "DAX Audio Driver Missing". |
| **Windows** | No built-in DAX driver. The DAX applet says "No built-in DAX driver on Windows. Use TCI, or SmartSDR DAX." The **Take DAX TX** and **Release DAX TX** controls manage AetherSDR's stream independently of audio drivers. Use the [TCI Server](./tci-server.md) (audio and CAT over one connection), or FlexRadio's own SmartSDR DAX, for audio. |

## Setup

### Turning DAX on

- The **DAX Audio** applet (applet-tray button **DAX**) has a **DAX: Enabled / Disabled** button at the bottom.
- **Settings → Autostart DAX with AetherSDR** is the same setting: while it is on, DAX starts every time you connect. This menu entry is not shown on Windows.

### Assigning a slice to a DAX channel

On FlexRadio, the radio sends a slice's audio to the DAX channel set on that slice. Choose it on the slice's **VFO widget → DAX tab** (**DAX Ch**). The channel list follows the radio's capacity, and a slice on a channel the radio cannot back shows **Off**.

The panadapter overlay's **DAX** flyout is for IQ instead: it holds **IQ Ch** and the **WFM** button (see [DAX IQ Streaming](./dax-iq-streaming.md)).

### Checking the devices (Linux)

```bash
pactl list sources short | grep aethersdr   # DAX 1..N capture devices
pactl list sinks short   | grep aethersdr   # TX device
```

Stale devices left by a crash are cleaned up the next time DAX starts.

## Using DAX

### RTL-SDR receive audio

Turn on **DAX Audio** and select the matching **AetherSDR DAX n** input in your program. Native receive export requires a build with WebSockets support. Input 1 follows TCI receiver 0, input 2 follows receiver 1, and so on; the applet displays the assigned slice. These assignments do not use the FlexRadio **DAX Ch** selector, and neither a running TCI listener nor a connected TCI client is required.

Speaker gain and mute do not alter exported audio. Receiver squelch still applies, and the DAX row has its own gain. Linux exports mono at 48 kHz; macOS exports stereo at 24 kHz through the installed DAX driver. Use stereo TCI when stereo WFM is required on Linux. RTL creates no TX endpoint and the TX control is unavailable.

The normal RTL limit remains one receiver. Additional inputs are for the process-only [multi-receiver evaluation](./rtl-sdr.md#multi-receiver-evaluation). An older four-input macOS driver must be updated separately before evaluating all eight inputs; building AetherSDR does not update an installed driver.

### Transmit audio

On FlexRadio, a digital program's audio from **AetherSDR TX** uses this client's DAX TX stream. Before transmitting, select your local TX slice, leave the radio idle, and click **Take DAX TX** in **DAX Audio**. Wait for **DAX TX: Owned** without **Audio blocked**. Put the TX slice in a digital mode (DIGU or DIGL) and initiate transmission separately in your program. Voice modes use your microphone.

With MultiFlex, another client can select its own DAX TX stream. AetherSDR's label follows the radio's report, and loss of ownership blocks its DAX transmit audio. Click **Release DAX TX** while idle to yield AetherSDR's stream. These actions do not change the TX slice or global DAX input, key the radio, start audio, or restore a previous transmission. A later transmission needs a new operator action.

Ownership is not taken on connect, mode change, incoming status or settings restore. Reconnecting does not replay a previous **Take DAX TX** request. If a request is refused or times out, the applet shows its reason; check the current status before making another request.

On Linux and macOS, starting the hosted DAX bridge temporarily selects **PC**
as the mic source. Stopping it restores the original source only in the same
radio session and only if you have not selected a mic source since it started.
This temporary selection is separate from DAX stream ownership and never keys
the transmitter.

### Wiring WSJT-X (Linux and macOS)

This transmit-capable setup applies to FlexRadio.

1. Put the slice in **DIGU** and set its DAX channel to **1**.
2. Turn DAX on, click **Take DAX TX** while the radio is idle, and wait for **DAX TX: Owned**. Turn on a **Rigctld** CAT port (see [CAT Control](./cat-control.md)).
3. In WSJT-X, **File → Settings → Audio**: Input **AetherSDR DAX 1**, Output **AetherSDR TX**.
4. **Settings → Radio**: Rig `Hamlib NET rigctl`, Network Server `127.0.0.1:4532`, PTT Method `CAT`.

Full walk-through: [WSJT-X Integration](./wsjt-x-integration.md).

### Other programs

| Program | Modes | Audio in | Audio out | Rig control |
|---|---|---|---|---|
| **WSJT-X / JTDX** | FT8, FT4, JT65, Q65, WSPR | AetherSDR DAX n | AetherSDR TX | Rigctld CAT port, or TCI |
| **fldigi** | PSK, RTTY, MFSK, CW, … | AetherSDR DAX n | AetherSDR TX | Rigctld CAT port |
| **JS8Call** | JS8 | AetherSDR DAX n | AetherSDR TX | Rigctld CAT port |
| **VARA HF/FM** | VARA | AetherSDR DAX n | AetherSDR TX | Rigctld CAT port |
| **Dire Wolf** | APRS, packet | AetherSDR DAX n | AetherSDR TX | — |

For packet and APRS you may not need an external modem at all: see [AetherModem Packet Radio](./aethermodem-packet-radio.md).

### Two programs on two slices

Give each program its own DAX channel and its own CAT port (with a different **VFO A** slice).

## Reference

### Devices

For FlexRadio, AetherSDR offers **up to eight RX channels plus one TX channel**. The number of RX channels follows the connected radio's slice capacity (for example 8 on a FLEX-6700, 4 on a FLEX-6600 or 8600, 2 on a FLEX-6300 or 6400). Rows and devices above that number are not shown.

| Device | Direction | Purpose |
|---|---|---|
| **AetherSDR DAX 1** … **DAX 8** | Capture (input) | Receive audio from whichever slice is assigned that DAX channel |
| **AetherSDR TX** | Playback (output) | Transmit audio to the radio |

### The DAX Audio applet

Each RX row shows **DAX n:**, the slice currently assigned to that channel (or "—"), and a combined **level meter and gain slider**. The **TX:** row shows which slice holds transmit and the gain applied to audio coming from your program. Gains are saved per channel.

The ownership section is present on every platform, including Windows:

| Control or status | Meaning |
|---|---|
| **Take DAX TX** | Request selection of this client's stream. It can create AetherSDR's stream when needed. |
| **Release DAX TX** | Yield this client's confirmed stream. |
| **DAX TX: Owned / Released / Unknown** | The latest radio-confirmed ownership observation. **No stream** and **Unavailable** identify sessions without a usable stream. |
| **Creating / Taking / Releasing** | A request is pending; the radio has not yet confirmed completion. |
| **Refused / Timed out** | The request failed; the displayed reason explains why. The last radio observation is retained. |
| **Audio blocked** | Ownership alone is insufficient for the current audio route. Do not treat a level on the TX meter as confirmation that audio reaches the radio. |

Unavailable actions remain visible with a tooltip and an accessible reason. Manual handoff requires an idle radio with no local transmit intent or profile load in progress; taking also requires a TX slice confirmed as belonging to this client. Radio families without DAX TX support keep both actions disabled.

<img src="/img/screens/dax-applet.png" width="248" alt="DAX Audio applet. Level bars for DAX 1, assigned to Slice A, DAX 2 to DAX 4, unassigned, and TX, assigned to Slice A, above a DAX row with a Disabled button." />

*The DAX Audio applet on Linux: one level bar per DAX channel and the button that starts DAX.*

## Known issues

- On macOS, a digital-mode program's transmit audio can reach the **AetherSDR TX** meter while the radio sends no RF ([#4554](https://github.com/aethersdr/AetherSDR/issues/4554)).
- On macOS, DAX transmit can stop sending audio to the radio altogether until AetherSDR is restarted ([#5870](https://github.com/aethersdr/AetherSDR/issues/5870)).
- On macOS with a FLEX-8000-series radio, DAX receive audio can come out chopped and very quiet ([#3837](https://github.com/aethersdr/AetherSDR/issues/3837)).

## Troubleshooting

### The program hears no audio

The slice has no DAX channel, or the program is reading a different device.

1. On FlexRadio, set a **DAX Ch** on the slice's **VFO widget → DAX tab**. On RTL-SDR, check the automatic slice assignment shown in the DAX applet.
2. In the program, choose the matching **AetherSDR DAX n** input.
3. On Linux, `pactl list source-outputs` shows which device each program is reading.

### Transmit audio does not reach the radio

The program sends to the wrong device, the TX slice is in a voice mode, or this client does not own its DAX TX stream.

1. Set the program's audio output to **AetherSDR TX**.
2. Put the TX slice in a digital mode (DIGU or DIGL).
3. With the radio idle and your local TX slice selected, click **Take DAX TX**. Wait for **DAX TX: Owned** without **Audio blocked**, then initiate a new transmission in your program.
4. If taking is disabled, read its tooltip for the current reason. Resolve an active transmission or profile load before requesting a handoff.

### A DAX row is missing

The connected radio has fewer slices than that channel number, so AetherSDR does not show it. Use a lower channel number.

### WSJT-X shows "Error in Sound Input"

DAX was switched off while WSJT-X was using it.

1. Turn DAX back on.
2. Restart WSJT-X.

### There is no DAX on Windows

AetherSDR ships no DAX driver on Windows.

1. Use the [TCI Server](./tci-server.md), which carries audio and CAT over one connection, or FlexRadio's own SmartSDR DAX.

## See also

- [DAX IQ Streaming](./dax-iq-streaming.md)
- [CAT Control](./cat-control.md)
- [TCI Server](./tci-server.md)
- [WSJT-X Integration](./wsjt-x-integration.md)
- [AetherModem Packet Radio](./aethermodem-packet-radio.md)
