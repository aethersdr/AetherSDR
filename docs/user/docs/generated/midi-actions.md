---
title: "MIDI Controller Actions"
description: "Every control AetherSDR can map to a MIDI controller, with its ID, type and range, generated from the source."
sidebar_position: 3
custom_edit_url: null
generated_by: "tools/docs/gen_reference.py"
generated_from: ["src/gui/MainWindow_Controllers.cpp", "src/gui/MainWindowHelpers.h", "src/core/DigitalVoiceFeature.h", "src/core/DigitalVoiceModeRegistry.cpp", "src/core/MidiControlManager.h"]
---

:::info[Generated page]

This page is generated from `src/gui/MainWindow_Controllers.cpp`, `src/gui/MainWindowHelpers.h`, `src/core/DigitalVoiceFeature.h`, `src/core/DigitalVoiceModeRegistry.cpp`, `src/core/MidiControlManager.h` by `tools/docs/gen_reference.py`. Do not edit it by hand: change the source, then run `python3 tools/docs/gen_reference.py`.

:::

These are the controls you can map to a MIDI controller in **Settings → MIDI Mapping...**. The same list feeds the Ulanzi Dial button menus.

- **Slider**: a continuous control (knob, fader, pitch bend), scaled to the range shown.
- **Toggle**: on/off. A CC above 63 is on; a note toggles.
- **Trigger**: fires once on a note or CC.
- **Gate (held)**: on while the note is held, off when it is released.

MIDI control exists only in builds with `HAVE_MIDI`.

## RX

| Control | ID | Type | Range | Notes |
|---|---|---|---|---|
| AF Gain | `rx.afGain` | Slider | 0 to 200 |   |
| Squelch Level | `rx.squelch` | Slider | 0 to 100 |   |
| AGC Threshold | `rx.agcThreshold` | Slider | 0 to 100 |   |
| Audio Pan | `rx.audioPan` | Slider | 0 to 100 |   |
| Noise Blanker | `rx.nbEnable` | Toggle |   |   |
| Adaptive RX Filter | `rx.adaptiveFilter` | Toggle |   |   |
| Noise Reduction | `rx.nrEnable` | Toggle |   |   |
| Auto Notch | `rx.anfEnable` | Toggle |   |   |
| Squelch Enable | `rx.squelchEnable` | Toggle |   |   |
| Audio Mute | `rx.mute` | Toggle |   |   |
| Tune Lock | `rx.tuneLock` | Toggle |   |   |
| Center Lock | `rx.centerLock` | Toggle |   |   |
| RIT Enable | `rx.ritEnable` | Toggle |   |   |
| XIT Enable | `rx.xitEnable` | Toggle |   |   |
| NR2 (Spectral) | `rx.nr2Enable` | Toggle |   |   |
| RN2 (RNNoise) | `rx.rn2Enable` | Toggle |   |   |
| NR4 (Spectral Bleach) | `rx.nr4Enable` | Toggle |   |   |
| DFNR (DeepFilter) | `rx.dfnrEnable` | Toggle |   |   |
| Step Size Up | `rx.stepUp` | Trigger |   |   |
| Step Size Down | `rx.stepDown` | Trigger |   |   |
| VFO Tune Knob | `rx.tuneKnob` | Slider | 0 to 127 |   |
| NR Cycle | `global.nrCycle` | Trigger |   |   |
| AGC Cycle | `global.agcCycle` | Trigger |   |   |

## TX

| Control | ID | Type | Range | Notes |
|---|---|---|---|---|
| RF Power | `tx.rfPower` | Slider | 0 to 100 |   |
| Tune Power | `tx.tunePower` | Slider | 0 to 100 |   |
| MOX | `tx.mox` | Toggle |   |   |
| TUNE | `tx.tune` | Toggle |   |   |
| ATU Start | `tx.atuStart` | Trigger |   |   |
| Two-Tone Tune | `global.twoToneTune` | Trigger |   |   |

## Phone/CW

| Control | ID | Type | Range | Notes |
|---|---|---|---|---|
| Mic Level | `phone.micLevel` | Slider | 0 to 100 |   |
| Monitor Volume | `phone.monGain` | Slider | 0 to 100 |   |
| Speech Processor | `phone.procEnable` | Toggle |   |   |
| DAX | `phone.daxEnable` | Toggle |   |   |
| Monitor | `phone.monEnable` | Toggle |   |   |
| VOX Enable | `phone.voxEnable` | Toggle |   |   |
| VOX Level | `phone.voxLevel` | Slider | 0 to 100 |   |
| AM Carrier | `phone.amCarrier` | Slider | 0 to 100 |   |
| CW Speed | `cw.speed` | Slider | 5 to 100 |   |
| CW Break-In Delay | `cw.delayMs` | Slider | 0 to 2000 |   |
| CW Sidetone | `cw.sidetoneEnable` | Toggle |   |   |
| CW Iambic | `cw.iambicEnable` | Toggle |   |   |
| CW Iambic Mode (0=A, 1=B) | `cw.iambicMode` | Toggle |   |   |
| CW Swap Paddles | `cw.swapPaddles` | Toggle |   |   |
| CWL Frequency Offset | `cw.cwlEnable` | Toggle |   |   |
| CW Break-In (QSK) | `cw.breakInEnable` | Toggle |   |   |
| Trigger straight key | `cwkey` | Gate (held) |   |   |
| Trigger CW Left Paddle | `cwdit` | Gate (held) |   |   |
| Trigger CW Right Paddle | `cwdah` | Gate (held) |   |   |
| PTT (hold) | `cw.ptt` | Gate (held) |   |   |

## EQ

| Control | ID | Type | Range | Notes |
|---|---|---|---|---|
| TX EQ Enable | `eq.txEnable` | Toggle |   |   |
| RX EQ Enable | `eq.rxEnable` | Toggle |   |   |
| 63 Hz | `eq.band63` | Slider | -10 to 10 |   |
| 125 Hz | `eq.band125` | Slider | -10 to 10 |   |
| 250 Hz | `eq.band250` | Slider | -10 to 10 |   |
| 500 Hz | `eq.band500` | Slider | -10 to 10 |   |
| 1 kHz | `eq.band1000` | Slider | -10 to 10 |   |
| 2 kHz | `eq.band2000` | Slider | -10 to 10 |   |
| 4 kHz | `eq.band4000` | Slider | -10 to 10 |   |
| 8 kHz | `eq.band8000` | Slider | -10 to 10 |   |

## Global

| Control | ID | Type | Range | Notes |
|---|---|---|---|---|
| Master Volume | `global.masterVolume` | Slider | 0 to 100 |   |
| Headphone Volume | `global.hpVolume` | Slider | 0 to 100 |   |
| Master Mute | `global.masterMute` | Toggle |   |   |
| TX Button | `global.txButton` | Toggle |   |   |
| RADE Modem | `global.rade` | Toggle |   | Only in builds with `HAVE_RADE` |
| TNF Global | `global.tnfEnable` | Toggle |   |   |
| Band Up | `global.bandUp` | Trigger |   |   |
| Band Down | `global.bandDown` | Trigger |   |   |
| Mode Up | `global.modeUp` | Trigger |   |   |
| Mode Down | `global.modeDown` | Trigger |   |   |
| Next Slice | `global.nextSlice` | Trigger |   |   |
| Previous Slice | `global.prevSlice` | Trigger |   |   |
| QSO Record | `global.qsoRecord` | Toggle |   |   |
| QSO Playback | `global.qsoPlay` | Toggle |   |   |

## Mode

| Control | ID | Type | Range | Notes |
|---|---|---|---|---|
| Mode USB | `global.modeUSB` | Trigger |   |   |
| Mode LSB | `global.modeLSB` | Trigger |   |   |
| Mode CW | `global.modeCW` | Trigger |   |   |
| Mode CWL | `global.modeCWL` | Trigger |   |   |
| Mode AM | `global.modeAM` | Trigger |   |   |
| Mode SAM | `global.modeSAM` | Trigger |   |   |
| Mode FM | `global.modeFM` | Trigger |   |   |
| Mode NFM | `global.modeNFM` | Trigger |   |   |
| Mode DFM | `global.modeDFM` | Trigger |   |   |
| Mode DSTR | `global.modeDSTR` | Trigger |   | Only in builds with `AETHER_ENABLE_DIGITAL_VOICE_HELPER` |
| Mode DIGU | `global.modeDIGU` | Trigger |   |   |
| Mode DIGL | `global.modeDIGL` | Trigger |   |   |
| Mode RTTY | `global.modeRTTY` | Trigger |   |   |
| Mode FDVU | `global.modeFDVU` | Trigger |   |   |
| Mode FDVL | `global.modeFDVL` | Trigger |   |   |

## Band

| Control | ID | Type | Range | Notes |
|---|---|---|---|---|
| Band 160m | `global.band160m` | Trigger |   |   |
| Band 80m | `global.band80m` | Trigger |   |   |
| Band 60m | `global.band60m` | Trigger |   |   |
| Band 40m | `global.band40m` | Trigger |   |   |
| Band 30m | `global.band30m` | Trigger |   |   |
| Band 20m | `global.band20m` | Trigger |   |   |
| Band 17m | `global.band17m` | Trigger |   |   |
| Band 15m | `global.band15m` | Trigger |   |   |
| Band 12m | `global.band12m` | Trigger |   |   |
| Band 10m | `global.band10m` | Trigger |   |   |
| Band 6m | `global.band6m` | Trigger |   |   |
| Band 2m | `global.band2m` | Trigger |   |   |

## Slice

| Control | ID | Type | Range | Notes |
|---|---|---|---|---|
| Split Toggle | `global.splitToggle` | Trigger |   |   |

## Filter

| Control | ID | Type | Range | Notes |
|---|---|---|---|---|
| Filter Widen | `global.filterWiden` | Trigger |   |   |
| Filter Narrow | `global.filterNarrow` | Trigger |   |   |

## Frequency

| Control | ID | Type | Range | Notes |
|---|---|---|---|---|
| Tune Up 1 MHz | `global.tuneUp1mhz` | Trigger |   |   |
| Tune Down 1 MHz | `global.tuneDown1mhz` | Trigger |   |   |

## Display

| Control | ID | Type | Range | Notes |
|---|---|---|---|---|
| Band Zoom | `global.bandZoom` | Trigger |   |   |
| Segment Zoom | `global.segmentZoom` | Trigger |   |   |
| Panadapter Zoom In | `global.panZoomIn` | Trigger |   |   |
| Panadapter Zoom Out | `global.panZoomOut` | Trigger |   |   |
| Open Memories | `global.openMemories` | Trigger |   |   |
