---
title: "RADE Digital Voice"
slug: "/rade-digital-voice"
description: "AetherSDR includes RADE v1, an AI-based digital voice codec from the FreeDV project."
status: "Supported"
applies_to: ["FlexRadio"]
---

:::info[Status]

**Status:** Supported · **Applies to:** FlexRadio

:::

AetherSDR includes RADE v1, an AI-based digital voice codec from the [FreeDV](https://freedv.org/) project. RADE uses a neural network to encode speech into an OFDM waveform for transmission over HF radio, then decode it back to speech on the receiving end. All the encoding and decoding happens on your PC; the radio just carries the waveform in a data mode.

The radio is set to DIGU or DIGL (SSB passthrough) — it just transmits and receives the raw OFDM waveform. On your PC:

- **TX path:** microphone → LPCNet feature extraction → RADE encoder → OFDM modulation → radio transmits
- **RX path:** receive audio from the radio → RADE demodulator → neural decoder → FARGAN vocoder → speaker

*Contributed by @pepefrog1234*

## Requirements

- **A FlexRadio.** RADE's modem receives on a DAX channel, and only a FlexRadio offers one. On other radio families AetherSDR refuses to start RADE.
- **A build with RADE support.** RADE appears in the mode list only when AetherSDR is built with it, which the Linux, macOS and Windows release builds include.
- **Both stations** must be running RADE — one station transmits RADE OFDM, the other decodes it.
- Compatible with other FreeDV RADE v1 implementations (FreeDV GUI, etc.).

## Using RADE

### Turn RADE on

Select **RADE** from the mode dropdown in either the RX applet or the VFO widget. AetherSDR then:

- sets the radio to **DIGU** or **DIGL** using the band's usual sideband (LSB bands get DIGL; 60 m uses DIGU);
- sets the filter to **750–2250 Hz** (mirrored for DIGL);
- makes the RADE slice the **TX slice**.

RADE stays active across PTT cycles. It switches itself off if the radio or another client changes the slice to a mode other than DIGU/DIGL. If the RADE engine fails to start, the slice's previous mode, filter and TX assignment are restored.

You can also start RADE from **Tools → FreeDV Reporter...**: double-clicking a station tunes to it and switches the slice to RADE.

### Read sync and SNR

While RADE is active, the VFO flag shows:

- a **RADE** badge with a sync LED next to the frequency — **green ●** when the modem is synchronised, grey ○ when it is not;
- a **RADE info row** below the frequency with the far-end **callsign** (when received), the decoded **SNR** and the **frequency offset**.

The same readouts are in the **RADE Status** applet (button **RADE**, in the applet drawer, closed by default), which shows "RADE inactive" when RADE is off.

The RADE modem uses pilot symbols for synchronisation and can acquire signals down to approximately 0 dB SNR.

### End-of-over callsign

At the end of each over, RADE sends an **end-of-over (EOO)** frame that carries the transmitting station's callsign.

- **Receiving:** the far end's callsign is decoded from its EOO frame, shown in the RADE info row and the RADE Status applet, and posted to FreeDV Reporter when reporting is on.
- **Transmitting:** AetherSDR encodes **your** callsign into your EOO frame and holds the transmitter until the frame has played out. The callsign comes from **SpotHub → FreeDV**: the radio's callsign when **Use radio** is ticked, otherwise the **Callsign** you entered there. If neither is set, no callsign is sent.

### Report to FreeDV Reporter

FreeDV Reporter is the FreeDV community's live view of who is on the air.

- Turn on reporting in **SpotHub → FreeDV** with **Enable FreeDV Reporter reporting when RADE is active**, and set **Callsign** (or **Use radio**), **Grid Square** (or **Use GPS**) and **Station Msg**.
- **Tools → FreeDV Reporter...** opens the reporter panel. Its **Message** field is the same setting as SpotHub's **Station Msg** — the two stay in sync, and changes go out live. The field is disabled while reporting is off.
- Double-click a station in the panel to tune to it and force RADE.

See [SpotHub](./spothub.md) for the FreeDV spot feed.

## Reference

| Item | Value |
|---|---|
| **Codec** | RADE v1 (Radio Autoencoder) by David Rowe VK5DGR |
| **Library** | [radae_nopy](https://github.com/drowe67/radae_nopy) (BSD-2 license), bundled |
| **Dependency** | Opus with FARGAN/LPCNet, built from a vendored snapshot — nothing is downloaded during the build |
| **Resampling** | r8brain-free-src polyphase resampler for all rate conversions |
| **Mode selection** | DIGU or DIGL following the band's conventional sideband |
| **Filter** | 750–2250 Hz for DIGU, −2250 to −750 Hz for DIGL (set automatically) |

## Troubleshooting

### "RADE Unavailable" when selecting RADE

The connected radio has no DAX audio. RADE's modem receives on a DAX channel, and only a FlexRadio provides one, so AetherSDR declines and resets the RADE control. The slice's mode is left as it was.

1. Connect to a FlexRadio to use RADE.

### RADE is missing from the mode list

The build was made without RADE support.

1. Install a Linux, macOS or Windows release build, which include RADE.

### RADE turned itself off

The radio or another client changed the slice to a mode other than DIGU/DIGL.

1. Select **RADE** again from the mode dropdown.

### The far end receives no callsign

No callsign is set for the end-of-over frame.

1. Open **SpotHub → FreeDV**.
2. Tick **Use radio**, or enter your **Callsign**.

## See also

- [SpotHub](./spothub.md)
- [DAX Virtual Audio](./dax-virtual-audio.md)
- [TX Controls](./tx-controls.md)
- [FlexRadio](./flexradio.md)
- [FreeDV project](https://freedv.org/)
- [radae_nopy](https://github.com/drowe67/radae_nopy)
