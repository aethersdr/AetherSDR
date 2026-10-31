---
title: "Networked Icom"
slug: "/networked-icom"
description: "AetherSDR can operate Icom transceivers that have a built-in network connection, using Icom's CI-V commands over the RS-BA1 network protocol."
status: "Early"
applies_to: ["Networked Icom"]
---

:::info[Status]

**Status:** Early · **Applies to:** Networked Icom

:::

AetherSDR can operate Icom transceivers that have a built-in network connection, using Icom's CI-V commands over the RS-BA1 network protocol. No Icom software and no USB cable are needed.

The **IC-7300MK2** is supported and shows no EXPERIMENTAL badge. Every other Icom shows the **EXPERIMENTAL** badge in the title bar and an "Experimental radio support" notice when you connect. FlexRadio remains AetherSDR's supported target; see [Supported Radios](./supported-radios.md).

## Where to go next

- [Before You Transmit](./before-you-transmit.md) and [TX Controls](./tx-controls.md).
- [TCI Server](./tci-server.md) and [WSJT-X Integration](./wsjt-x-integration.md): FT8, WSPR and other digital modes.
- [Aetherial Audio](./aetherial-audio.md): the transmit audio chain, with PC Audio on.
- [CWX Panel](./cwx-panel.md): CW text through the radio's own keyer (CWK).
- [CW Decoder](./cw-decoder.md), [RTTY Operation](./rtty-operation.md) and [AetherModem Packet Radio](./aethermodem-packet-radio.md).
- [Memory Channels](./memory-channels.md): the shared bank, with import from the radio.
- [USB Control Surfaces](./usb-control-surfaces.md): the Icom RC-28 remote encoder.

## Requirements

| Model | Network | Status |
|-------|---------|--------|
| **IC-7300MK2** | Built-in Ethernet | **Supported**: no EXPERIMENTAL badge |
| **IC-705** | Wi-Fi | Early (experimental); verified against its CI-V guide |
| **IC-9700** | Ethernet | Early (experimental); works, with read-only memories |
| IC-7610, IC-785x, IC-905 | Ethernet | Listed in the model chooser, not verified |

The original IC-7300 has no network port and is not in the model list.

## Setup

### On the radio

Enable network (remote) control and create a network user name and password. AetherSDR uses the radio's three UDP ports, which start at 50001 by default (control, CI-V, audio). The IC-705 must be on your Wi-Fi network; the IC-7300MK2 and IC-9700 use Ethernet.

### Connecting

Networked Icoms are not discovered automatically. Open **File → Connect to Radio...**, choose **Connect by IP** and set **Radio type:** to **Icom (network)**. The form gains these rows:

<img src="/img/screens/connect-icom-rows.png" width="734" alt="Radio IP address group with Radio type: Icom (network), Radio IP: 192.0.2.30, empty Icom user and Icom password fields, Icom ports: Standard (50001–50003), Icom CI-V: Auto-detect (recommended), Network Diagnostics and Connect by IP buttons, and an Advanced: choose the VPN source path expander." />

*Connect by IP with Radio type set to Icom (network): the Icom user, password, ports and CI-V rows appear.*

| Row | What to enter |
|-----|---------------|
| **Radio IP:** | The radio's IP address or host name. |
| **Icom user:** | The network user name configured on the radio. |
| **Icom password:** | The network password configured on the radio. It is stored in the operating system's credential store, never in the settings file, and only after a successful connect. |
| **Icom ports:** | **Standard (50001–50003)**, or **Custom NAT ports...** with a **First UDP port:**. AetherSDR uses that port and the next two. Different port triplets let you reach several radios behind one NAT router. |
| **Icom CI-V:** | **Auto-detect (recommended)** asks the radio for its own CI-V address. You can also pick a model from the list, or **Custom...** and type a hex **CI-V address:** (for example `A2`, from the radio's MENU > SET > Connectors > CI-V). A wrong address is not silently corrected: the connection gets no frequency, panadapter or transmit. |

Click **Connect by IP**. The model is identified over the wire, not from the radio's name. If the handshake fails, the message names the target and the likely cause.

### Wake Icom on connect

**Wake Icom on connect**, near the bottom of the Connect to Radio window, is off by default. When it is on and the radio does not answer identification, AetherSDR wakes a supported model (IC-705, IC-7300MK2 or IC-9700) from standby once. It never puts the radio to sleep on disconnect. The radio's network interface must stay reachable in standby, so a Wi-Fi radio that has dropped off the network cannot be woken. For a custom CI-V address, pick the model in **Icom CI-V:** so AetherSDR knows which radio to wake.

## Using a Networked Icom

AetherSDR treats an Icom differently from a FlexRadio:

- **The radio is authoritative.** It remembers its own frequency, mode and filter. AetherSDR reads the radio's state and levels when it connects and never pushes saved state onto it. The one thing it initializes is the scope mode, described under [Panadapter scope modes](#panadapter-scope-modes).
- **Unknown models fail closed.** Each verified model has its own command profile. A control that has not been proven on a model is hidden, and a radio AetherSDR does not recognize gets no panadapter and no transmit, rather than guessed defaults.

What you can do:

- **Panadapter** from the radio's own scope. Every Icom has **one panadapter**.
- **Receive and transmit:** RX audio, transmit, RF gain, preamp and attenuator (each separate from the display's RF gain), AF gain, TX monitor, VOX, RIT/XIT, manual notch and the ATU on models that have one. TUNE power changes made during a tune are kept.
- **Filters:** the radio's real IF width is read back, the filter buttons are ordered narrow to wide for the mode, twin PBT is supported, and TX low/high cut follow what the model supports.
- **Status bar:** the radio's **Network Radio Name** (the name set in its network settings) is shown, separate from model and callsign.
- **Radio Health** (**Tools → Radio Health...**) shows the radio's modulation inputs and levels.
- **Decoders:** the CW, RTTY and AetherClock decoders work. The CW decoder opens in CW.
- **Memory channels** use AetherSDR's shared bank on this computer, and the radio's own memories can be read in on models that support it. The IC-9700's radio memories are read-only.

### Panadapter scope modes

The panadapter is the radio's own scope, and the radio owns the scope mode. On the IC-7300MK2, the IC-705 (firmware 1.20 or later) and the IC-9700 (firmware 1.30 or later) the **Scope** row in the Display panel offers the radio's three modes:

| Mode | What the window does |
|---|---|
| **Scroll-F** | The window stays put while you tune inside it, like a FlexRadio panadapter. Click to tune in place, drag the panadapter to pan the window, and the window follows the tuned frequency when it reaches an edge. This is the mode AetherSDR starts in. |
| **Fixed** | The window is one of the radio's stored Fixed Edges presets and does not move on its own. |
| **Center** | The window is centered on the tuned frequency and moves with every tune. Dragging the panadapter retunes the radio. |

**At connect, AetherSDR reads the radio's scope mode and edge preset.** A radio found in Scroll-F or Fixed is left exactly as it was: nothing is written. A radio found in Center is switched to Scroll-F once per session, so the panadapter never starts slaved to the VFO. Picking Center in the Display panel is honored for that session, and the next connect initializes Scroll-F again. Picking Scroll-F or Fixed is written to the radio and stays across reconnects.

**AetherSDR's window is the radio's Fixed Edges preset 4.** In Scroll-F and in Fixed chosen from the Display panel, every pan and zoom rewrites preset 4 for the band segment you are on; presets 1–3 are never written. If the radio is in Fixed on one of presets 1–3 (chosen on its front panel), the panadapter shows that preset as it is, no Scope button is lit, and a drag or zoom is refused with the view put back; click **Fixed** to switch the radio to preset 4 with the same window, after which the window is yours to pan and zoom. In Scroll-F on one of presets 1–3, the radio stays on that preset until the window first has to move: a drag or zoom in AetherSDR, or a dial move that reaches the edge of the window, switches the radio to preset 4 and writes there. The window cannot cross one of the radio's band-segment boundaries (for example 8.000 MHz on HF), so near a boundary it shifts rather than centers.

Tuning works the same way as on other radios: click, drag the tuned-frequency marker, scroll the wheel, type a frequency, or recall a memory. A tune that leaves the window brings the window with it. The original IC-7300 has Center only, and the Scope row is dimmed with that reason. On the IC-9700 the panadapter follows one scope, MAIN or SUB, and that is the one read and initialized; in Dual scope the other scope is not drawn.

### PC Audio and data modes

The **PC Audio** toggle chooses the radio's DATA OFF modulation input: **WLAN** on the IC-705, **LAN** on the IC-7300MK2, and **MIC** when PC Audio is off. The radio's DATA MOD setting is left alone. On a model AetherSDR cannot verify, the toggle refuses.

**DIGU**, **DIGL** and **DFM** set the radio's DATA flag, so transmit audio comes from the computer, not the microphone. If the radio's modulation input is wrong, AetherSDR tells you what to change.

With PC Audio off, the Aetherial Audio TX controls are replaced by a note that names the fix.

### FT8, WSPR and other digital modes

WSJT-X and similar programs work through the [TCI Server](./tci-server.md): FT8 decoding and PSK Reporter spotting, and WSPR transmit (proven on air from an IC-7300MK2). The built-in AX.25 modem can transmit on an Icom too. DAX is not available on an Icom.

### CW text: CWK

On an Icom the status-bar CWX toggle reads **CWK**: CWX text is sent through the radio's own CI-V text keyer, on models that have one (IC-705, IC-7300MK2). It drives the same [CWX Panel](./cwx-panel.md).

### FM repeaters and XFC

The FM controls set tone, duplex and offset where the model supports them. On an Icom, the FM **REV** button becomes **XFC**, a momentary transmit-frequency check; it always releases when hidden or on disconnect. The IC-705 and IC-9700 support the extended tone registers (including DTCS); the IC-7300MK2 supports tone and tone squelch, and its offset controls are disabled.

### Controllers

The Icom RC-28 remote encoder is supported (**Settings → Controllers → Icom RC-28 Remote Encoder...**); see [USB Control Surfaces](./usb-control-surfaces.md).

## Reference

### IC-7300MK2

- TX bandwidth table; CW speed 6–48 WPM; CW pitch in 5 Hz steps.
- The speech processor button reads **COMP**, and its slider is the radio's own COMP level, 0–10, with a numeric readout.
- Squelch works in CW and data modes. The panadapter squelch line uses the MK2's own measured scale in SSB, CW, AM and data modes; FM, DFM and WFM have no squelch line. **Auto SQL is not available** on the MK2.
- AGC threshold, AGC Off, AM carrier, VOX delay and repeater offset are disabled. TUNE is refused in CW and CW-R.
- The preamp and ATT buttons update about 0.2 s after a change, including the one the radio switches by itself: engaging ATT drops the preamp, and engaging the preamp drops ATT.

### IC-705

- On the IC-705, the preamp cycles through **OFF**, **P.AMP1** and **P.AMP2** below 74.800 MHz. At 74.800 MHz and above it toggles **OFF** / **P.AMP1**. The choices follow the radio's reported frequency; until that frequency is known, only OFF and P.AMP1 are offered. Changing ranges does not change the radio's preamp setting; its readback takes priority over routine meter polling. After reconnecting, S-meter readings wait for the radio's frequency so they use the correct HF or VHF reference.
- 2 m and 70 cm are available alongside HF.
- The speech processor button reads **COMP**, with a 0–10 slider and a numeric readout, as on the MK2.
- **WFM** (broadcast FM) is receive-only.
- GPS location and NTP time controls. The power gauge uses a QRP scale.
- AetherSDR warns if the radio's MOD Input is not WLAN.

### IC-9700

- Three bands with their own power ceilings (100 / 75 / 10 W).
- CTCSS (TX and RX), DTCS, dial-lock sync, supply voltage, PA drain current, continuous compression and LAN MOD level. The **COMP** slider runs 0–100, with a numeric readout.
- If CI-V stalls, AetherSDR tries a bounded automatic recovery before a full reconnect.
- Its MAIN/SUB dual-receiver layout is not modeled.

### Not available on a networked Icom

- DAX, SmartLink or Multi-Flex.
- More than one panadapter.
- Anything a model has not been proven to support: those controls are hidden, dimmed with a reason, or refuse with a notice.

## Known issues

- On the IC-7300MK2, transmit audio drops out for about a third of a second roughly every 1.2 seconds while the radio stays keyed ([#6169](https://github.com/aethersdr/AetherSDR/issues/6169)).
- CW speed, break-in and pitch do nothing on models without a CW text keyer, such as the IC-9700 ([#6110](https://github.com/aethersdr/AetherSDR/issues/6110)).
- On the IC-9700, the panadapter trace can sit on the bottom line and look blank on a quiet band with the preamp off ([#4814](https://github.com/aethersdr/AetherSDR/issues/4814)).
- In Scroll-F, the IC-7300MK2 sends no scope data while it is transmitting, so the panadapter freezes for the length of a transmission; in Center it keeps sweeping ([#6168](https://github.com/aethersdr/AetherSDR/issues/6168)).

## Troubleshooting

### Connected, but no frequency, panadapter or transmit

The **Icom CI-V:** address is wrong. AetherSDR does not correct a wrong address silently.

1. Disconnect and open **Connect by IP** with **Radio type:** set to **Icom (network)**.
2. Set **Icom CI-V:** to **Auto-detect (recommended)**.
3. If you use **Custom...**, check the address on the radio under MENU > SET > Connectors > CI-V and type it as hex (for example `A2`).

### Audio and radio updates stop after switching applications

An interrupted application shutdown can leave an old network session on the radio. If the radio releases the current audio and CI-V streams while cleaning it up, AetherSDR disconnects and retries the connection automatically.

1. Allow the automatic reconnect to finish. Frequency and other radio state are read again from the radio.
2. When switching applications, disconnect the first application and let it finish before opening the next connection.
3. If the radio continues refusing connections, check that another network client is not still using it.

### Connect by IP fails with a handshake error

The radio is not accepting network control from AetherSDR. The message names the target and the likely cause.

1. Check that network (remote) control is enabled on the radio.
2. Check **Icom user:** and **Icom password:** against the network user configured on the radio.
3. Behind a NAT router, check that **Icom ports:** matches the three forwarded UDP ports.

### The radio is in standby and does not answer

AetherSDR does not wake the radio unless you ask it to.

1. Tick **Wake Icom on connect** near the bottom of the Connect to Radio window.
2. For a custom CI-V address, also pick the model in **Icom CI-V:**.
3. On an IC-705, make sure the radio is still on your Wi-Fi network in standby; a radio that has dropped off the network cannot be woken.

### Data-mode transmit warns about the modulation input

The radio's modulation input does not match the PC Audio setting.

1. Toggle **PC Audio** off and on to select the right input.
2. Or set it on the radio's front panel: MENU > SET > Connectors > MOD Input (WLAN on the IC-705, LAN on the IC-7300MK2).

## See also

- [Supported Radios](./supported-radios.md)
- [Manual Connection](./manual-connection.md)
- [TCI Server](./tci-server.md)
- [USB Control Surfaces](./usb-control-surfaces.md)
- [`docs/architecture/icom-capability-profiles.md`](https://github.com/aethersdr/AetherSDR/blob/main/docs/architecture/icom-capability-profiles.md)
- [`docs/architecture/aetherd-icom-civ-backend-design.md`](https://github.com/aethersdr/AetherSDR/blob/main/docs/architecture/aetherd-icom-civ-backend-design.md)
