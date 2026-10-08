---
title: "TNF (Tracking Notch Filters)"
slug: "/tnf-tracking-notch-filters"
description: "Tracking Notch Filters remove persistent interfering carriers from the audio passband."
status: "Supported"
applies_to: ["FlexRadio", "Hermes-Lite 2 (experimental)"]
---

:::info[Status]

**Status:** Supported · **Applies to:** FlexRadio, Hermes-Lite 2 (experimental)

:::

Tracking Notch Filters remove persistent interfering carriers from the audio
passband. A notch stays on its frequency as you tune, so it keeps removing the
same interferer.

## Using TNFs

### Creating a TNF

- Right-click on the spectrum or waterfall → **Add TNF at XX.XXX MHz**
- Or click **+TNF** in the panadapter's overlay menu (creates a TNF at the
  current frequency)
- The TNF appears as a shaded region on the spectrum and waterfall

The **Add TNF** entry is absent on a radio with no notch engine, and once the
radio has as many notches as it supports.

### Managing TNFs

Hover a TNF marker for a tooltip with its depth and whether it is permanent.
Right-click it for a menu headed by its frequency and width.

#### Move and resize
- Drag the marker horizontally to move it
- Drag vertically to change its width

#### Width and Depth
- Right-click → **Width**: 50, 100, 200 or 500 Hz (only the widths the radio
  can actually produce are listed)
- Right-click → **Depth**: Normal, Deep or Very Deep (FlexRadio)

#### Extended TNF
- Right-click → **Extended TNF** extends every notch's band down through the
  waterfall, the way Extended Passband does for the slice filter. It is a
  display preference that applies to all notches.

#### Make Permanent
- Right-click → **Make Permanent** — the radio keeps the TNF across power
  cycles. **Make Temporary** reverses it.
- Permanent TNFs display in **green**; temporary TNFs in **yellow**

#### Remove
- Right-click → **Remove TNF**

### Hermes-Lite 2

The Hermes-Lite 2 supports the same notch gestures: right-click to add, drag
to move and resize, right-click to remove. Its notches run on the computer
rather than in the radio and keep tracking as you tune. Because the HL2 has no
place to store them and its notches are full nulls, the **Depth** and **Make
Permanent** entries are not offered. A notch (or a very narrow CW filter)
lengthens the receive filter slightly, which adds a little audio delay. See
[Hermes-Lite 2](./hermes-lite-2.md).

## Reference

### Permanent vs temporary

| Type | Colour | Survives reboot | Default |
|------|-------|-----------------|---------|
| Temporary | Yellow | No | Yes |
| Permanent | Green | Yes | No |

TNF removal and permanent-state changes are applied straight away in the
display; the radio does not send a status confirmation for these commands.

## Troubleshooting

### Add TNF is missing from the menu

The radio has no notch engine, or it already holds as many notches as it
supports.

1. Remove a notch you no longer need (right-click it → **Remove TNF**), then
   try again.
2. If the entry never appears, your radio doesn't offer notches. TNFs work on
   FlexRadio and Hermes-Lite 2.

### My notches disappeared after the radio restarted

They were temporary (yellow). Temporary notches don't survive a power cycle.

1. Add the notch again.
2. Right-click it and choose **Make Permanent**. It turns green.

## See also

- [Panadapter Controls](./panadapter-controls.md)
- [DSP Noise Mitigation](./dsp-noise-mitigation.md)
- [RX Controls](./rx-controls.md)
- [Hermes-Lite 2](./hermes-lite-2.md)
