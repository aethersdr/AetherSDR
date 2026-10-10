---
title: "Multi-Slice Operation"
slug: "/multi-slice-operation"
description: "AetherSDR supports multiple simultaneous receivers (slices), on one panadapter or spread across several."
---

AetherSDR supports multiple simultaneous receivers (slices), on one panadapter
or spread across several.

## Requirements

The number of slices and panadapters comes from the connected radio, not from
a built-in table, so AetherSDR offers exactly what your radio can do. Other
radio families have their own limits: the Hermes-Lite 2 has up to four
receivers (A–D), the RTL-SDR and ANAN-G2 one, and the IC-9700 one panadapter.
See [Supported Radios](./supported-radios.md).

## Using multiple slices

### Adding a slice

- Click **+RX** in the panadapter's overlay menu
- Or right-click an empty part of the panadapter → **Add Slice at X MHz**
- Clicking an empty panadapter also creates a new slice there instead of
  moving the active one
- A new slice appears with its own coloured VFO marker (see [Slice Colors](./slice-colors.md))

To add another panadapter, use **Tools → Add Panadapter...**

### Switching the active slice

- **Click the slice badge** on the spectrum to switch focus
- **Click the VFO marker line** to switch
- The VFO widget and the RX applet follow the active slice

### TX assignment

- TX stays on its assigned slice until you move it
- **Red TX badge** — active TX slice
- **Grey TX badge** (in the VFO widget) — click to move TX to this slice
- Switching the active slice does **not** move TX
- The **Cycle TX Slice** keyboard and controller action steps TX through your
  slices

### Split

Click **SPLIT** on a slice to listen there and transmit on a second slice
beside it. See [Split Operation](./split-operation.md) for SWAP, Split Up, Monitor TX and
remembered audio.

### Slice Link

Two slices, even on different panadapters, can be tuned together. Right-click
a panadapter → **Link Slice**: the submenu lists **Current Links** (choose one
to unlink) and **Available Links** (choose a pair to link).

- Tuning either slice retunes the other, including KiwiSDR-sourced slices.
- Linked slices show a ⇄ marker.
- The link is suspended while either slice is tune-locked; the panadapter says
  "Slice Link A to B suspended".

### Center Lock

Right-click a panadapter → **Center Lock Slice A** (or the **Center Lock**
submenu when there are several slices) keeps that panadapter centred on the
chosen slice as it tunes. Each panadapter locks independently, and the lock
survives band changes. See [Panadapter Controls](./panadapter-controls.md).

### Closing a slice

- Click the **✕** button floating beside the VFO widget
- Or right-click the slice on the spectrum → **Close Slice X**
- You cannot close the last remaining slice

### Off-screen slices

When a slice is off the visible span, an arrow on the spectrum edge shows its
letter, direction and frequency. **Single-click** the arrow to bring the slice
back into view; right-click it for the slice menu.

### Hermes-Lite 2

On the Hermes-Lite 2 the sample rate is shared by the whole board, so every
panadapter shows the same span. Only the pane holding the TX slice (or the
first docked pane) keeps live span buttons; the others are dimmed with the
reason. See [Hermes-Lite 2](./hermes-lite-2.md).

## Known issues

- Adding a panadapter or a slice can make a loud pop in the speakers for about a second ([#4973](https://github.com/aethersdr/AetherSDR/issues/4973)).
- Clicking an off-screen slice's arrow brings the slice to the edge of the panadapter rather than the centre ([#2371](https://github.com/aethersdr/AetherSDR/issues/2371)).
- On the Hermes-Lite 2, receivers B, C and D don't come back after AetherSDR restarts; only one receiver appears ([#5777](https://github.com/aethersdr/AetherSDR/issues/5777)).

## Troubleshooting

### A slice won't close

It is the last slice; AetherSDR always keeps one. The status bar says "Cannot close the last slice".

1. Add another slice first if you want to replace this one, then close the old one.

### Transmit stayed on the old slice

Switching the active slice does not move TX.

1. Click the grey **TX** badge on the slice you want to transmit on.

### Linked slices stopped tuning together

One of the slices is tune-locked, so the link is suspended ("Slice Link A to B suspended").

1. Click **🔒** beside that slice's VFO flag to unlock it. The link resumes.

### A click added a new slice instead of moving the one I have

Clicking an empty panadapter (one with no slice on it) creates a slice there.

1. To retune an existing slice, use the panadapter that slice is on, or close the new slice with **✕**.

## See also

- [Split Operation](./split-operation.md)
- [VFO Widget](./vfo-widget.md)
- [Slice Colors](./slice-colors.md)
- [Panadapter Controls](./panadapter-controls.md)
- [Diversity and ESC](./diversity-and-esc.md)
- [Supported Radios](./supported-radios.md)
