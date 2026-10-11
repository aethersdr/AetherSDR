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
- **SmartSDR+ subscription:** DVK requires an active SmartSDR+ subscription on the radio. See [SmartSDR+ licence](#smartsdr-licence).
- **Mode:** DVK plays on the **transmit slice**, so it is available when the TX slice is in a **voice mode** (USB, LSB, AM, SAM, FM, NFM, DFM). The indicator is greyed out in digital and CW modes, and switching to a non-voice mode closes the panel.

## Using the DVK panel

### Opening DVK

Click the **DVK** indicator in the status bar at the bottom of the screen. The panel opens beside the panadapter.

The DVK panel and the [CWX Panel](./cwx-panel.md) share the same space — opening one closes the other.

<img src="/img/screens/dvk-panel.png" width="370" alt="Digital Voice Keyer panel along the left edge of the window, status Idle. Twelve slots, F1 Recording 1 to F12 Recording 12, each read Empty. REC, STOP, PLAY and PREV buttons run along the bottom." />

*The DVK panel, opened from DVK in the status bar.*

### Recording

1. Click a slot to select it (blue highlight).
2. Click **● REC** to start recording (button turns red, progress bar pulses red).
3. Click **■ STOP** to end recording.
4. The radio stores the recording and updates the duration.

### Playback (on-air)

- Click **▶ PLAY** or press the **F-key button** to transmit the recording.
- The radio keys TX automatically during playback.
- The progress bar fills green in proportion to elapsed/total time.
- Status shows: `Status: Playback 1 / 3.2s`

### Preview (local)

- Click **◀ PREV** to hear the recording locally without transmitting.
- The progress bar fills blue.
- Status shows: `Status: Preview 1 / 2.1s`

### Managing slots

Right-click any slot row to access:

- **Rename…** — inline text editor (also available via double-click on name)
- **Clear** — erase the recording (greyed out for empty slots)
- **Delete** — remove the recording
- **Import WAV…** — upload a WAV file to this slot (see [WAV file requirements](#wav-file-requirements))
- **Export WAV…** — download the recording as a WAV file (greyed out for empty slots)

If an import or export fails, the destination is left intact — a failed export never leaves a half-written file over an existing one.

## Reference

### Recording slots

The panel shows 12 recording slots (F1–F12), each with:

- **F-key button** — click to toggle playback on-air
- **Recording name** — displayed next to the button
- **Duration** — shown on the right (or "Empty" if no recording)
- **Progress bar** — 3 px coloured bar during active operations

Empty slots appear dimmed. Slots with recordings show bright text.

### Keyboard shortcuts

| Key | Action |
|-----|--------|
| F1–F12 | Play that slot on-air; press again to stop. Works while the TX slice is in a voice mode, even with the panel closed. |
| Escape | Stop any active recording, playback, or preview (or cancel a rename) |

F1–F12 follow the transmit slice's mode: in voice modes they play DVK recordings, in CW they send CWX macros (see [CWX Panel](./cwx-panel.md)), and only one of the two responds.

### WAV file requirements

For importing WAV files, the radio requires:

- **Format:** 32-bit IEEE float
- **Channels:** 2 (stereo)
- **Sample rate:** 48,000 Hz
- **Max size:** 5 MB (~13 seconds)

Files that don't meet these requirements are rejected with a specific error message.

### Status bar indicator

The DVK label in the status bar has three visual states:

- **Bright cyan** — panel is open
- **Dim white** — available (voice mode), click to open
- **Dark grey** — unavailable (not in a voice mode, or not licensed)

### SmartSDR+ licence

When the radio reports that the feature is not licensed, the **DVK indicator itself is disabled** — greyed out with an arrow cursor and the tooltip "Digital Voice Keyer — requires an active SmartSDR+ subscription". The F1–F12 shortcuts and any open panel are gated the same way, and the panel status reads `Disabled (SmartSDR+ required)`.

AetherSDR only applies this once the radio positively reports the licence as disabled; if the radio never reports it, DVK stays available.

## Known issues

- Cancelling a WAV import or export and starting another straight away can corrupt the new transfer ([#5665](https://github.com/aethersdr/AetherSDR/issues/5665)).

## Troubleshooting

### The DVK indicator is greyed out

The transmit slice is not in a voice mode, or the radio reports no SmartSDR+ licence.

1. Switch the TX slice to a voice mode (USB, LSB, AM, SAM, FM, NFM or DFM).
2. Hover the indicator. If the tooltip says it requires an active SmartSDR+ subscription, renew SmartSDR+ on the radio.

### The DVK indicator is missing

The radio has no radio-side voice keyer, as on the [Hermes-Lite 2](./hermes-lite-2.md) and [Networked Icom](./networked-icom.md) radios. DVK is not available there.

### A WAV import is rejected

The file doesn't match the format the radio requires.

1. Convert the file to 2-channel, 32-bit IEEE float, 48 kHz.
2. Keep it under 5 MB (about 13 seconds).
3. Import it again with **Import WAV…**.

## See also

- [CWX Panel](./cwx-panel.md)
- [Keyboard Shortcuts](./keyboard-shortcuts.md)
- [Audio Settings](./audio-settings.md)
- [FlexRadio](./flexradio.md)
