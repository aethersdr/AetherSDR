---
title: "FlexControl Actions"
description: "The actions a FlexControl knob's buttons can run, and their defaults, generated from the source."
sidebar_position: 4
custom_edit_url: null
generated_by: "tools/docs/gen_reference.py"
generated_from: ["src/gui/FlexControlDialog.cpp"]
---

:::info[Generated page]

This page is generated from `src/gui/FlexControlDialog.cpp` by `tools/docs/gen_reference.py`. Do not edit it by hand: change the source, then run `python3 tools/docs/gen_reference.py`.

:::

The FlexControl knob's three Aux buttons can each run one action on a single tap and another on a double tap. Set them in **Settings → FlexControl Knob & Buttons...**.

## Default button actions

| Button | Single tap | Double tap |
|---|---|---|
| Aux1 | Step Up | Step Down |
| Aux2 | MOX | Toggle Tune |
| Aux3 | Toggle Mute | Toggle Lock |

## Actions

| Action | ID |
|---|---|
| Tune Slice | `WheelFrequency` |
| Band Zoom | `BandZoom` |
| Segment Zoom | `SegmentZoom` |
| RIT (Receive Incremental Tuning) | `WheelRit` |
| XIT (Transmit Incremental Tuning) | `WheelXit` |
| Master Volume | `WheelVolume` |
| Slice Audio Volume | `WheelSliceAudio` |
| Headphone Volume | `WheelHeadphoneVolume` |
| AGCT (Automatic Gain Control Threshold) | `WheelAgcT` |
| APF (Audio Peaking Filter) | `WheelApf` |
| Clear RIT | `ClearRit` |
| Clear XIT | `ClearXit` |
| Toggle APF | `ToggleApf` |
| Change Active Slice | `NextSlice` |
| Split Active Slice | `SplitActiveSlice` |
| Monitor TX Frequency | `SplitMonitorTx` |
| MOX | `ToggleMox` |
| RF Power | `WheelPower` |
| CW Speed | `WheelCwSpeed` |
| CWX Macro 1 | `CwxF1` |
| CWX Macro 2 | `CwxF2` |
| CWX Macro 3 | `CwxF3` |
| CWX Macro 4 | `CwxF4` |
| CWX Macro 5 | `CwxF5` |
| CWX Macro 6 | `CwxF6` |
| CWX Macro 7 | `CwxF7` |
| CWX Macro 8 | `CwxF8` |
| CWX Macro 9 | `CwxF9` |
| CWX Macro 10 | `CwxF10` |
| CWX Macro 11 | `CwxF11` |
| CWX Macro 12 | `CwxF12` |
| Step Up | `StepUp` |
| Step Down | `StepDown` |
| Toggle Tune | `ToggleTune` |
| Toggle Mute | `ToggleMute` |
| Toggle Lock | `ToggleLock` |
| Previous Slice | `PrevSlice` |
| Toggle AGC | `ToggleAgc` |
| Slice AF Up | `VolumeUp` |
| Slice AF Down | `VolumeDown` |
| None | `None` |
