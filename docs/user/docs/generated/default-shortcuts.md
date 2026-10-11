---
title: "Default Keyboard Shortcuts"
description: "Every assignable keyboard action in AetherSDR, with its ID and default key, generated from the source."
sidebar_position: 2
custom_edit_url: null
generated_by: "tools/docs/gen_reference.py"
generated_from: ["src/gui/MainWindow_Shortcuts.cpp", "src/core/ShortcutManager.cpp", "src/gui/MainWindowHelpers.h", "src/core/DigitalVoiceFeature.h", "src/core/DigitalVoiceModeRegistry.cpp"]
---

:::info[Generated page]

This page is generated from `src/gui/MainWindow_Shortcuts.cpp`, `src/core/ShortcutManager.cpp`, `src/gui/MainWindowHelpers.h`, `src/core/DigitalVoiceFeature.h`, `src/core/DigitalVoiceModeRegistry.cpp` by `tools/docs/gen_reference.py`. Do not edit it by hand: change the source, then run `python3 tools/docs/gen_reference.py`.

:::

These are the actions you can bind in **Settings → Configure Shortcuts...**, with the key each one has before you change it. An action with no default key does nothing from the keyboard until you bind one.

- The **ID** is the stable name used in a shortcuts backup file and by the automation bridge's `shortcut` verb.
- On macOS, Qt reads **Ctrl** as the Command (⌘) key and **Meta** as the Control key.
- Menu accelerators and fixed application shortcuts are not in this list; they are not assignable.

## Frequency

| Action | ID | Default key | Notes |
|---|---|---|---|
| Tune Up (1 step) | `tune_up_1` | `Right` | Repeats while held |
| Tune Down (1 step) | `tune_down_1` | `Left` | Repeats while held |
| Tune Up (10 steps) | `tune_up_10` | `Shift+Right` | Repeats while held |
| Tune Down (10 steps) | `tune_down_10` | `Shift+Left` | Repeats while held |
| Tune Up 1 MHz | `tune_up_1mhz` |   |   |
| Tune Down 1 MHz | `tune_down_1mhz` |   |   |
| Go to Frequency | `go_to_freq` | `G` |   |

## Band

| Action | ID | Default key | Notes |
|---|---|---|---|
| 160m | `band_160m` |   |   |
| 80m | `band_80m` |   |   |
| 60m | `band_60m` |   |   |
| 40m | `band_40m` |   |   |
| 30m | `band_30m` |   |   |
| 20m | `band_20m` |   |   |
| 17m | `band_17m` |   |   |
| 15m | `band_15m` |   |   |
| 12m | `band_12m` |   |   |
| 10m | `band_10m` |   |   |
| 6m | `band_6m` |   |   |
| 2m | `band_2m` |   |   |

## Mode

| Action | ID | Default key | Notes |
|---|---|---|---|
| USB | `mode_usb` |   |   |
| LSB | `mode_lsb` |   |   |
| CW | `mode_cw` |   |   |
| CWL | `mode_cwl` |   |   |
| AM | `mode_am` |   |   |
| SAM | `mode_sam` |   |   |
| FM | `mode_fm` |   |   |
| NFM | `mode_nfm` |   |   |
| DFM | `mode_dfm` |   |   |
| DSTR | `mode_dstr` |   | Only in builds with `AETHER_ENABLE_DIGITAL_VOICE_HELPER` |
| DIGU | `mode_digu` |   |   |
| DIGL | `mode_digl` |   |   |
| RTTY | `mode_rtty` |   |   |
| FDVU | `mode_fdvu` |   |   |
| FDVL | `mode_fdvl` |   |   |

## TX

| Action | ID | Default key | Notes |
|---|---|---|---|
| MOX Toggle | `mox_toggle` | `T` | Keys the transmitter |
| PTT (Hold) | `ptt_hold` | `Space` | Keys the transmitter |
| ATU Start | `atu_start` |   | Keys the transmitter |
| TUNE Toggle | `tune_toggle` |   | Keys the transmitter |
| Two-Tone Tune | `two_tone_tune` |   | Keys the transmitter |
| VOX Toggle | `vox_toggle` |   |   |
| Speech Processor Toggle | `speech_proc_toggle` |   |   |
| DAX TX Toggle | `dax_toggle` |   |   |
| TX Monitor Toggle | `tx_monitor_toggle` |   |   |

## CW

| Action | ID | Default key | Notes |
|---|---|---|---|
| CW Speed Up (+5 WPM) | `cw_speed_up` |   |   |
| CW Speed Down (-5 WPM) | `cw_speed_down` |   |   |
| CW Sidetone Toggle | `cw_sidetone_toggle` |   |   |
| CW Iambic Toggle | `cw_iambic_toggle` |   |   |
| CW Iambic Mode Toggle (A/B) | `cw_iambic_mode_toggle` |   |   |
| CW Swap Paddles Toggle | `cw_swap_paddles_toggle` |   |   |
| CWL Frequency Offset Toggle | `cwl_toggle` |   |   |
| CW Break-In (QSK) Toggle | `cw_breakin_toggle` |   |   |
| Trigger straight key | `cwkey` |   | Keys the transmitter |
| Trigger CW Left Paddle | `cwdit` |   | Keys the transmitter |
| Trigger CW Right Paddle | `cwdah` |   | Keys the transmitter |

## Audio

| Action | ID | Default key | Notes |
|---|---|---|---|
| AF Gain Up | `af_gain_up` | `Up` |   |
| AF Gain Down | `af_gain_down` | `Down` |   |
| Mute Toggle | `mute_toggle` | `M` |   |
| Mute All Slices | `mute_all_slices_toggle` |   |   |
| Master Mute Toggle | `master_mute_toggle` |   |   |
| Master Volume Up | `master_volume_up` |   | Repeats while held |
| Master Volume Down | `master_volume_down` |   | Repeats while held |
| Squelch Toggle | `squelch_toggle` |   |   |

## Slice

| Action | ID | Default key | Notes |
|---|---|---|---|
| Next Slice | `next_slice` |   |   |
| Prev Slice | `prev_slice` |   |   |
| Split Toggle | `split_toggle` |   |   |
| Monitor TX (Hold) | `split_monitor_tx` |   |   |
| Split Up 1 kHz | `split_up_1` |   |   |
| Split Up 5 kHz | `split_up_5` |   |   |
| Split Up 10 kHz | `split_up_10` |   |   |
| Cycle TX Slice | `cycle_tx_slice` |   |   |

## Filter

| Action | ID | Default key | Notes |
|---|---|---|---|
| Filter Widen | `filter_widen` |   |   |
| Filter Narrow | `filter_narrow` |   |   |

## Tuning

| Action | ID | Default key | Notes |
|---|---|---|---|
| Step Size Up | `step_up` | `]` |   |
| Step Size Down | `step_down` | `[` |   |
| Tune Lock Toggle | `lock_toggle` | `L` |   |
| Center Lock Active Slice | `center_lock_toggle` |   |   |

## DSP

| Action | ID | Default key | Notes |
|---|---|---|---|
| NB Toggle | `nb_toggle` |   |   |
| NR2 Toggle | `nr2_toggle` |   |   |
| RN2 (RNNoise) Toggle | `rn2_toggle` |   |   |
| NR4 Toggle | `nr4_toggle` |   |   |
| DFNR Toggle | `dfnr_toggle` |   |   |
| TNF Global Toggle | `tnf_toggle` |   |   |
| NR Cycle (Off/NR/NR2/NR4/DFNR) | `nr_cycle` |   |   |
| ANF Toggle | `anf_toggle` |   |   |

## AGC

| Action | ID | Default key | Notes |
|---|---|---|---|
| AGC Mode Cycle | `agc_cycle` |   |   |
| RF Gain Up | `rf_gain_up` |   | Repeats while held |
| RF Gain Down | `rf_gain_down` |   | Repeats while held |
| AGC-T Up | `agct_up` |   | Repeats while held |
| AGC-T Down | `agct_down` |   | Repeats while held |

## EQ

| Action | ID | Default key | Notes |
|---|---|---|---|
| TX EQ Toggle | `tx_eq_toggle` |   |   |
| RX EQ Toggle | `rx_eq_toggle` |   |   |

## Display

| Action | ID | Default key | Notes |
|---|---|---|---|
| Band Zoom | `band_zoom` |   |   |
| Segment Zoom | `segment_zoom` |   |   |
| Panadapter Zoom In | `pan_zoom_in` | `=` |   |
| Panadapter Zoom Out | `pan_zoom_out` | `-` |   |
| Open Memories Dialog | `open_memories` | `/` |   |
| Minimize Active Window | `window_minimize` | macOS: `Ctrl+M`; other: none | Window management: works with keyboard shortcuts switched off |
| Toggle Active Window Full Screen | `window_fullscreen` | macOS: `Ctrl+Meta+F`; other: `F11` | Window management: works with keyboard shortcuts switched off |
| Minimal Mode Toggle | `minimal_mode` |   |   |

## RIT/XIT

| Action | ID | Default key | Notes |
|---|---|---|---|
| RIT Toggle | `rit_toggle` |   |   |
| XIT Toggle | `xit_toggle` |   |   |
