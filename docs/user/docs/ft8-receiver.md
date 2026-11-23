---
title: "FT8 Receiver"
slug: "/ft8-receiver"
description: "A receive-only FT8 decoder that runs on a FLEX-6000 itself, with no PC, and feeds its spots to SpotHub and the panadapter."
status: "Experimental, receive-only"
applies_to: ["FLEX-6000"]
platforms: ["Linux", "macOS", "Windows"]
---

:::info[Status]

**Status:** Experimental, receive-only · **Applies to:** FLEX-6000 · **Platforms:** Linux, macOS, Windows

:::

The FT8 receiver is a small program that runs on the radio and decodes FT8 around the clock, whether or not a computer is connected. It listens on a slice of its own and decodes each 15-second cycle. AetherSDR installs it, configures it and shows what it hears.

It is receive-only: it never transmits, and its slice is never the transmit slice. It doesn't upload its spots anywhere.

It finds fewer stations than WSJT-X on a crowded band. It decodes 77.3% of the messages in a set of reference recordings, and it doesn't subtract a decoded signal to look for a weaker one underneath.

## Requirements

- A FLEX-6000 series radio, where it runs as a legacy waveform. On a FLEX-8000 or Aurora the **FT8 Receiver** entry is dimmed.
- AetherSDR connected to the radio over its **local network**. SmartLink can't reach the receiver.
- One free slice. The receiver takes one of the radio's slices for as long as it runs. On a two-slice radio such as the FLEX-6300, that leaves one.
- One free DAX channel.
- The radio's clock synchronized. The receiver waits for it before it records.

## Setup

1. Open **Tools → Waveforms...**
2. Click **Install** and choose **FT8 Receiver**.

The receiver ships inside AetherSDR, so nothing is downloaded. It takes less than 200 KB on the radio. It listens on 20 m (14.074 MHz) until you change that.

To remove it, click **Uninstall** on its **AetherFT8** row. This also removes its settings and its slice.

To update it, uninstall it and install it again. That starts it over with default settings.

## Configuring the receiver

Click **Configure...** on the **AetherFT8** row in the Waveforms window, or in **SpotHub → FT8**. The receiver keeps these settings itself, in a file on the radio, so they apply with no computer connected. Click **Apply** to send your changes to the radio. The receiver applies them on its next cycle.

The first computer to click **Apply** claims the receiver: AetherSDR keeps the receiver's admin token in your system keychain, and only that computer can change the settings afterward. On another computer, reinstall the receiver to start over.

The top of the window shows what the receiver is doing, such as **Listening on 20 m** or **Waiting for the radio's clock**, and how long its last decode took.

## Seeing what it hears

Open **Settings → SpotHub...** and choose the **FT8** tab, then click **Start**, or turn on **Auto-Start** to start reading whenever you connect to a radio that has the receiver. See [SpotHub](./spothub.md#ft8).

- The tab's console lists every decode with its time, band, frequency, score and message.
- Each station appears on the panadapter as a spot, when its frequency is in view.

The receiver keeps decoding on the radio whether or not AetherSDR is reading it.

## Reference

### Station

**CALLSIGN** and **GRID** name the receiving station. Leave them empty to use the radio's callsign and its GPS grid; the empty fields show what those are.

### Receive

| Setting | What it does |
|---|---|
| **BAND** | The band the receiver listens on, 2200 m to 2 m, at the standard FT8 frequency for that band. It stays there: band hopping isn't set up from this window |
| **ANTENNA** | The receive antenna for its slice; **Radio default** leaves the radio's choice |
| **DAX CHANNEL** | The DAX audio channel its slice uses, 3 by default. It must differ from any other receiver's: the WSPR receiver starts on 4. If the radio refuses the channel, the receiver steps down to the next one |

### What the receiver does to the radio

- Its audio never leaves the radio. Its slice stays muted, and if any program unmutes it, the receiver mutes it again. It records from DAX, which the mute doesn't affect.
- AetherSDR leaves the receiver's slice alone: its letter shows as taken, labeled **FT8 receiver**, and AetherSDR opens a slice of its own. Other programs, SmartSDR among them, may still show it as theirs. Don't tune it there, because the receiver moves it back.
- If another program makes the receiver's slice the transmit slice, or takes it out of DIGU, the receiver sets it back. It doesn't choose another transmit slice for you, so pick one again.
- The score in the decode list and in a spot's comment is the decoder's own measure of signal strength. It isn't an SNR in dB.

## Known issues

These are limits of the receiver as approved in [#7114](https://github.com/aethersdr/AetherSDR/issues/7114).

- On a busy band, a decode can take longer than the 2 seconds a cycle allows, and the next cycle is then missed. The Configure window says so when the last decode ran over. Measured on a FLEX-6300 on 20 m, more than half of the cycles ran over ([#7114](https://github.com/aethersdr/AetherSDR/issues/7114)).
- On a FLEX-6300, running it beside the [WSPR receiver](./wspr-receiver.md) slows both. In one test the FT8 receiver completed 20 of 24 cycles and the WSPR receiver about 7 of 12 ([#7114](https://github.com/aethersdr/AetherSDR/issues/7114)).
- On a FLEX-6300, **Radio default** puts the receiver's slice on ANT1. If your antenna is on ANT2, it decodes nothing until you set **ANTENNA** ([#7114](https://github.com/aethersdr/AetherSDR/issues/7114)).
- **DAX CHANNEL** offers 1 to 4 on every radio, including a radio that has only two ([#7114](https://github.com/aethersdr/AetherSDR/issues/7114)).

## Troubleshooting

### The receiver is running but decodes nothing

The slice is probably on an antenna with nothing connected.

1. Open **Configure...** on the **AetherFT8** row.
2. Set **ANTENNA** to the antenna you receive on.
3. Click **Apply** and wait for the next cycle.

### Configure says "Not reachable"

AetherSDR reaches the receiver only on the radio's local network.

1. Check that you're connected to the radio directly, not through SmartLink.
2. Check that **AetherFT8** is listed in **Tools → Waveforms...**

### Configure says another computer set this receiver up

Only the computer that first clicked **Apply** can change the settings.

1. Open **Tools → Waveforms...**
2. Click **Uninstall** on the **AetherFT8** row.
3. Click **Install** and choose **FT8 Receiver**, then configure it from this computer.

## See also

- [WSPR Receiver](./wspr-receiver.md)
- [SpotHub](./spothub.md)
- [FlexRadio](./flexradio.md)
