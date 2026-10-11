---
title: "Before You Transmit"
slug: "/before-you-transmit"
description: "Work through this checklist before you key up with AetherSDR for the first time, and again after a big change to your station, your computer or AetherSDR's settings."
---

Work through this checklist before you key up with AetherSDR for the first time, and again after a big change to your station, your computer or AetherSDR's settings. Each item says what to check and links to the page that explains it. Items marked **(FlexRadio)** apply only to a FlexRadio; the rest apply to every radio AetherSDR can transmit on.

The checks are written so you can do them without putting a signal on the air. Where you do need to transmit to check something, use low power into a dummy load or a clear frequency, and identify as your licence requires.

## Requirements

- A radio that can transmit: a FlexRadio, a Hermes-Lite 2 or a recognised networked Icom. The ANAN-G2, RTL-SDR dongles, KiwiSDR/Web-888 receivers and the demo are receive-only. See [Supported Radios](./supported-radios.md).
- AetherSDR connected to it. See [First Connection](./first-connection.md).
- A licence that covers the bands and modes you plan to use.

## The checklist

### The radio and the transmit slice

- [ ] **The right radio is connected.** Its tab is active in the title bar, and it is not the demo ("Simulator (not on the air)"). See [First Connection](./first-connection.md).
- [ ] **You know which slice transmits.** The transmit slice has the **red TX badge** on its VFO. Switching the active slice does not move transmit; click a grey **TX** badge to move it. See [Multi-Slice Operation](./multi-slice-operation.md) and [VFO Widget](./vfo-widget.md).
- [ ] **The transmit slice is on a frequency and mode you are licensed for.** Check the band plan strip along the bottom of the panadapter. See [Panadapter Controls](./panadapter-controls.md).
- [ ] **Split is what you expect.** If a red **SPLIT** badge shows, you transmit on the second slice, not where you are listening. Click the red **SPLIT** badge to end the split. See [Split Operation](./split-operation.md).

### Your station identity

- [ ] **Your callsign is set** in **Settings → Radio Setup... → Radio → Callsign:** (on macOS, **AetherSDR → Preferences... → Radio**). AetherSDR uses it for PSK Reporter, WSPR and spotting; on a FlexRadio it is also stored on the radio. See [Radio Setup](./radio-setup.md).
- [ ] **Your callsign and grid are set wherever a feature transmits them.** AetherSDR has no single station grid; each feature that sends one has its own fields:
  - [RADE Digital Voice](./rade-digital-voice.md) end-of-over frames and FreeDV Reporter: **SpotHub → FreeDV**, **Callsign** (or **Use radio**) and **Grid Square** (or **Use GPS**). See [SpotHub](./spothub.md).
  - The WSPR beacon in **Tools → PSK Reporter...**: **TX call:** and **Grid:**. See [PSK Reporter Map](./psk-reporter-map.md).
  - APRS in [AetherModem Packet Radio](./aethermodem-packet-radio.md): **MY CALLSIGN**, and **GRID** when the radio has no GPS fix.
  - D-STAR: **MYCALL**, which defaults to the radio's callsign. See [D-STAR (ThumbDV)](./d-star-thumbdv.md).
  - Digital-mode programs such as WSJT-X use the callsign and grid in their own settings.

### Microphone and transmit audio

- [ ] **The microphone path is the one you mean to use.** On a FlexRadio, **Mic source** in the **P/CW** applet chooses MIC, BAL, LINE, ACC or PC; for a microphone on this computer, choose **PC**. On a radio that takes its transmit audio over the network, such as a networked Icom, turn on **PC Audio** in the title bar. See [TX Controls](./tx-controls.md).
- [ ] **The computer's Input device is your real microphone,** not a virtual device such as a DAX microphone. Check **Settings → Radio Setup... → Audio**. See [Audio Settings](./audio-settings.md).
- [ ] **The Mic level gauge in the P/CW applet moves when you speak.**
- [ ] **AetherTX is set up, if you use a computer microphone.** AetherTX shapes only a microphone on this computer. Open **Tools → AetherTX...**, check which stages are ticked, and set the **Gate** stage so it opens on your voice and closes between words. Use **REC / PLAY** to record and hear your processed audio **without keying the radio**. See [Aetherial Audio](./aetherial-audio.md).

### Power, tuner and antenna

- [ ] **RF Power and Tune Power are at sensible levels** in the **TX** applet (both 0–100 %). Start low for a first test. See [TX Controls](./tx-controls.md).
- [ ] **(FlexRadio)** **The radio's power limits suit your station:** **Max Power** on the **Transmit** page of Radio Setup, and per-band RF and tune power and PTT inhibit in **Settings → TX Band Settings...**. See [Radio Setup](./radio-setup.md).
- [ ] **The transmit antenna is the one you mean.** The RX applet header shows the RX and TX antenna buttons, with the names you gave them in **Settings → Radio Setup... → Antennas**. See [RX Controls](./rx-controls.md).
- [ ] **The antenna is matched.** Run the radio's **ATU** from the TX applet (FlexRadio), or your external tuner, at low tune power, and check the **SWR** gauge. A Tuner Genius XL has its own applet; see [TGXL Tuner Control](./tgxl-tuner-control.md). **Tools → Pre-tune ATU Bands...** keys the transmitter repeatedly, so make sure the bands are clear first.
- [ ] **External amplifiers are in STANDBY** until everything above checks out, and while you tune or sweep. The amplifier applets have **STANDBY** / **OPERATE** buttons; the PGXL has a **STBY** key. See [Amplifiers](./amplifiers.md) and [Peripherals](./peripherals.md).
- [ ] **(FlexRadio)** **TUNE will not key anything you don't want it to:** **Settings → Inhibit during TUNE ▸** chooses which outputs (ACC TX, TX1, TX2, TX3) are held while the radio tunes. See [TX Controls](./tx-controls.md).

### Digital modes

- [ ] **The program's audio route works.** On Linux and macOS with a FlexRadio, [DAX Virtual Audio](./dax-virtual-audio.md) gives each slice a virtual audio device. On Windows, and on radios without DAX, use the [TCI Server](./tci-server.md). See [WSJT-X Integration](./wsjt-x-integration.md).
- [ ] **CAT or TCI control works before you transmit.** Changing frequency and mode in the program should move the transmit slice in AetherSDR, and the program should key the slice you expect. See [CAT Control](./cat-control.md) and [TCI Server](./tci-server.md).
- [ ] **(FlexRadio)** **The TX filter passes your tones.** If AetherSDR shows a TX filter card on the panadapter when you transmit, widen **TX Filter** in the **Phone** applet. See [TX Controls](./tx-controls.md).
- [ ] **Your computer's clock is accurate.** FT8 and similar modes fail when the clock is off by more than about a second. Keep the computer synchronised with NTP. [AetherClock and GPS](./aetherclock-and-gps.md) shows how far the computer's clock is from WWV/WWVB or the radio's GPS.

### Keying paths and timers

- [ ] **You know every way AetherSDR can be keyed.** Check each one you have set up:
  - **Keyboard shortcuts** are off by default. If you turn them on in **Settings → Keyboard Shortcuts**, **Space** is push-to-talk while held and **T** toggles MOX. See [Keyboard Shortcuts](./keyboard-shortcuts.md).
  - Controller buttons can be bound to PTT, MOX or TUNE: [USB Control Surfaces](./usb-control-surfaces.md) (including serial PTT and foot switches), [MIDI Controller Mapping](./midi-controller-mapping.md), [Ulanzi Dial](./ulanzi-dial.md), [StreamDeck](./streamdeck.md) and the [FlexControl Tuning Knob](./flexcontrol-tuning-knob.md).
  - CAT and TCI clients can key the radio. See [CAT Control](./cat-control.md) and [TCI Server](./tci-server.md).
  - The automation bridge cannot key the transmitter unless **Allow TX via MCP** is ticked, which is off by default. Leave it off unless you need it. See [Automation Bridge and MCP](./automation-bridge-and-mcp.md).
- [ ] **(FlexRadio)** **The radio's transmit time-out and interlocks are set:** **Timeout (sec):** and the **Interlocks - TX REQ** group on the **Transmit** page of Radio Setup. See [Radio Setup](./radio-setup.md).
- [ ] **You can see when you are transmitting.** A green `M:SS` transmit timer appears in the title bar, just left of **PC Audio**, for each of your own transmissions, and the VFO marker turns orange/red. If an interlock blocks transmit, a notification says why. See [TX Controls](./tx-controls.md).

### First transmission

- [ ] **The frequency is clear,** or you are on a dummy load.
- [ ] **Press MOX (or your PTT) briefly at low power** and watch the **Fwd Power** and **SWR** gauges in the TX applet. Unkey at once if the SWR is high or the power is not what you expect.

## Troubleshooting

### The radio keys but puts out almost no RF in DIGU/DIGL

The TX filter's low and high cut leave out your transmit audio.

1. In the **Phone** applet, widen the **TX Filter** with the **Low Cut** and **High Cut** buttons until it covers your tones.
2. Transmit again and check that the panadapter warning card no longer appears.

### Transmit audio is missing with Mic source set to PC

The computer's selected microphone is not your microphone, for example a virtual DAX microphone.

1. Open **Settings → Radio Setup... → Audio** and choose your real microphone as the **Input** device.
2. Check that the **Mic level** gauge in the **P/CW** applet moves when you speak.

### Space or another key does not key the radio

Operating keyboard shortcuts are off by default.

1. Turn them on with **Settings → Keyboard Shortcuts**.
2. Check the binding in **Settings → Configure Shortcuts...**.

### A notification says transmit is blocked

An interlock is holding transmit off, for example an amplifier warming up, the ATU busy, or a band the radio does not allow.

1. Read the notification for the reason.
2. Wait for the amplifier or ATU to finish, or move to a band you may transmit on, then key again.

## See also

- [TX Controls](./tx-controls.md)
- [Aetherial Audio](./aetherial-audio.md)
- [Amplifiers](./amplifiers.md)
- [WSJT-X Integration](./wsjt-x-integration.md)
- [Keyboard Shortcuts](./keyboard-shortcuts.md)
- [Your First Session](./your-first-session.md)
