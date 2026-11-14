---
title: "Default Keyboard Shortcuts"
description: "Every assignable keyboard action in AetherSDR, with its ID and default key, generated from the source."
sidebar_position: 2
custom_edit_url: null
generated_by: "tools/docs/gen_reference.py"
generated_from: ["src/gui/MainWindow_Shortcuts.cpp", "src/gui/MainWindow_ControlActions.cpp", "src/core/ShortcutManager.cpp", "src/gui/MainWindowHelpers.h", "src/core/DigitalVoiceFeature.h", "src/core/DigitalVoiceModeRegistry.cpp"]
---

:::info[Generated page]

This page is generated from `src/gui/MainWindow_Shortcuts.cpp`, `src/gui/MainWindow_ControlActions.cpp`, `src/core/ShortcutManager.cpp`, `src/gui/MainWindowHelpers.h`, `src/core/DigitalVoiceFeature.h`, `src/core/DigitalVoiceModeRegistry.cpp` by `tools/docs/gen_reference.py`. Do not edit it by hand: change the source, then run `python3 tools/docs/gen_reference.py`.

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
| Tune Up (100 steps) | `tune_up_100` |   | Repeats while held |
| Tune Down (100 steps) | `tune_down_100` |   | Repeats while held |

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
| Band Up | `band_up` |   |   |
| Band Down | `band_down` |   |   |
| Band Stack Up | `band_stack_up` |   |   |
| Band Stack Down | `band_stack_down` |   |   |

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
| Mode Up | `mode_up` |   |   |
| Mode Down | `mode_down` |   |   |

## TX

| Action | ID | Default key | Notes |
|---|---|---|---|
| MOX Toggle | `mox_toggle` | `T` | Keys the transmitter |
| PTT (Hold) | `ptt_hold` | `Space` | Keys the transmitter |
| ATU Start | `atu_start` |   | Keys the transmitter |
| TUNE Toggle | `tune_toggle` |   | Keys the transmitter |
| Two-Tone Tune | `two_tone_tune` |   | Keys the transmitter |
| VOX Toggle | `vox_toggle` |   | Keys the transmitter |
| Speech Processor Toggle | `speech_proc_toggle` |   |   |
| DAX TX Toggle | `dax_toggle` |   |   |
| TX Monitor Toggle | `tx_monitor_toggle` |   |   |
| RF Power Up | `rf_power_up` |   | Repeats while held |
| RF Power Down | `rf_power_down` |   | Repeats while held |
| Mic Level Up | `mic_level_up` |   | Repeats while held |
| Mic Level Down | `mic_level_down` |   | Repeats while held |
| TX Antenna 1 | `tx_antenna_1` |   |   |
| TX Antenna 2 | `tx_antenna_2` |   |   |
| TX Antenna 3 | `tx_antenna_3` |   |   |
| TX Antenna 4 | `tx_antenna_4` |   |   |
| Next TX Antenna | `tx_antenna_next` |   |   |
| ATU Memories | `atu_memories_toggle` |   |   |
| ACC TX Output | `tx_output_acc` |   |   |
| TX1 Output | `tx_output_1` |   |   |
| TX2 Output | `tx_output_2` |   |   |
| TX3 Output | `tx_output_3` |   |   |
| ATU Bypass | `atu_bypass` |   | Keys the transmitter |

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
| CW Keyer: Iambic A | `cw_keyer_iambic_a` |   |   |
| CW Keyer: Iambic B | `cw_keyer_iambic_b` |   |   |
| CW Keyer: Straight Key | `cw_keyer_straight` |   |   |
| Stop CWX / Macros | `cwx_cancel` |   |   |
| CW Decoder | `cw_decode_toggle` |   |   |
| Show/Hide CWX Panel | `cwx_panel_toggle` |   |   |
| Macro F1 | `cwx_macro_1` |   | Keys the transmitter |
| Macro F2 | `cwx_macro_2` |   | Keys the transmitter |
| Macro F3 | `cwx_macro_3` |   | Keys the transmitter |
| Macro F4 | `cwx_macro_4` |   | Keys the transmitter |
| Macro F5 | `cwx_macro_5` |   | Keys the transmitter |
| Macro F6 | `cwx_macro_6` |   | Keys the transmitter |
| Macro F7 | `cwx_macro_7` |   | Keys the transmitter |
| Macro F8 | `cwx_macro_8` |   | Keys the transmitter |
| Macro F9 | `cwx_macro_9` |   | Keys the transmitter |
| Macro F10 | `cwx_macro_10` |   | Keys the transmitter |
| Macro F11 | `cwx_macro_11` |   | Keys the transmitter |
| Macro F12 | `cwx_macro_12` |   | Keys the transmitter |

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
| Binaural RX | `binaural_toggle` |   |   |

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
| Make Active Slice TX | `slice_set_tx` |   |   |
| Select Slice A | `slice_select_a` |   |   |
| Select Slice B | `slice_select_b` |   |   |
| Select Slice C | `slice_select_c` |   |   |
| Select Slice D | `slice_select_d` |   |   |
| Select Slice E | `slice_select_e` |   |   |
| Select Slice F | `slice_select_f` |   |   |
| Select Slice G | `slice_select_g` |   |   |
| Select Slice H | `slice_select_h` |   |   |
| Copy Slice A to B | `slice_a_to_b` |   |   |
| Copy Slice B to A | `slice_b_to_a` |   |   |
| Swap Slices A and B | `slice_swap_ab` |   |   |
| New Slice | `slice_new` |   |   |
| Close Active Slice | `slice_close` |   |   |
| RX Antenna 1 | `rx_antenna_1` |   |   |
| RX Antenna 2 | `rx_antenna_2` |   |   |
| RX Antenna 3 | `rx_antenna_3` |   |   |
| RX Antenna 4 | `rx_antenna_4` |   |   |
| Next RX Antenna | `rx_antenna_next` |   |   |
| Diversity RX | `diversity_toggle` |   |   |

## Filter

| Action | ID | Default key | Notes |
|---|---|---|---|
| Filter Widen | `filter_widen` |   |   |
| Filter Narrow | `filter_narrow` |   |   |
| Filter Width 100 Hz | `filter_width_100` |   |   |
| Filter Width 250 Hz | `filter_width_250` |   |   |
| Filter Width 400 Hz | `filter_width_400` |   |   |
| Filter Width 3 kHz | `filter_width_3000` |   |   |

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
| Noise Reduction | `nr_toggle` |   |   |
| APF | `apf_toggle` |   |   |
| NB Level Up | `nb_level_up` |   | Repeats while held |
| NB Level Down | `nb_level_down` |   | Repeats while held |
| NR Level Up | `nr_level_up` |   | Repeats while held |
| NR Level Down | `nr_level_down` |   | Repeats while held |
| ANF Level Up | `anf_level_up` |   | Repeats while held |
| ANF Level Down | `anf_level_down` |   | Repeats while held |
| APF Level Up | `apf_level_up` |   | Repeats while held |
| APF Level Down | `apf_level_down` |   | Repeats while held |
| WNB | `wnb_toggle` |   |   |
| WNB Level Up | `wnb_level_up` |   | Repeats while held |
| WNB Level Down | `wnb_level_down` |   | Repeats while held |

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
| Select Panadapter 1 | `pan_select_1` |   |   |
| Select Panadapter 2 | `pan_select_2` |   |   |
| Select Panadapter 3 | `pan_select_3` |   |   |
| Select Panadapter 4 | `pan_select_4` |   |   |
| Select Panadapter 5 | `pan_select_5` |   |   |
| Select Panadapter 6 | `pan_select_6` |   |   |
| Select Panadapter 7 | `pan_select_7` |   |   |
| Select Panadapter 8 | `pan_select_8` |   |   |
| Add Panadapter | `pan_add` |   |   |
| Close Active Panadapter | `pan_close` |   |   |
| Panadapter Span 5 kHz | `pan_span_5k` |   |   |
| Panadapter Span 10 kHz | `pan_span_10k` |   |   |
| Panadapter Span 25 kHz | `pan_span_25k` |   |   |
| Panadapter Span 50 kHz | `pan_span_50k` |   |   |
| Panadapter Span 100 kHz | `pan_span_100k` |   |   |
| Panadapter Span 200 kHz | `pan_span_200k` |   |   |
| Recall Memory 1 | `memory_recall_1` |   |   |
| Recall Memory 2 | `memory_recall_2` |   |   |
| Recall Memory 3 | `memory_recall_3` |   |   |
| Recall Memory 4 | `memory_recall_4` |   |   |
| Recall Memory 5 | `memory_recall_5` |   |   |
| Recall Memory 6 | `memory_recall_6` |   |   |
| Recall Memory 7 | `memory_recall_7` |   |   |
| Recall Memory 8 | `memory_recall_8` |   |   |
| Recall Memory 9 | `memory_recall_9` |   |   |
| Recall Memory 10 | `memory_recall_10` |   |   |
| Next Memory | `memory_next` |   |   |
| Previous Memory | `memory_prev` |   |   |
| Load Global Profile 1 | `global_profile_1` |   |   |
| Load Global Profile 2 | `global_profile_2` |   |   |
| Load Global Profile 3 | `global_profile_3` |   |   |
| Load Global Profile 4 | `global_profile_4` |   |   |
| Load Global Profile 5 | `global_profile_5` |   |   |
| Open SpotHub | `open_spothub` |   |   |
| Open AetherModem | `open_aethermodem` |   |   |
| Band Plan Toggle | `band_plan_toggle` |   |   |

## RIT/XIT

| Action | ID | Default key | Notes |
|---|---|---|---|
| RIT Toggle | `rit_toggle` |   |   |
| XIT Toggle | `xit_toggle` |   |   |
| RIT Offset Up | `rit_offset_up` |   | Repeats while held |
| RIT Offset Down | `rit_offset_down` |   | Repeats while held |
| XIT Offset Up | `xit_offset_up` |   | Repeats while held |
| XIT Offset Down | `xit_offset_down` |   | Repeats while held |
