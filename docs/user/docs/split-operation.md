---
title: "Split Operation"
slug: "/split-operation"
description: "Split operation means listening on one frequency and transmitting on another, the usual way to work a DX station or a pile-up."
status: "Supported"
applies_to: ["FlexRadio"]
---

:::info[Status]

**Status:** Supported · **Applies to:** FlexRadio

:::

Split operation means listening on one frequency and transmitting on another,
the usual way to work a DX station or a pile-up. AetherSDR does it with two
slices: your receive slice and a transmit slice beside it on the same
panadapter.

## Using split

### Starting and ending a split

<img src="/img/screens/split-pair.png" width="760" alt="Two VFO flags on one panadapter. Slice A at 14.250.000 has a red SPLIT badge and a dimmed TX badge. Slice B at 14.255.000, 5 kHz up, has a SWAP badge, the red TX badge and a magenta B, with its own magenta marker line in the spectrum." />

*A split pair: the receive slice's SPLIT badge turns red and the new transmit slice shows SWAP and TX.*

1. On the slice you are listening with, click **SPLIT** on its VFO flag.
2. AetherSDR makes that slice the **receive** slice and creates a **transmit**
   slice on the same panadapter, **1 kHz up in CW** and **5 kHz up** in other
   modes. Transmit moves to the new slice.
3. The receive slice's SPLIT badge turns **red**. The transmit slice's badge
   becomes **SWAP**.

- **SWAP** exchanges the two frequencies without changing which slice
  transmits, so you can jump to the DX station's listening frequency and back.
- Click the red **SPLIT** badge again to end the split. The transmit slice is
  removed.

The two VFO panels of a split pair stay on opposite sides of the dual-VFO
layout so they never cover each other. The SPLIT/SWAP badges follow each
slice's actual role, so a diversity child slice is never mistaken for a split
partner (see [Diversity and ESC](./diversity-and-esc.md)).

### The badge menu

**Right-click either badge** (SPLIT or SWAP) for the controls that go with
split operation.

<img src="/img/screens/split-badge-menu.png" width="212" alt="SPLIT badge context menu: Split Up 1 kHz, Split Up 5 kHz, Split Up 10 kHz, Split QSY Option (submenu), Monitor TX (submenu), Nothing remembered yet and Forget remembered audio." />

*Right-clicking the SPLIT badge: preset offsets and the split options.*

#### Split Up 1 / 5 / 10 kHz

Moves the transmit slice that far above the receive slice. The receive
frequency stays where it is. With no split running, the same choice starts one
at that offset. **Split Up 1 kHz**, **Split Up 5 kHz** and **Split Up
10 kHz** are also keyboard shortcut actions (see [Keyboard Shortcuts](./keyboard-shortcuts.md)).

#### Monitor TX

Chooses what the **Monitor TX (Hold)** key does while you hold it:

- **Solo TX frequency** silences the receive slice so you hear only where you
  are about to transmit, like XFC, TF-SET or TXW on a conventional
  transceiver.
- **Hear both** makes both slices audible for the hold, the way a second
  receiver would.

Releasing the key puts both mutes back as they were. **Monitor TX (Hold)** is
**not bound by default**: set a key in **Settings → Configure Shortcuts...**
(and tick **Settings → Keyboard Shortcuts**, since operating shortcuts are off
by default), or put it on a FlexControl, RC-28 or HID controller, where it toggles instead
of holding. The menu shows the current binding, or "Not bound — set a key in
Keyboard Shortcuts".

#### Split QSY Option

**Split QSY Option ▸ Close split on QSY** (off by default) closes a split that
AetherSDR created when something outside AetherSDR moves the receive slice
(Slice A) by more than a threshold. The threshold is **1 to 200000 Hz,
default 2 kHz**.

This is for a logging program that sends CAT `FA` / `ZZFA` directly to the
radio through its own connection: when the logger QSYs you to a new spot, the
old split goes away instead of transmitting on a stale frequency.

- Echoes of AetherSDR's own tunes (its CAT, TCI, on-screen controls and SWAP)
  keep the split open.
- Moves at or below the threshold keep the split open and become the new
  reference point.
- Any other unmatched change to Slice A looks the same as the external CAT
  case and closes the split.
- Closing removes the transmit slice and returns TX to Slice A. It does not
  wait for you to finish transmitting.

#### Forget remembered audio

Clears the remembered audio arrangement (below). The menu also shows a summary
of what is remembered, for example "TX unmuted, TX left, TX 40 %, RX right".

### Remembered audio

The transmit slice starts muted. If you change that (unmute it, set its level,
pan it to one ear and the receive slice to the other), the next split you start
comes back the same way, and keeps doing so until you change it.

- Only the transmit slice's mute, level and pan are remembered, plus the
  receive slice's pan if you moved it.
- The receive slice's own volume and mute are never touched, and its original
  pan is restored when the split ends.
- Only your own edits teach it: the VFO flag, applets, keyboard and
  controllers. Changes arriving over CAT, TCI or from the radio do not.
- If you mute the transmit slice yourself before ending a split, the remembered
  arrangement is cleared, so muting it once returns to the original behaviour.
- Splits started by a logging program or another client over CAT, Hamlib or
  TCI are left alone.

### Split from logging and digital-mode software

- Split is reported **per CAT client**. Several WSJT-X instances, each on its
  own CAT port, can all use **Split Operation: Fake It** at once. See
  [CAT Control](./cat-control.md) and [WSJT-X Integration](./wsjt-x-integration.md).
- Over TCI, a client's explicit split request still selects the transmit
  slice, so satellite and cross-band split work. See [TCI Server](./tci-server.md).

### Controllers

FlexControl, AetherControl and other controller buttons can run **Split
Active Slice** and **Monitor TX Frequency** actions. See
[FlexControl Tuning Knob](./flexcontrol-tuning-knob.md).

On radios that cannot do split, split controls and controller bindings refuse
with a notice instead of silently doing nothing.

## Known issues

- After closing a split and switching to another slice on a different band, clicking that band's panadapter can retune Slice A instead of the slice you are on ([#2102](https://github.com/aethersdr/AetherSDR/issues/2102)).

## Troubleshooting

### Clicking SPLIT shows a notice and nothing happens

The connected radio can't do split, so AetherSDR refuses rather than doing nothing silently.

1. Use split on a FlexRadio (or in Demo mode). Other radio families don't offer it.

### The split closed on its own after my logger changed frequency

**Close split on QSY** is on, and something outside AetherSDR moved Slice A by more than the threshold.

1. Right-click the **SPLIT** or **SWAP** badge and open **Split QSY Option**.
2. Untick **Close split on QSY**, or raise **QSY change threshold to close split** in the same submenu.

### The Monitor TX key does nothing

**Monitor TX (Hold)** is not bound by default, and operating shortcuts are off by default.

1. Open **Settings → Configure Shortcuts...** and bind a key to **Monitor TX (Hold)**.
2. Turn on **Settings → Keyboard Shortcuts**.

### A new split's transmit slice starts unmuted or panned to one side

AetherSDR remembered how you set up the transmit slice's audio last time.

1. Right-click the **SPLIT** or **SWAP** badge and choose **Forget remembered audio**.
2. Or mute the transmit slice yourself before ending the split; that also clears the remembered arrangement.

## See also

- [Multi-Slice Operation](./multi-slice-operation.md)
- [VFO Widget](./vfo-widget.md)
- [TX Controls](./tx-controls.md)
- [Keyboard Shortcuts](./keyboard-shortcuts.md)
- [CAT Control](./cat-control.md)
- [WSJT-X Integration](./wsjt-x-integration.md)
