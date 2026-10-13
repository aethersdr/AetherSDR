---
title: "DVK Panel"
slug: "/dvk-panel"
description: "The DVK (Digital Voice Keyer) panel enables recording and playback of up to 12 voice messages stored on the radio, commonly used for contesting and repetitive voice operations."
status: "Supported"
applies_to: ["FlexRadio"]
---

:::info[Status]

**Status:** Supported · **Applies to:** FlexRadio

:::

The DVK (Digital Voice Keyer) panel enables recording and playback of up to 12 voice messages stored on the radio, commonly used for contesting and repetitive voice operations. DVK is a FlexRadio feature and requires the radio's SmartSDR+ subscription.

## Requirements

- **Radio:** a FlexRadio Aurora, FLEX-8000 series or FLEX-6000 series radio. The DVK indicator is **hidden** on radios that have no radio-side voice keyer, such as the [Hermes-Lite 2](./hermes-lite-2.md) and [Networked Icom](./networked-icom.md) radios, and the panel closes with it.
- **SmartSDR+ subscription:** DVK requires an active SmartSDR+ subscription on the radio. See [SmartSDR+ license](#smartsdr-license).
- **Mode:** DVK plays on the **transmit slice**, so it is available when the TX slice is in a **voice mode** (USB, LSB, AM, SAM, FM, NFM, DFM). The indicator is grayed out in digital and CW modes, and switching to a non-voice mode closes the panel.

## Using the DVK panel

### Opening DVK

Click the **DVK** indicator in the status bar at the bottom of the screen. The panel opens beside the panadapter.

The DVK panel and the [CWX Panel](./cwx-panel.md) share the same space — opening one closes the other.

<img src="/img/screens/dvk-panel.png" width="370" alt="Digital Voice Keyer panel along the left edge of the window, status Idle. Twelve slots, F1 Recording 1 to F12 Recording 12, each read Empty. REC, STOP, PLAY and PREV buttons run along the bottom." />

*The DVK panel, opened from DVK in the status bar.*

### Recording

1. Click a slot to select it (the row takes the highlight).
2. Click **● REC** to start recording. The status line reads `Recording · slot 1 · 0.4s of 10.0s`; a slot holds up to 10 seconds.
3. Click **■ STOP** to end recording.
4. The radio stores the recording and updates the duration.

### Playback (on-air)

- Click **▶ PLAY** or press the **F-key button** to transmit the recording.
- The radio keys TX automatically during playback.
- PLAY, the playing slot's F-key and its progress bar take the MOX (transmit) colors, and the bar fills in proportion to elapsed/total time.
- The status line reads `On air · slot 3 · 0.4s of 4.2s`.

### Preview (local)

- Click **◀ PREV** to hear the recording locally without transmitting.
- The progress bar fills cyan.
- The status line reads `Previewing · slot 3 · 0.4s of 4.2s`.

Only one operation runs at a time. STOP is enabled only while something is running, PLAY and PREV are disabled on an empty slot, and while a recording, playback, preview or WAV transfer is in progress the other start controls stay disabled. The button of the running recording, playback or preview stays enabled, so clicking it again stops it. A refused operation shows in the status line as, for example, `Play failed (slot 3): DVK requires an active SmartSDR+ subscription`.

### Managing slots

Right-click any slot row to access:

- **Rename…** — inline text editor (also available via double-click on name). A name holds up to 61 UTF-8 bytes; quotes and `|` are removed. Enter saves, Escape cancels.
- **Clear** — erase the recording (grayed out for empty slots and while the DVK is busy). The radio decides whether the slot keeps its name.
- **Import WAV…** — upload a WAV file to this slot (see [WAV file requirements](#wav-file-requirements))
- **Export WAV…** — download the recording as a WAV file (grayed out for empty slots)

If an import or export fails, the destination is left intact — a failed export never leaves a half-written file over an existing one. After an import, **Import WAV…** and **Export WAV…** stay grayed out for about 3 seconds while the radio's file server finishes.

## Reference

### Recording slots

The panel shows 12 recording slots (F1–F12), each with:

- **F-key button** — click to toggle playback on-air
- **Recording name** — displayed next to the button
- **Duration** — shown on the right (or "Empty" if no recording)
- **Progress bar** — 3 px colored bar during active operations

Empty slots appear dimmed. Slots with recordings show bright text.

Screen readers announce each slot as, for example, "Slot 3: CQ Contest, 4.2s", and each F-key as "Play slot 3: CQ Contest".

### Keyboard shortcuts

| Key | Action |
|-----|--------|
| F1–F12 | Play that slot on-air; press again to stop. Works while the TX slice is in a voice mode, even with the panel closed. |
| Escape | Stop any active recording, playback, or preview (or cancel a rename) |

F1–F12 follow the transmit slice's mode: in voice modes they play DVK recordings, in CW they send CWX macros (see [CWX Panel](./cwx-panel.md)), and only one of the two responds.

### WAV file requirements

The DVK plays audio at 24 kHz mono. **Import WAV…** converts the file for you, so most WAV files import as they are:

- **Encoding:** PCM 8, 16, 24 or 32-bit, or 32-bit float (plain or WAVE_FORMAT_EXTENSIBLE)
- **Channels:** 1 to 8, mixed down to mono
- **Sample rate:** 8 kHz to 384 kHz, resampled to 24 kHz
- **Length:** at most 10 seconds

The file is sent to the radio as 24 kHz mono 16-bit PCM. Files that can't be converted are rejected with a specific error message, such as `Recording is 12.0 s long; a DVK slot holds at most 10 s`.

### Status bar indicator

The DVK label in the status bar has three visual states:

- **Bright cyan** — panel is open
- **Dim white** — available (voice mode), click to open
- **Dark gray** — unavailable (not in a voice mode, or not licensed)

The indicator can also be reached with the keyboard: Tab to it and press Enter.

### SmartSDR+ license

When the radio reports that the feature is not licensed, or refuses a DVK command or transfer with the SmartSDR+ error, the **DVK indicator itself is disabled** — grayed out with an arrow cursor and the tooltip "Digital Voice Keyer — requires an active SmartSDR+ subscription". The F1–F12 shortcuts and any open panel are gated the same way, and the panel status reads `Disabled · SmartSDR+ required`.

AetherSDR only applies this once the radio positively says no; if the radio never reports the license, DVK stays available. If the radio later reports the license as enabled, for example after a subscription is activated mid-session, DVK becomes available again without reconnecting.

## Troubleshooting

### The DVK indicator is grayed out

The transmit slice is not in a voice mode, or the radio reports no SmartSDR+ license.

1. Switch the TX slice to a voice mode (USB, LSB, AM, SAM, FM, NFM or DFM).
2. Hover the indicator. If the tooltip says it requires an active SmartSDR+ subscription, renew SmartSDR+ on the radio.

### The DVK indicator is missing

The radio has no radio-side voice keyer, as on the [Hermes-Lite 2](./hermes-lite-2.md) and [Networked Icom](./networked-icom.md) radios. DVK is not available there.

### A WAV import is rejected

The file can't be converted, is longer than 10 seconds, or the radio is busy.

1. Read the message in the panel's status line.
2. If it says the file is too long, trim it to 10 seconds or less.
3. If it names an unsupported encoding, save the file as PCM or 32-bit float WAV.
4. If it says the voice keyer or file server is busy, stop the running operation, wait a few seconds and try again.
5. Import it again with **Import WAV…**.

## See also

- [CWX Panel](./cwx-panel.md)
- [Keyboard Shortcuts](./keyboard-shortcuts.md)
- [Audio Settings](./audio-settings.md)
- [FlexRadio](./flexradio.md)
