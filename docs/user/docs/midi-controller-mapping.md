---
title: "MIDI Controller Mapping"
slug: "/midi-controller-mapping"
description: "AetherSDR supports class-compliant USB MIDI controllers for hands-on control of the radio."
---

AetherSDR supports class-compliant USB MIDI controllers for hands-on control of the radio. Any standard controller — Behringer X-Touch Mini, Korg nanoKONTROL, Akai MIDImix, a DJ controller, or anything else that sends MIDI CC, Note or Pitch Bend messages — can map its knobs, faders, encoders and buttons to AetherSDR controls.

## Requirements

- A class-compliant USB MIDI controller (no special drivers needed)
- MIDI support is built into AetherSDR on all platforms (RtMidi is bundled)

## Setup

1. Plug in your MIDI controller.
2. Open **Settings → Controllers → MIDI Mapping...**.
3. Click **Refresh**, pick your controller from the **Port** list and click **Connect** — the status turns green.
4. Move a knob or fader — the activity line shows the MIDI message.
5. Choose a parameter (for example "AF Gain").
6. Click **Learn**, then move the control you want to assign.
7. The binding appears in the table — that control now drives AF Gain.

## Using the mapping dialog

**MIDI Device**

<img src="/img/screens/midi-mapping-dialog.png" width="700" alt="MIDI Controller Mapping window, with a round close button in its top-right corner. The MIDI Device group has a Port selector, Refresh and Connect buttons, a Disconnected status and Auto-connect on startup ticked. The Parameter Bindings table, with columns Parameter, MIDI Source, Channel, Invert and Relative, is empty. Below are a category filter, a parameter selector set to [RX] AF Gain with Learn and Manual… buttons, and Clear All, Profile, Save, Load, Import… and Export… controls." />

*The MIDI Controller Mapping window, before any control is bound.*

It is a window of its own that remembers its size and position. Close it with the round **✕** in its top-right corner, **Esc**, or **⌘W** (**Ctrl+W** on Windows and Linux); choosing **MIDI Mapping...** while it is open brings it to the front.

- **Port** with **Refresh** and **Connect / Disconnect**
- **Auto-connect on startup** — reconnect to the last device at launch
- Activity line — the last message received (channel, type, number, value)

**Parameter Bindings**

- Binding table: **Parameter**, **MIDI Source**, **Channel**, **Invert**, **Relative**, an edit (**✎**) button and a delete (**×**) button
- Category filter: All, RX, TX, Phone/CW, EQ, Global, Mode, Band, Filter, Slice, Display, Frequency
- **Learn** / **Manual…** to add a binding, **Clear All** to remove every binding
- **Profile** row: **Save**, **Load**, **Import...**, **Export...**

## Adding bindings

### MIDI Learn

1. Choose a parameter (use the category filter to narrow the list).
2. Click **Learn** — the button changes to **Cancel Learn**.
3. Move a knob, press a button or slide a fader.
4. AetherSDR captures the message and creates the binding. Bindings are saved automatically.

Closing the window cancels a Learn that is still waiting for a control. Minimising it, or hiding AetherSDR, does not: the Learn stays armed and saves the binding when you move the control.

### Manual entry

Click **Manual…** to type a binding instead of learning it: **Channel** (Any or 1–16), **Type** (Note On, Control Change (CC) or Pitch Bend), **Number** (note or CC number), plus **Invert value range** and **Relative (knob sends deltas)**.

The **✎** button on any row opens the same form filled in, so you can fix a binding that Learn captured wrongly without re-learning it. If two bindings share the same source, AetherSDR asks before going ahead, because only the last one would work: **Replace** removes the older binding, and **Cancel** (the default, so also Return, **Esc** or the **✕**) leaves both as they were and adds nothing.

## Using relative encoders

Endless encoders send small "turned up / turned down" deltas instead of an absolute position. Tick **Relative** on the binding. AetherSDR recognizes both common relative encodings (two's-complement 1/127 and center-64 65/63) from the first detents and re-checks if the values stop making sense, so a mis-detected knob corrects itself. Fast turns are smoothed so a burst of deltas does not overshoot.

The **VFO Tune Knob** parameter is meant for an endless encoder bound as **Relative**; as an absolute control it treats 64 as center.

These sliders also step with a **Relative** encoder, by their keyboard step per detent: Filter Width, RIT Offset and XIT Offset (10 Hz), NB / NR / ANF / APF / WNB Level, RF Power and Mic Level.

## Using profiles

Save your bindings as named profiles to switch between controllers or layouts.

- **Save** — type a name in the **Profile** box and click **Save**. The dialog reports whether it saved or overwrote a profile.
- **Load** — pick a profile and click **Load** to apply it to the current bindings.
- **Import...** — load a profile file into your profile list. It accepts AetherSDR profile XML or a **SmartSDR `.map` file** (the per-controller maps vendors publish for SmartSDR for iOS/Mac); the format is detected from the content, and SmartSDR band and mode selections are translated. A summary lists what was imported and anything skipped — controls with no AetherSDR equivalent, invalid values and duplicates — so nothing is dropped silently. A name that already exists gets a " (2)" suffix instead of being overwritten. Click **Load** to apply the imported profile.
- **Export...** — write the current bindings to an AetherSDR profile XML (suggested name `AetherSDR_MidiProfile_<date>_<time>_v<version>.xml`) that **Import** reads back.

## Using MIDI with non-Flex radios

On Hermes-Lite 2, Icom, ANAN-G2 and RTL-SDR, bindings for features the radio does not have (for example CWL, NR/ANF on, split or CWX) refuse with a warning and a once-per-session notice instead of silently doing nothing. On these radios **NR Cycle** goes straight to NR2.

**Master Volume** sets a FlexRadio's line-out gain. On the other radios it moves the title-bar master volume while PC Audio is on, and is refused while PC Audio is off. **Headphone Volume** is refused on radios without a headphone output.

## Reference

### Supported MIDI messages

| Message Type | Use | Example |
|-------------|-----|---------|
| **Control Change (CC)** | Knobs, faders, encoders | CC #7 → AF Gain (0–127 maps to 0–200) |
| **Note On / Off** | Buttons, pads | Note C3 → MOX |
| **Pitch Bend** | High-resolution controls | 14-bit (0–16383) |

### Value mapping

- **Sliders:** CC value 0–127 maps linearly to the parameter's range.
- **Toggles:** CC > 63 = on, ≤ 63 = off. Note On toggles the current state.
- **Triggers:** fire on Note On or CC > 63 (one shot, no repeat while held).
- **Gates:** on while the note or CC is held, off when released.
- **Pitch Bend:** 14-bit value (0–16383) for high-resolution control.
- **Invert** reverses the range (for faders mounted upside-down).

### Mappable parameters

The tables below cover the long-standing parameters. The full list, including every action that is also a keyboard shortcut, with its ID, is on [MIDI Controller Actions](./generated/midi-actions.md). Every keyboard action is also a MIDI control; see [Keyboard Shortcuts](./keyboard-shortcuts.md#actions-you-can-assign) for the groups.

#### RX
| Parameter | Type | Range |
|-----------|------|-------|
| AF Gain | Slider | 0–200 |
| Squelch Level | Slider | 0–100 |
| AGC Threshold | Slider | 0–100 |
| Audio Pan | Slider | 0–100 |
| VFO Tune Knob | Slider | 0–127 (relative, center 64) |
| Noise Blanker, Adaptive RX Filter, Noise Reduction, Auto Notch | Toggle | on/off |
| NR2 (Spectral), RN2 (RNNoise), NR4 (Spectral Bleach), DFNR (DeepFilter) | Toggle | on/off |
| Squelch Enable, Audio Mute, Tune Lock, Center Lock | Toggle | on/off |
| RIT Enable, XIT Enable | Toggle | on/off |
| Step Size Up, Step Size Down | Trigger | momentary |
| NR Cycle, AGC Cycle | Trigger | momentary |

#### TX
| Parameter | Type | Range |
|-----------|------|-------|
| RF Power | Slider | 0–100 |
| Tune Power | Slider | 0–100 |
| MOX | Toggle | on/off |
| TUNE | Toggle | on/off |
| ATU Start | Trigger | momentary |
| Two-Tone Tune | Trigger | momentary |

#### Phone/CW
| Parameter | Type | Range |
|-----------|------|-------|
| Mic Level | Slider | 0–100 |
| Monitor Volume | Slider | 0–100 |
| VOX Level | Slider | 0–100 |
| AM Carrier | Slider | 0–100 |
| CW Speed | Slider | 5–100 WPM |
| CW Break-In Delay | Slider | 0–2000 ms |
| Speech Processor, DAX, Monitor, VOX Enable | Toggle | on/off |
| CW Sidetone, CW Iambic, CW Iambic Mode (0=A, 1=B), CW Swap Paddles, CWL Frequency Offset, CW Break-In (QSK) | Toggle | on/off |
| PTT (hold) | Gate | held = transmit |
| Trigger straight key, Trigger CW Left Paddle, Trigger CW Right Paddle | Gate | held = key down |

Gate parameters follow the button: press to key, release to un-key.

#### EQ
| Parameter | Type | Range |
|-----------|------|-------|
| TX EQ Enable, RX EQ Enable | Toggle | on/off |
| 63 Hz … 8 kHz (8 bands, TX EQ) | Slider | −10 to +10 dB |

#### Global
| Parameter | Type | Range |
|-----------|------|-------|
| Master Volume | Slider | 0–100 |
| Headphone Volume | Slider | 0–100 |
| Master Mute, TX Button, TNF Global | Toggle | on/off |
| RADE Modem | Toggle | on/off |
| QSO Record, QSO Playback | Toggle | on/off |
| Band Up, Band Down | Trigger | momentary |
| Mode Up, Mode Down | Trigger | momentary |
| Next Slice, Previous Slice | Trigger | momentary |

#### Mode, Band, Filter, Slice, Display, Frequency
| Category | Parameters (all triggers) |
|----------|---------------------------|
| Mode | Mode USB, LSB, CW, CWL, AM, SAM, FM, NFM, DFM, DSTR, DIGU, DIGL, RTTY, FDVU, FDVL |
| Band | Band 160m, 80m, 60m, 40m, 30m, 20m, 17m, 15m, 12m, 10m, 6m, 2m |
| Filter | Filter Widen, Filter Narrow |
| Slice | Split Toggle |
| Display | Band Zoom, Segment Zoom, Panadapter Zoom In, Panadapter Zoom Out, Open Memories |
| Frequency | Tune Up 1 MHz, Tune Down 1 MHz |

Digital-voice modes that need a helper (such as DSTR) are only offered when that helper is available. **Mode Up / Mode Down** step only through the modes the radio reports, in the order above, so a mode the radio lacks (CWL on some FlexRadio models, for example) is skipped. On a radio that reports no mode list, they step through the whole list except FDVU and FDVL. Either way they skip DSTR while the D-STAR helper is not running.

**RADE Modem** turns the RADE modem on or off on the active slice. Turning it on switches the slice to DIGU or DIGL and makes it the TX slice; if RADE is running on another slice, it moves to the active one. Turning it off leaves the slice in DIGU/DIGL. RADE needs DAX audio, so on a radio without it the press is ignored. The parameter is present in builds with RADE support.

### Coming from SmartSDR for Mac

SmartSDR for Mac's Stream Deck and MIDI action list, and the AetherSDR control that does the same thing. Bind the keyboard action in **Settings → Configure Shortcuts...** and the MIDI control in **Settings → Controllers → MIDI Mapping...**. "Not available" rows are SmartSDR for Mac windows and panels AetherSDR does not have.

<details>
<summary>SmartSDR for Mac actions and their AetherSDR equivalents</summary>

| SmartSDR for Mac | Keyboard action | MIDI control |
|---|---|---|
| A &lt;&gt; B | **Swap Slices A and B** | **Swap Slices A and B** |
| A &gt; B | **Copy Slice A to B** | **Copy Slice A to B** |
| ACC TX toggle | **ACC TX Output** | **ACC TX Output** |
| ANF | **ANF Toggle** | **Auto Notch** |
| APF toggle | **APF** | **APF** |
| ATU | **ATU Start** | **ATU Start** |
| ATU Bypass | **ATU Bypass** | **ATU Bypass** |
| ATU MEM | **ATU Memories** | **ATU Memories** |
| ATU Tune | **ATU Start** | **ATU Start** |
| B &gt; A | **Copy Slice B to A** | **Copy Slice B to A** |
| Band Next | **Band Up** | **Band Up** |
| Band Previous | **Band Down** | **Band Down** |
| Band zoom toggle | **Band Zoom** | **Band Zoom** |
| Bandwidth 10 KHz | **Panadapter Span 10 kHz** | **Panadapter Span 10 kHz** |
| Bandwidth 25 KHz | **Panadapter Span 25 kHz** | **Panadapter Span 25 kHz** |
| Bandwidth 5 KHz | **Panadapter Span 5 kHz** | **Panadapter Span 5 kHz** |
| Bandwidth 50 KHz | **Panadapter Span 50 kHz** | **Panadapter Span 50 kHz** |
| Binaural RX toggle | **Binaural RX** | **Binaural RX** |
| Cancel CWX | **Stop CWX / Macros** | **Stop CWX / Macros** |
| Clear Log Window | — | Not available: no AetherSDR equivalent |
| Close Panadapter | **Close Active Panadapter** | **Close Active Panadapter** |
| Command 1 | — | Not available: no AetherSDR equivalent |
| Command 10 | — | Not available: no AetherSDR equivalent |
| Command 2 | — | Not available: no AetherSDR equivalent |
| Command 3 | — | Not available: no AetherSDR equivalent |
| Command 4 | — | Not available: no AetherSDR equivalent |
| Command 5 | — | Not available: no AetherSDR equivalent |
| Command 6 | — | Not available: no AetherSDR equivalent |
| Command 7 | — | Not available: no AetherSDR equivalent |
| Command 8 | — | Not available: no AetherSDR equivalent |
| Command 9 | — | Not available: no AetherSDR equivalent |
| Create Panadapter | **Add Panadapter** | **Add Panadapter** |
| CWX | **Show/Hide CWX Panel** | **Show/Hide CWX Panel** |
| DAX toggle | **DAX TX Toggle** | **DAX** |
| Delete Slice | **Close Active Slice** | **Close Active Slice** |
| Diversity toggle | **Diversity RX** | **Diversity RX** |
| Enter Frequency | **Go to Frequency** | **Go to Frequency** |
| Filter width 100 Hz | **Filter Width 100 Hz** | **Filter Width 100 Hz** |
| Filter width 250 Hz | **Filter Width 250 Hz** | **Filter Width 250 Hz** |
| Filter width 3 KHz | **Filter Width 3 kHz** | **Filter Width 3 kHz** |
| Filter width 400 Hz | **Filter Width 400 Hz** | **Filter Width 400 Hz** |
| Frequency Down | **Tune Down (1 step)** | **Tune Down (1 step)** |
| Frequency Down 100x | **Tune Down (100 steps)** | **Tune Down (100 steps)** |
| Frequency Down 10x | **Tune Down (10 steps)** | **Tune Down (10 steps)** |
| Frequency Up | **Tune Up (1 step)** | **Tune Up (1 step)** |
| Frequency Up 100x | **Tune Up (100 steps)** | **Tune Up (100 steps)** |
| Frequency Up 10x | **Tune Up (10 steps)** | **Tune Up (10 steps)** |
| Global Profile 1 | — | Profile ruling pending |
| Global Profile 2 | — | Profile ruling pending |
| Global Profile 3 | — | Profile ruling pending |
| Global Profile 4 | — | Profile ruling pending |
| Global Profile 5 | — | Profile ruling pending |
| Log QSO | — | AetherSDR has no QSO logbook |
| Macro 1 | **CWX Macro F1** | **CWX Macro F1** |
| Macro 10 | **CWX Macro F10** | **CWX Macro F10** |
| Macro 11 | **CWX Macro F11** | **CWX Macro F11** |
| Macro 12 | **CWX Macro F12** | **CWX Macro F12** |
| Macro 2 | **CWX Macro F2** | **CWX Macro F2** |
| Macro 3 | **CWX Macro F3** | **CWX Macro F3** |
| Macro 4 | **CWX Macro F4** | **CWX Macro F4** |
| Macro 5 | **CWX Macro F5** | **CWX Macro F5** |
| Macro 6 | **CWX Macro F6** | **CWX Macro F6** |
| Macro 7 | **CWX Macro F7** | **CWX Macro F7** |
| Macro 8 | **CWX Macro F8** | **CWX Macro F8** |
| Macro 9 | **CWX Macro F9** | **CWX Macro F9** |
| Macros Stop | **Stop CWX / Macros** | **Stop CWX / Macros** |
| Main Audio Mute toggle | **Master Mute Toggle** | **Master Mute** |
| Memory 1 | **Recall Memory 1** | **Recall Memory 1** |
| Memory 10 | **Recall Memory 10** | **Recall Memory 10** |
| Memory 2 | **Recall Memory 2** | **Recall Memory 2** |
| Memory 3 | **Recall Memory 3** | **Recall Memory 3** |
| Memory 4 | **Recall Memory 4** | **Recall Memory 4** |
| Memory 5 | **Recall Memory 5** | **Recall Memory 5** |
| Memory 6 | **Recall Memory 6** | **Recall Memory 6** |
| Memory 7 | **Recall Memory 7** | **Recall Memory 7** |
| Memory 8 | **Recall Memory 8** | **Recall Memory 8** |
| Memory 9 | **Recall Memory 9** | **Recall Memory 9** |
| Memory Down | **Previous Memory** | **Previous Memory** |
| Memory Up | **Next Memory** | **Next Memory** |
| Mode AM | **AM** | **Mode AM** |
| Mode CW | **CW** | **Mode CW** |
| Mode DIGL | **DIGL** | **Mode DIGL** |
| Mode DIGU | **DIGU** | **Mode DIGU** |
| Mode FM | **FM** | **Mode FM** |
| Mode LSB | **LSB** | **Mode LSB** |
| Mode Next | **Mode Up** | **Mode Up** |
| Mode Previous | **Mode Down** | **Mode Down** |
| Mode RTTY | **RTTY** | **Mode RTTY** |
| Mode SAM | **SAM** | **Mode SAM** |
| Mode USB | **USB** | **Mode USB** |
| MOX | **MOX Toggle** | **MOX** |
| NB | **NB Toggle** | **Noise Blanker** |
| Not assigned | — | Not available: no AetherSDR equivalent |
| NR | **Noise Reduction** | **Noise Reduction** |
| Open APRS | — | Not available: no AetherSDR equivalent |
| Open Band Plan Tool | **Band Plan Toggle** | **Band Plan Toggle** |
| Open Call lookup | Ctrl+Shift+L (fixed) | **Callsign Lookup** |
| Open CW Decoder | **CW Decoder** | **CW Decoder** |
| Open DX-Cluster Tool | **Open SpotHub** | **Open SpotHub** |
| Open Frequency List Tool | — | Not available: no AetherSDR equivalent |
| Open FT8 Tool | — | Not available: no AetherSDR equivalent |
| Open Green Heron Tool | — | Not available: no AetherSDR equivalent |
| Open HF-Fax | — | Not available: no AetherSDR equivalent |
| Open Logbook Tool | — | AetherSDR has no QSO logbook |
| Open Memories | **Open Memories Dialog** | **Open Memories** |
| Open Metropwr Tool | — | Not available: no AetherSDR equivalent |
| Open POTA Tool | — | Not available: no AetherSDR equivalent |
| Open PSK | — | Not available: no AetherSDR equivalent |
| Open PSK Reporter Tool | — | Not available: no AetherSDR equivalent |
| Open RTTY | — | Not available: no AetherSDR equivalent |
| Open S.A.T. Controller Tool | — | Not available: no AetherSDR equivalent |
| Open SSTV | — | Not available: no AetherSDR equivalent |
| Open Ultra Beam Tool | — | Not available: no AetherSDR equivalent |
| Open Waveshare Tool | — | Not available: no AetherSDR equivalent |
| PTT Push | **PTT (Hold)** | **PTT (hold)** |
| PTT toggle | **MOX Toggle** | **TX Button** |
| RCA TX 1 toggle | **TX1 Output** | **TX1 Output** |
| RCA TX 2 toggle | **TX2 Output** | **TX2 Output** |
| RCA TX 3 toggle | **TX3 Output** | **TX3 Output** |
| RIT | **RIT Toggle** | **RIT Enable** |
| RIT Down | **RIT Offset Down** | **RIT Offset** |
| RIT Up | **RIT Offset Up** | **RIT Offset** |
| RX Ant 1 | **RX Antenna 1** | **RX Antenna 1** |
| RX Ant 2 | **RX Antenna 2** | **RX Antenna 2** |
| RX RX_A | **RX Antenna 3** | **RX Antenna 3** (by position in the radio's antenna list) |
| RX XVTR | **RX Antenna 4** | **RX Antenna 4** (by position in the radio's antenna list) |
| Select 10m Band | **10m** | **Band 10m** |
| Select 12m Band | **12m** | **Band 12m** |
| Select 15m Band | **15m** | **Band 15m** |
| Select 160m Band | **160m** | **Band 160m** |
| Select 17m Band | **17m** | **Band 17m** |
| Select 20m Band | **20m** | **Band 20m** |
| Select 30m Band | **30m** | **Band 30m** |
| Select 40m Band | **40m** | **Band 40m** |
| Select 60m Band | **60m** | **Band 60m** |
| Select 6m Band | **6m** | **Band 6m** |
| Select 80m Band | **80m** | **Band 80m** |
| Select Panadapter 1 | **Select Panadapter 1** | **Select Panadapter 1** |
| Select Panadapter 2 | **Select Panadapter 2** | **Select Panadapter 2** |
| Select Panadapter 3 | **Select Panadapter 3** | **Select Panadapter 3** |
| Select Panadapter 4 | **Select Panadapter 4** | **Select Panadapter 4** |
| Select Panadapter 5 | **Select Panadapter 5** | **Select Panadapter 5** |
| Select Panadapter 6 | **Select Panadapter 6** | **Select Panadapter 6** |
| Select Panadapter 7 | **Select Panadapter 7** | **Select Panadapter 7** |
| Select Panadapter 8 | **Select Panadapter 8** | **Select Panadapter 8** |
| Select Slice A | **Select Slice A** | **Select Slice A** |
| Select Slice B | **Select Slice B** | **Select Slice B** |
| Select Slice C | **Select Slice C** | **Select Slice C** |
| Select Slice D | **Select Slice D** | **Select Slice D** |
| Slice details | — | Not available: no AetherSDR equivalent |
| Slice Lock | **Tune Lock Toggle** | **Tune Lock** |
| Slice Mute toggle | **Mute Toggle** | **Audio Mute** |
| Slice new | **New Slice** | **New Slice** |
| Slice next | **Next Slice** | **Next Slice** |
| Slice set active | **Next Slice** | **Next Slice** (or Select Slice A–H) |
| Slice set TX | **Make Active Slice TX** | **Make Active Slice TX** |
| Split toggle | **Split Toggle** | **Split Toggle** |
| Switch Keyer to Iambic-A | **CW Keyer: Iambic A** | **CW Keyer: Iambic A** |
| Switch Keyer to Iambic-B | **CW Keyer: Iambic B** | **CW Keyer: Iambic B** |
| Switch Keyer to Straight-Key | **CW Keyer: Straight Key** | **CW Keyer: Straight Key** |
| Trigger CW left Paddle | **Trigger CW Left Paddle** | **Trigger CW Left Paddle** |
| Trigger CW right Paddle | **Trigger CW Right Paddle** | **Trigger CW Right Paddle** |
| Trigger CW Straight Key | **Trigger straight key** | **Trigger straight key** |
| Tune Stack Down | **Band Stack Down** | **Band Stack Down** |
| Tune Stack Up | **Band Stack Up** | **Band Stack Up** |
| Tune step (1,10,100,1000) | **Step Size Up** | **Step Size Up** |
| Tune step Previous | **Step Size Down** | **Step Size Down** |
| TX Ant 1 | **TX Antenna 1** | **TX Antenna 1** |
| TX Ant 2 | **TX Antenna 2** | **TX Antenna 2** |
| TX XVTR | **TX Antenna 3** | **TX Antenna 3** (by position in the radio's antenna list) |
| View Command Bar | — | Not available: no AetherSDR equivalent |
| View CWX Panel | **Show/Hide CWX Panel** | **Show/Hide CWX Panel** |
| View DTMF Panel | — | Not available: no AetherSDR equivalent |
| View Macros Bar | — | Not available: no AetherSDR equivalent |
| View PTT | — | Not available: no AetherSDR equivalent |
| View Radio Controls Panel | — | Not available: no AetherSDR equivalent |
| View Rit/Xit Panel | — | Not available: no AetherSDR equivalent |
| View Stack Panel | — | Not available: no AetherSDR equivalent |
| VOX | **VOX Toggle** | **VOX Enable** |
| WNB | **WNB** | **WNB** |
| XIT | **XIT Toggle** | **XIT Enable** |
| XIT Down | **XIT Offset Down** | **XIT Offset** |
| XIT Up | **XIT Offset Up** | **XIT Offset** |
| Zoom in | **Panadapter Zoom In** | **Panadapter Zoom In** |
| Zoom out | **Panadapter Zoom Out** | **Panadapter Zoom Out** |

| SmartSDR wheel | Keyboard action | MIDI control |
|---|---|---|
| Mic gain | **Mic Level Up** | **Mic Level** |
| Adjust Main Volume | **Master Volume Up** | **Master Volume** |
| Slice Frequency (active) | **Tune Up (1 step)** | **VFO Tune Knob** |
| Bandwidth (active) | **Filter Widen** | **Filter Width** |
| CW speed | **CW Speed Up (+5 WPM)** | **CW Speed** |
| AGC Threshold (active) | **AGC-T Up** | **AGC Threshold** |
| RF Power | **RF Power Up** | **RF Power** |
| NB-Level (active) | **NB Level Up** | **NB Level** |
| Slice Volume (active) | **AF Gain Up** | **AF Gain** |
| WNB-Level (active) | **WNB Level Up** | **WNB Level** |
| APF value (active) | **APF Level Up** | **APF Level** |
| ANF Level (active) | **ANF Level Up** | **ANF Level** |
| NR-Level (active) | **NR Level Up** | **NR Level** |
| XIT Frequency (active) | **XIT Offset Up** | **XIT Offset** |
| RIT Frequency (active) | **RIT Offset Up** | **RIT Offset** |

</details>

### Settings files

MIDI configuration lives in its own files in the AetherSDR configuration folder (`~/.config/AetherSDR/` on Linux), separate from the main `AetherSDR.db` settings store:

```
midi.settings      — device preferences and active bindings
midi/<name>.xml    — saved profiles
```

Saves to `midi.settings` replace the file atomically. If writing the new
document fails, the previous file remains intact and AetherSDR logs a warning.
Changing **Auto-connect on startup** or connecting a device also leaves the
file untouched and logs a warning if the existing file cannot be read or
contains malformed XML. Restore read access or a valid backup before changing
those preferences again.

If `midi.settings` cannot be read or contains malformed XML when AetherSDR
starts, AetherSDR loads whatever bindings it could read, leaves the file
untouched, and saves no MIDI binding or preference changes for the rest of
the session. The MIDI Controller Mapping window says so when it opens.
Restore read access or a valid backup, then restart AetherSDR. If you have
no backup, deleting `midi.settings` lets AetherSDR start over with no bindings.
Export a profile to keep a separate backup of your bindings.

## Troubleshooting

### No MIDI ports shown

The controller is not recognized, or the port list is out of date.

1. Check that your controller is plugged in and recognized (on Linux, `amidi -l` should list it).
2. Click **Refresh** to re-scan. Selections follow the device, not its position in the list, so a refresh does not swap your port.

### A knob moves but the parameter doesn't change

The binding is missing or captured the wrong message, the radio is not connected, or the radio does not have that control.

1. Check the binding exists in the table and that the activity line shows the CC number you expect. If not, edit the binding with **✎** or re-learn it.
2. Make sure the radio is connected.
3. On a non-Flex radio, look for a notice saying the radio does not support that control.

### Values jump or are reversed

The control is upside down, or it is an endless encoder bound as absolute.

1. Tick **Invert** to reverse the direction.
2. If an endless encoder jumps between extremes, tick **Relative**.

### Two bindings share a source and only one works

When two bindings use the same MIDI source, only the last one works. AetherSDR warns you when this happens.

1. Click **✎** on one of the bindings and give it a different source, or delete it with **×**.

## See also

- [FlexControl Tuning Knob](./flexcontrol-tuning-knob.md)
- [USB Control Surfaces](./usb-control-surfaces.md)
- [Ulanzi Dial](./ulanzi-dial.md)
- [Keyboard Shortcuts](./keyboard-shortcuts.md)
- [Settings and Backups](./settings-and-backups.md)
