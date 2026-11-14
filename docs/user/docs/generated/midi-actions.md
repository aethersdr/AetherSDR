---
title: "MIDI Controller Actions"
description: "Every control AetherSDR can map to a MIDI controller, with its ID, type and range, generated from the source."
sidebar_position: 3
custom_edit_url: null
generated_by: "tools/docs/gen_reference.py"
generated_from: ["src/gui/MainWindow_Controllers.cpp", "src/gui/MainWindow_ControlActions.cpp", "src/gui/MainWindowHelpers.h", "src/core/DigitalVoiceFeature.h", "src/core/DigitalVoiceModeRegistry.cpp", "src/core/MidiControlManager.h"]
---

:::info[Generated page]

This page is generated from `src/gui/MainWindow_Controllers.cpp`, `src/gui/MainWindow_ControlActions.cpp`, `src/gui/MainWindowHelpers.h`, `src/core/DigitalVoiceFeature.h`, `src/core/DigitalVoiceModeRegistry.cpp`, `src/core/MidiControlManager.h` by `tools/docs/gen_reference.py`. Do not edit it by hand: change the source, then run `python3 tools/docs/gen_reference.py`.

:::

These are the controls you can map to a MIDI controller in **Settings → Controllers → MIDI Mapping...**. The same list feeds the Ulanzi Dial button menus.

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
| Noise Reduction | `rx.nrEnable` | Toggle |   |   |
| Filter Width 100 Hz | `rx.filterWidth100` | Trigger |   |   |
| Filter Width 250 Hz | `rx.filterWidth250` | Trigger |   |   |
| Filter Width 400 Hz | `rx.filterWidth400` | Trigger |   |   |
| Filter Width 3 kHz | `rx.filterWidth3000` | Trigger |   |   |
| Filter Width | `rx.filterWidth` | Slider | 0 to 100 |   |
| APF | `rx.apfEnable` | Toggle |   |   |
| NB Level | `rx.nbLevel` | Slider | 0 to 100 |   |
| NR Level | `rx.nrLevel` | Slider | 0 to 100 |   |
| ANF Level | `rx.anfLevel` | Slider | 0 to 100 |   |
| APF Level | `rx.apfLevel` | Slider | 0 to 100 |   |
| RIT Offset | `rx.ritOffset` | Slider | -99999 to 99999 |   |
| RX Antenna 1 | `rx.antenna1` | Trigger |   |   |
| RX Antenna 2 | `rx.antenna2` | Trigger |   |   |
| RX Antenna 3 | `rx.antenna3` | Trigger |   |   |
| RX Antenna 4 | `rx.antenna4` | Trigger |   |   |
| Next RX Antenna | `rx.antennaNext` | Trigger |   |   |
| WNB | `rx.wnbEnable` | Toggle |   |   |
| WNB Level | `rx.wnbLevel` | Slider | 0 to 100 |   |
| Binaural RX | `rx.binaural` | Toggle |   |   |
| Diversity RX | `rx.diversity` | Toggle |   |   |

## TX

| Control | ID | Type | Range | Notes |
|---|---|---|---|---|
| Tune Power | `tx.tunePower` | Slider | 0 to 100 |   |
| MOX | `tx.mox` | Toggle |   |   |
| TUNE | `tx.tune` | Toggle |   |   |
| ATU Start | `tx.atuStart` | Trigger |   |   |
| Two-Tone Tune | `global.twoToneTune` | Trigger |   |   |
| RF Power | `tx.rfPower` | Slider | 0 to 100 |   |
| XIT Offset | `tx.xitOffset` | Slider | -99999 to 99999 |   |
| TX Antenna 1 | `tx.antenna1` | Trigger |   |   |
| TX Antenna 2 | `tx.antenna2` | Trigger |   |   |
| TX Antenna 3 | `tx.antenna3` | Trigger |   |   |
| TX Antenna 4 | `tx.antenna4` | Trigger |   |   |
| Next TX Antenna | `tx.antennaNext` | Trigger |   |   |
| ATU Memories | `tx.atuMemories` | Toggle |   |   |
| ACC TX Output | `tx.outputAcc` | Toggle |   |   |
| TX1 Output | `tx.output1` | Toggle |   |   |
| TX2 Output | `tx.output2` | Toggle |   |   |
| TX3 Output | `tx.output3` | Toggle |   |   |
| ATU Bypass | `tx.atuBypass` | Trigger |   |   |

## Phone/CW

| Control | ID | Type | Range | Notes |
|---|---|---|---|---|
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
| Mic Level | `phone.micLevel` | Slider | 0 to 100 |   |
| CW Keyer: Iambic A | `cw.keyerIambicA` | Trigger |   |   |
| CW Keyer: Iambic B | `cw.keyerIambicB` | Trigger |   |   |
| CW Keyer: Straight Key | `cw.keyerStraight` | Trigger |   |   |
| Stop CWX / Macros | `cwx.cancel` | Trigger |   |   |
| CW Decoder | `cw.decodeRx` | Toggle |   |   |
| Show/Hide CWX Panel | `cwx.panelToggle` | Trigger |   |   |
| Macro F1 | `cwx.macro1` | Trigger |   |   |
| Macro F2 | `cwx.macro2` | Trigger |   |   |
| Macro F3 | `cwx.macro3` | Trigger |   |   |
| Macro F4 | `cwx.macro4` | Trigger |   |   |
| Macro F5 | `cwx.macro5` | Trigger |   |   |
| Macro F6 | `cwx.macro6` | Trigger |   |   |
| Macro F7 | `cwx.macro7` | Trigger |   |   |
| Macro F8 | `cwx.macro8` | Trigger |   |   |
| Macro F9 | `cwx.macro9` | Trigger |   |   |
| Macro F10 | `cwx.macro10` | Trigger |   |   |
| Macro F11 | `cwx.macro11` | Trigger |   |   |
| Macro F12 | `cwx.macro12` | Trigger |   |   |

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
| Next Slice | `global.nextSlice` | Trigger |   |   |
| Previous Slice | `global.prevSlice` | Trigger |   |   |
| QSO Record | `global.qsoRecord` | Toggle |   |   |
| QSO Playback | `global.qsoPlay` | Toggle |   |   |
| Band Up | `global.bandUp` | Trigger |   |   |
| Band Down | `global.bandDown` | Trigger |   |   |
| Mode Up | `global.modeUp` | Trigger |   |   |
| Mode Down | `global.modeDown` | Trigger |   |   |
| Make Active Slice TX | `global.sliceSetTx` | Trigger |   |   |
| Select Slice A | `global.sliceSelectA` | Trigger |   |   |
| Select Slice B | `global.sliceSelectB` | Trigger |   |   |
| Select Slice C | `global.sliceSelectC` | Trigger |   |   |
| Select Slice D | `global.sliceSelectD` | Trigger |   |   |
| Select Slice E | `global.sliceSelectE` | Trigger |   |   |
| Select Slice F | `global.sliceSelectF` | Trigger |   |   |
| Select Slice G | `global.sliceSelectG` | Trigger |   |   |
| Select Slice H | `global.sliceSelectH` | Trigger |   |   |
| Copy Slice A to B | `global.sliceAToB` | Trigger |   |   |
| Copy Slice B to A | `global.sliceBToA` | Trigger |   |   |
| Swap Slices A and B | `global.sliceSwapAB` | Trigger |   |   |
| New Slice | `global.sliceNew` | Trigger |   |   |
| Close Active Slice | `global.sliceClose` | Trigger |   |   |
| Select Panadapter 1 | `global.panSelect1` | Trigger |   |   |
| Select Panadapter 2 | `global.panSelect2` | Trigger |   |   |
| Select Panadapter 3 | `global.panSelect3` | Trigger |   |   |
| Select Panadapter 4 | `global.panSelect4` | Trigger |   |   |
| Select Panadapter 5 | `global.panSelect5` | Trigger |   |   |
| Select Panadapter 6 | `global.panSelect6` | Trigger |   |   |
| Select Panadapter 7 | `global.panSelect7` | Trigger |   |   |
| Select Panadapter 8 | `global.panSelect8` | Trigger |   |   |
| Add Panadapter | `global.panAdd` | Trigger |   |   |
| Close Active Panadapter | `global.panClose` | Trigger |   |   |
| Panadapter Span 5 kHz | `global.panSpan5k` | Trigger |   |   |
| Panadapter Span 10 kHz | `global.panSpan10k` | Trigger |   |   |
| Panadapter Span 25 kHz | `global.panSpan25k` | Trigger |   |   |
| Panadapter Span 50 kHz | `global.panSpan50k` | Trigger |   |   |
| Panadapter Span 100 kHz | `global.panSpan100k` | Trigger |   |   |
| Panadapter Span 200 kHz | `global.panSpan200k` | Trigger |   |   |
| Recall Memory 1 | `global.memoryRecall1` | Trigger |   |   |
| Recall Memory 2 | `global.memoryRecall2` | Trigger |   |   |
| Recall Memory 3 | `global.memoryRecall3` | Trigger |   |   |
| Recall Memory 4 | `global.memoryRecall4` | Trigger |   |   |
| Recall Memory 5 | `global.memoryRecall5` | Trigger |   |   |
| Recall Memory 6 | `global.memoryRecall6` | Trigger |   |   |
| Recall Memory 7 | `global.memoryRecall7` | Trigger |   |   |
| Recall Memory 8 | `global.memoryRecall8` | Trigger |   |   |
| Recall Memory 9 | `global.memoryRecall9` | Trigger |   |   |
| Recall Memory 10 | `global.memoryRecall10` | Trigger |   |   |
| Next Memory | `global.memoryNext` | Trigger |   |   |
| Previous Memory | `global.memoryPrev` | Trigger |   |   |
| Load Global Profile 1 | `global.globalProfile1` | Trigger |   |   |
| Load Global Profile 2 | `global.globalProfile2` | Trigger |   |   |
| Load Global Profile 3 | `global.globalProfile3` | Trigger |   |   |
| Load Global Profile 4 | `global.globalProfile4` | Trigger |   |   |
| Load Global Profile 5 | `global.globalProfile5` | Trigger |   |   |
| Band Stack Up | `global.bandStackUp` | Trigger |   |   |
| Band Stack Down | `global.bandStackDown` | Trigger |   |   |
| Open SpotHub | `global.openSpotHub` | Trigger |   |   |
| Open AetherModem | `global.openAetherModem` | Trigger |   |   |
| Band Plan Toggle | `global.bandPlanToggle` | Trigger |   |   |

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
| Tune Up (100 steps) | `global.tuneUp100` | Trigger |   |   |
| Tune Down (100 steps) | `global.tuneDown100` | Trigger |   |   |
| Tune Up (1 step) | `global.tuneUp1` | Trigger |   |   |
| Tune Down (1 step) | `global.tuneDown1` | Trigger |   |   |
| Tune Up (10 steps) | `global.tuneUp10` | Trigger |   |   |
| Tune Down (10 steps) | `global.tuneDown10` | Trigger |   |   |
| Go to Frequency | `global.goToFreq` | Trigger |   |   |

## Display

| Control | ID | Type | Range | Notes |
|---|---|---|---|---|
| Band Zoom | `global.bandZoom` | Trigger |   |   |
| Segment Zoom | `global.segmentZoom` | Trigger |   |   |
| Panadapter Zoom In | `global.panZoomIn` | Trigger |   |   |
| Panadapter Zoom Out | `global.panZoomOut` | Trigger |   |   |
| Open Memories | `global.openMemories` | Trigger |   |   |
| Callsign Lookup | `global.callsignLookup` | Trigger |   |   |
