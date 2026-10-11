---
title: "Accessibility"
slug: "/accessibility"
description: "AetherSDR is built to be usable with a screen reader and from the keyboard."
---

AetherSDR is built to be usable with a screen reader and from the keyboard. This page summarises what works today and how to get help when something doesn't.

## Requirements

AetherSDR uses the platform accessibility interfaces, so it works with the screen reader you already use:

| Platform | Screen readers |
|----------|---------------|
| macOS | VoiceOver |
| Windows | NVDA, Narrator, JAWS |
| Linux | Orca |

## Using a screen reader

What you can expect:

- **Every interactive control has a name** (and, where useful, a description), so a screen reader announces "Master volume" or "Dock applet panel left" rather than just "button".
- **Live values are announced.** The S-meter and the VFO frequency announce their values as they change. Announcements wait for a value to settle, so tuning across a band doesn't flood you with interim readings.
- **Custom-drawn meters report their values.** Meters and indicators that AetherSDR paints itself expose their current reading to the screen reader.
- **Status is given in words, not colour alone.** For example, each radio tab in the title bar states its connection state ("link lost" and so on) as text as well as with the coloured dot.
- **Unavailable controls say why.** A control your radio or build can't use is dimmed rather than hidden, and the reason is announced along with it (for example, a headphone control on a radio with no headphone output).
- **Notices are announced.** For example, pressing a shortcut key while keyboard shortcuts are off gives a "Keyboard shortcuts are off" notice that is announced as well as shown.

## Using the keyboard

- You can reach controls with Tab and Shift+Tab, including the tabs on the VFO flag (Audio, DSP, mode, X/RIT, DAX).
- The title bar's radio list ("+") is keyboard reachable, including its search box and per-radio actions.
- Operating shortcuts (tuning, MOX, PTT hold, band and mode changes) are off by default. Turn them on with **Settings → Keyboard Shortcuts**; see [Keyboard Shortcuts](./keyboard-shortcuts.md). With them off, keys go to the focused control.
- On Windows and Linux, Alt+letter opens each menu from the title bar's ☰ button. See [Menu Reference](./menu-reference.md).

## Reduced motion

Animated elements, such as those in the About window, hold still when your operating system asks for reduced motion (macOS *Reduce motion*, Windows animation effects, and the GNOME/KDE equivalents).

## Reporting a problem

Please report accessibility gaps. If something is silent, mislabelled or unreachable from the keyboard, use **Help → File an Issue...** and say which screen reader and platform you use. See [Support and Logging](./support-and-logging.md). Ongoing screen-reader work, including JAWS, is tracked in [issue #4896](https://github.com/aethersdr/AetherSDR/issues/4896).

## Known issues

- The WAVE waveform scope announces only its name, not its peak, RMS or clip readout ([#3959](https://github.com/aethersdr/AetherSDR/issues/3959)).

## Troubleshooting

### Space, the arrow keys or other operating shortcuts do nothing

Operating keyboard shortcuts are off by default, so keys go to the focused control instead.

1. Open **Settings → Keyboard Shortcuts** and turn them on.
2. See [Keyboard Shortcuts](./keyboard-shortcuts.md) for the full list.

### Animations still move

Your operating system is not asking for reduced motion.

1. Turn on macOS *Reduce motion*, turn off Windows animation effects, or turn on the GNOME/KDE equivalent.

## See also

- [Keyboard Shortcuts](./keyboard-shortcuts.md)
- [Menu Reference](./menu-reference.md)
- [Contributing Guide](./contributing-guide.md)
- [docs/a11y.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/a11y.md): accessibility patterns for contributors (accessible names, live value announcements, announced reasons for disabled controls, contrast)
