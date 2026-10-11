---
title: "Ulanzi Dial Actions"
description: "The Ulanzi Dial's wheel actions and button defaults, generated from the source."
sidebar_position: 5
custom_edit_url: null
generated_by: "tools/docs/gen_reference.py"
generated_from: ["src/gui/UlanziDialMapperDialog.cpp", "src/core/UlanziDialMappings.cpp"]
---

:::info[Generated page]

This page is generated from `src/gui/UlanziDialMapperDialog.cpp`, `src/core/UlanziDialMappings.cpp` by `tools/docs/gen_reference.py`. Do not edit it by hand: change the source, then run `python3 tools/docs/gen_reference.py`.

:::

Set the Ulanzi Dial up in **Settings → Ulanzi Dial Mapping...**.

- The **wheel** runs one of the actions below.
- Each **button** can run any action from [Default Keyboard Shortcuts](./default-shortcuts.md), or any Toggle, Trigger or Gate control from [MIDI Controller Actions](./midi-actions.md).

## Wheel actions

| Category | Action | ID | Default |
|---|---|---|---|
| Frequency | Tune Slice | `WheelFrequency` | Default |
| Filter | Filter Bandwidth | `FilterWidth` |   |
| Audio | Slice Audio Volume | `WheelSliceAudio` |   |
| Audio | Master Volume | `WheelVolume` |   |
| Audio | Headphone Volume | `WheelHeadphoneVolume` |   |
| Display | Panadapter Zoom | `PanadapterZoom` |   |
| Display | Band Zoom | `BandZoom` |   |
| Display | Segment Zoom | `SegmentZoom` |   |
| RIT/XIT | RIT (Receive Incremental Tuning) | `WheelRit` |   |
| RIT/XIT | XIT (Transmit Incremental Tuning) | `WheelXit` |   |
| AGC | AGC-T (Threshold) | `WheelAgcT` |   |
| AGC | RF Gain | `WheelRfGain` |   |
| DSP | APF (Audio Peaking Filter) | `WheelApf` |   |
| CW | CW Speed | `WheelCwSpeed` |   |
| TX | RF Power | `WheelPower` |   |

## Button defaults

| Button | Button ID | Default action | Stored as |
|---|---|---|---|
| Top Left | `top_left` | Unassigned |   |
| Top Middle | `top_middle` | \[RIT/XIT\] RIT Toggle | `shortcut:rit_toggle` |
| Top Right | `top_right` | Unassigned |   |
| Left Top | `side_lt` | Unassigned |   |
| Left Bottom | `side_lb` | Unassigned |   |
| Right Top | `side_rt` | \[Slice\] Next Slice | `shortcut:next_slice` |
| Right Bot | `side_rb` | Unassigned |   |
| Dial Press | `dial_press` | \[Audio\] Mute Toggle | `shortcut:mute_toggle` |
