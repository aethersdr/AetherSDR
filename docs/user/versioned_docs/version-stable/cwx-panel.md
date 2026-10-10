---
title: "CWX Panel"
slug: "/cwx-panel"
description: "The CWX (CW Transmit) panel sends Morse code typed on your keyboard or stored in macros, keyed by the radio's own CW text keyer."
status: "Supported"
applies_to: ["FlexRadio", "IC-7300MK2", "IC-705 (early)"]
---

:::info[Status]

**Status:** Supported · **Applies to:** FlexRadio, IC-7300MK2, IC-705 (early)

:::

The CWX (CW Transmit) panel sends Morse code typed on your keyboard or stored in macros, keyed by the radio's own CW text keyer. On a FlexRadio it uses the radio's CWX keyer; on a supported [Networked Icom](./networked-icom.md) radio the same panel drives the radio's CI-V text keyer and is labelled **CWK**.

## Requirements

- **Mode:** CWX keys the **transmit slice**, so it is available when the TX slice is in **CW** or **CWL**. The indicator is greyed out otherwise, and switching the TX slice out of CW closes the panel.
- **Radio support:** the indicator is **hidden** on radios without a radio-side CW text keyer (for example the [Hermes-Lite 2](./hermes-lite-2.md)), and the panel closes with it.
- **On Icom (CWK):** supported on the IC-705 and IC-7300MK2. The speed range follows the radio, and the **Live** and **Setup** views are hidden because the CI-V keyer has no live keying or stored macros.

## Using the CWX panel

### Opening CWX

Click the **CWX** indicator in the status bar at the bottom of the screen (or use **Tools → CW Keyer**). The panel opens beside the panadapter.

The CWX panel and the [DVK Panel](./dvk-panel.md) share the same space — opening one closes the other.

### Sending text

The panel has three views, chosen with the buttons at its top: **Send**, **Live** and **Setup** (see [Views](#views)). In **Send**, type text and send it; sent text appears as history bubbles above the entry line. In **Live**, characters are keyed as you type them.

**Speed** sets the keyer speed in WPM (5–100 on FlexRadio).

Right-click a sent bubble for **Resend** (sent again exactly as typed, per-word speed changes included) or **Clear History**.

When the CWX queue empties, AetherSDR releases the transmitter straight away, so the radio returns to receive right after the last character.

### Macros and shortcuts

| Key | Action |
|---|---|
| **F1–F12** | Send the matching macro. Works app-wide while the TX slice is in CW/CWL, even with the panel closed. |
| **Escape** | Abort the CW being sent |

F1–F12 follow the transmit slice's mode: in CW they fire CWX macros, in voice modes they play DVK recordings (see [DVK Panel](./dvk-panel.md)), and only one of the two responds. CWX macros 1–12 can also be assigned to FlexControl, MIDI and other controller buttons.

### Keyboard and MIDI keying (local iambic keyer)

Beyond text entry, AetherSDR has a local iambic / straight-key keyer that respects the radio's break-in setting. Three actions can be bound to keys or MIDI controls:

| Action | Default key |
|---|---|
| **Trigger straight key** | unbound |
| **Trigger CW Left Paddle** (dits) | unbound |
| **Trigger CW Right Paddle** (dahs) | unbound |

Bind keys under **Settings → Configure Shortcuts...** and turn on **Settings → Keyboard Shortcuts** (off by default), or map a MIDI control in [MIDI Controller Mapping](./midi-controller-mapping.md). Further CW actions include speed up/down, sidetone, iambic on/off, iambic mode A/B, swap paddles and break-in.

- With **break-in** on, a paddle or straight-key press keys the radio directly. With break-in off, MOX must be on for the CW to go out.
- Element timing is scheduled against an exact clock with a sample-accurate sidetone, and iambic Mode B behaves correctly when both paddles are released together.
- The break-in delay holds when you change speed.
- CW keying and **TUNE** lock each other out: you cannot key CW while TUNE is on, or start TUNE while a key is down.

The sidetone plays on your selected audio output and follows the radio's sidetone pitch and level.

To watch your own sending, turn on the **TX** decode of the [CW Decoder](./cw-decoder.md).

## Reference

### Views

| View | What it does |
|---|---|
| **Send** | Type text and send it. Sent text appears as history bubbles above the entry line. |
| **Live** | Characters are keyed as you type them. |
| **Setup** | Delay, QSK, speed step and the twelve F1–F12 macros. |

### Setup settings

| Setting | What it does |
|---|---|
| **Delay** | Break-in delay, 0–2000 ms |
| **QSK** | Full break-in on or off |
| **Step** | How many WPM each `+` or `-` speed prefix changes the speed (1–20) |
| **F1–F12** | Twelve macro texts |

### Special characters

The legend under the macros lists the special characters:

- **Prosigns:** `=` (BT), `+` (AR), `(` (KN), `&` (BK), `$` (SK)
- **Speed changes:** `+word` sends that word faster, `-word` slower; `++` / `--` change by twice the step. A `+` or `-` only counts as a speed prefix at the start of a word — a standalone `+` is still AR.

## Known issues

- Receive audio can go silent after a CW transmission and only return after a **TUNE** cycle ([#5312](https://github.com/aethersdr/AetherSDR/issues/5312)).

## Troubleshooting

### Receive audio goes silent for about a minute after sending CWX

On FLEX firmware v4.2.18, a CWX break-in delay of 0 makes the radio mute all receive audio for roughly 70 seconds after CWX text is sent. The panadapter keeps running, but audio, the CW decoder and the S-meter go dead. This is a firmware defect, fixed in v4.2.20 ([#5945](https://github.com/aethersdr/AetherSDR/issues/5945)).

1. Update the radio to firmware v4.2.20 or later. See [Firmware Update](./firmware-update.md).

### The CWX indicator is greyed out

The transmit slice is not in a CW mode.

1. Switch the TX slice to **CW** or **CWL**.

### The CWX indicator is missing

The radio has no radio-side CW text keyer, as on the [Hermes-Lite 2](./hermes-lite-2.md). Use the local keyer instead (see [Keyboard and MIDI keying](#keyboard-and-midi-keying-local-iambic-keyer)).

### A paddle or key press sends nothing

Break-in is off, so the keyer waits for MOX.

1. Turn on **break-in**, or turn on MOX before keying.
2. Check that **TUNE** is off; CW keying is locked out while it is on.

## See also

- [CW Decoder](./cw-decoder.md)
- [DVK Panel](./dvk-panel.md)
- [MIDI Controller Mapping](./midi-controller-mapping.md)
- [Keyboard Shortcuts](./keyboard-shortcuts.md)
- [FlexControl Tuning Knob](./flexcontrol-tuning-knob.md)
