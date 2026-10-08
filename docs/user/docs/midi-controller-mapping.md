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
2. Open **Settings → MIDI Mapping...**.
3. Click **Refresh**, pick your controller from the **Port** list and click **Connect** — the status turns green.
4. Move a knob or fader — the activity line shows the MIDI message.
5. Choose a parameter (for example "AF Gain").
6. Click **Learn**, then move the control you want to assign.
7. The binding appears in the table — that control now drives AF Gain.

## Using the mapping dialog

**MIDI Device**

<img src="/img/screens/midi-mapping-dialog.png" width="700" alt="MIDI Controller Mapping window. The MIDI Device group has a Port selector, Refresh and Connect buttons, a Disconnected status and Auto-connect on startup ticked. The Parameter Bindings table, with columns Parameter, MIDI Source, Channel, Invert and Relative, is empty. Below are a category filter, a parameter selector set to [RX] AF Gain with Learn and Manual… buttons, and Clear All, Profile, Save, Load, Import… and Export… controls, with Close at the bottom." />

*The MIDI Controller Mapping window, before any control is bound.*

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

### Manual entry

Click **Manual…** to type a binding instead of learning it: **Channel** (Any or 1–16), **Type** (Note On, Control Change (CC) or Pitch Bend), **Number** (note or CC number), plus **Invert value range** and **Relative (knob sends deltas)**.

The **✎** button on any row opens the same form filled in, so you can fix a binding that Learn captured wrongly without re-learning it. If two bindings share the same source, AetherSDR warns you — only the last one would work.

## Using relative encoders

Endless encoders send small "turned up / turned down" deltas instead of an absolute position. Tick **Relative** on the binding. AetherSDR recognises both common relative encodings (two's-complement 1/127 and centre-64 65/63) from the first detents and re-checks if the values stop making sense, so a mis-detected knob corrects itself. Fast turns are smoothed so a burst of deltas does not overshoot.

The **VFO Tune Knob** parameter is meant for an endless encoder bound as **Relative**; as an absolute control it treats 64 as centre.

## Using profiles

Save your bindings as named profiles to switch between controllers or layouts.

- **Save** — type a name in the **Profile** box and click **Save**. The dialog reports whether it saved or overwrote a profile.
- **Load** — pick a profile and click **Load** to apply it to the current bindings.
- **Import...** — load a profile file into your profile list. It accepts AetherSDR profile XML or a **SmartSDR `.map` file** (the per-controller maps vendors publish for SmartSDR for iOS/Mac); the format is detected from the content, and SmartSDR band and mode selections are translated. A summary lists what was imported and anything skipped — controls with no AetherSDR equivalent, invalid values and duplicates — so nothing is dropped silently. A name that already exists gets a " (2)" suffix instead of being overwritten. Click **Load** to apply the imported profile.
- **Export...** — write the current bindings to an AetherSDR profile XML (suggested name `AetherSDR_MidiProfile_<date>_<time>_v<version>.xml`) that **Import** reads back.

## Using MIDI with non-Flex radios

On Hermes-Lite 2, Icom, ANAN-G2 and RTL-SDR, bindings for features the radio does not have (for example CWL, NR/ANF on, split or CWX) refuse with a warning and a once-per-session notice instead of silently doing nothing. On these radios **NR Cycle** goes straight to NR2.

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

#### RX
| Parameter | Type | Range |
|-----------|------|-------|
| AF Gain | Slider | 0–200 |
| Squelch Level | Slider | 0–100 |
| AGC Threshold | Slider | 0–100 |
| Audio Pan | Slider | 0–100 |
| VFO Tune Knob | Slider | 0–127 (relative, centre 64) |
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
| QSO Record, QSO Playback | Toggle | on/off |
| Band Up, Band Down | Trigger | momentary |
| Mode Up, Mode Down | Trigger | momentary |
| Next Slice, Previous Slice | Trigger | momentary |

#### Mode, Band, Filter, Slice, Display, Frequency
| Category | Parameters (all triggers) |
|----------|---------------------------|
| Mode | Mode USB, LSB, CW, CWL, AM, SAM, FM, NFM, DFM, DSTR, DIGU, DIGL, RTTY |
| Band | Band 160m, 80m, 60m, 40m, 30m, 20m, 17m, 15m, 12m, 10m, 6m, 2m |
| Filter | Filter Widen, Filter Narrow |
| Slice | Split Toggle |
| Display | Band Zoom, Segment Zoom, Panadapter Zoom In, Panadapter Zoom Out, Open Memories |
| Frequency | Tune Up 1 MHz, Tune Down 1 MHz |

Digital-voice modes that need a helper (such as DSTR) are only offered when that helper is available, and **Mode Up / Mode Down** skip DSTR while the D-STAR helper is not running.

### Settings files

MIDI configuration lives in its own files in the AetherSDR configuration folder (`~/.config/AetherSDR/` on Linux), separate from the main `AetherSDR.db` settings store:

```
midi.settings      — device preferences and active bindings
midi/<name>.xml    — saved profiles
```

## Known issues

- A crash or a failed write while bindings are being saved can leave `midi.settings` partly written or empty, losing your live bindings without an error ([#5160](https://github.com/aethersdr/AetherSDR/issues/5160)). Export a profile as a backup.

## Troubleshooting

### No MIDI ports shown

The controller is not recognised, or the port list is out of date.

1. Check that your controller is plugged in and recognised (on Linux, `amidi -l` should list it).
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
