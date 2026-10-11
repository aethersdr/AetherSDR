---
title: "Stream Deck+ and HID Controller Actions"
description: "The actions and defaults for the Stream Deck+, HID encoders and the TMate 2, generated from the source."
sidebar_position: 6
custom_edit_url: null
generated_by: "tools/docs/gen_reference.py"
generated_from: ["src/gui/RadioSetupDialog.cpp"]
---

:::info[Generated page]

This page is generated from `src/gui/RadioSetupDialog.cpp` by `tools/docs/gen_reference.py`. Do not edit it by hand: change the source, then run `python3 tools/docs/gen_reference.py`.

:::

Set these up in **Settings → Radio Setup... → Serial & Controllers**. The encoder tables also apply to single-encoder HID devices such as the RC-28, PowerMate and ShuttleXpress, which use Encoder 1 only.

These controls exist only in builds with `HAVE_SERIALPORT` and `HAVE_HIDAPI`.

## Stream Deck+ and HID encoders

### LCD key actions

| Action | ID |
|---|---|
| None | `None` |
| Toggle TX (MOX) | `ToggleMox` |
| Toggle Tune | `ToggleTune` |
| Toggle RIT on/off | `ToggleRit` |
| Toggle XIT on/off | `ToggleXit` |
| Clear RIT offset | `ClearRit` |
| Clear XIT offset | `ClearXit` |
| Step Size Up | `StepUp` |
| Step Size Down | `StepDown` |
| Toggle Mute | `ToggleMute` |
| Toggle Slice Lock | `ToggleLock` |
| Toggle APF | `ToggleApf` |
| Cycle AGC Mode | `ToggleAgc` |
| Toggle Band Zoom | `BandZoom` |
| Toggle Segment Zoom | `SegmentZoom` |
| Next Slice | `NextSlice` |
| Previous Slice | `PrevSlice` |
| Volume Up (+5) | `VolumeUp` |
| Volume Down (-5) | `VolumeDown` |
| Toggle Split | `SplitActiveSlice` |
| Monitor TX Frequency | `SplitMonitorTx` |

### Encoder turn actions

| Encoder | Default |
|---|---|
| Encoder 1 | Tune Slice |
| Encoder 2 | RIT (Receive Incremental Tuning) |
| Encoder 3 | XIT (Transmit Incremental Tuning) |
| Encoder 4 | Master Volume |

| Action | ID |
|---|---|
| Tune Slice | `WheelFrequency` |
| RIT (Receive Incremental Tuning) | `WheelRit` |
| XIT (Transmit Incremental Tuning) | `WheelXit` |
| Master Volume | `WheelVolume` |
| Slice Audio Volume | `WheelSliceAudio` |
| Headphone Volume | `WheelHeadphoneVolume` |
| AGC Threshold | `WheelAgcT` |
| APF Level | `WheelApf` |
| CW Speed | `WheelCwSpeed` |
| RF Power | `WheelPower` |
| None | `None` |

### Encoder push actions

| Encoder | Default |
|---|---|
| Encoder 1 | Cycle Tuning Step |
| Encoder 2 | Toggle RIT on/off |
| Encoder 3 | Toggle XIT on/off |
| Encoder 4 | None |

| Action | ID |
|---|---|
| Cycle Tuning Step | `StepCycle` |
| Toggle RIT on/off | `ToggleRit` |
| Toggle XIT on/off | `ToggleXit` |
| Toggle TX (MOX) | `ToggleMox` |
| Toggle Mute | `ToggleMute` |
| Lock Slice | `ToggleLock` |
| None | `None` |

## TMate 2

### Function key actions

| Key | Default |
|---|---|
| F 1 | Toggle TX (MOX) |
| F 2 | Cycle AGC Mode |
| F 3 | Toggle Band Zoom |
| F 4 | Toggle APF |
| F 5 | Toggle Mute |
| F 6 | Toggle RIT on/off |

| Action | ID |
|---|---|
| None | `None` |
| Toggle TX (MOX) | `ToggleMox` |
| Toggle Tune | `ToggleTune` |
| Toggle RIT on/off | `ToggleRit` |
| Toggle XIT on/off | `ToggleXit` |
| Clear RIT offset | `ClearRit` |
| Clear XIT offset | `ClearXit` |
| Step Size Up | `StepUp` |
| Step Size Down | `StepDown` |
| Toggle Mute | `ToggleMute` |
| Toggle Slice Lock | `ToggleLock` |
| Toggle APF | `ToggleApf` |
| Cycle AGC Mode | `ToggleAgc` |
| Toggle Band Zoom | `BandZoom` |
| Toggle Segment Zoom | `SegmentZoom` |
| Next Slice | `NextSlice` |
| Previous Slice | `PrevSlice` |
| Volume Up (+5) | `VolumeUp` |
| Volume Down (-5) | `VolumeDown` |
| Toggle Split | `SplitActiveSlice` |
| Monitor TX Frequency | `SplitMonitorTx` |

### Encoder turn actions

| Encoder | Default |
|---|---|
| Encoder 1 | Tune Slice |
| Encoder 2 | RIT (Receive Incremental Tuning) |
| Encoder 3 | XIT (Transmit Incremental Tuning) |

| Action | ID |
|---|---|
| Tune Slice | `WheelFrequency` |
| RIT (Receive Incremental Tuning) | `WheelRit` |
| XIT (Transmit Incremental Tuning) | `WheelXit` |
| Master Volume | `WheelVolume` |
| Slice Audio Volume | `WheelSliceAudio` |
| Headphone Volume | `WheelHeadphoneVolume` |
| AGC Threshold | `WheelAgcT` |
| APF Level | `WheelApf` |
| CW Speed | `WheelCwSpeed` |
| RF Power | `WheelPower` |
| None | `None` |

### Encoder push actions

| Encoder | Default |
|---|---|
| Encoder 1 | Cycle Tuning Step |
| Encoder 2 | Toggle RIT on/off |
| Encoder 3 | Toggle XIT on/off |

| Action | ID |
|---|---|
| Cycle Tuning Step | `StepCycle` |
| Toggle RIT on/off | `ToggleRit` |
| Toggle XIT on/off | `ToggleXit` |
| Toggle TX (MOX) | `ToggleMox` |
| Toggle Mute | `ToggleMute` |
| Lock Slice | `ToggleLock` |
| None | `None` |
