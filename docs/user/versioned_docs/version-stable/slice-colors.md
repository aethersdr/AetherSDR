---
title: "Slice Colors"
slug: "/slice-colors"
description: "Each slice (A–H) has its own colour, used for its frequency marker, filter passband and badges on the panadapter, so you can tell slices apart at a glance."
---

Each slice (A–H) has its own colour, used for its frequency marker, filter passband and badges on the panadapter, so you can tell slices apart at a glance. You can keep the theme's colours or choose your own.

## Using custom slice colours

Open **Settings → Radio Setup... → Appearance & Behavior** and find the **Slice Colors** group:

<img src="/img/screens/radio-setup-appearance.png" width="679" alt="Appearance &amp; Behavior page. Slice Letter Display offers Global slot index (selected) or Radio-assigned letter with global subscript. Slice Colors offers Use Aether defaults or Custom colors (selected), with eight colour buttons A to H in cyan, magenta, green, yellow, orange, teal, pink and purple and a Reset All to Defaults button. Further down, Single-click delay is 400 ms with a Reset button, and Mouse wheel has a Reverse mouse-wheel tuning direction checkbox." />

*Radio Setup, Appearance & Behavior page with Custom colors chosen, so each slice letter's colour can be changed.*

1. Choose **Custom colors** (instead of **Use Aether defaults**).
2. Click a slice button (**A**–**H**) to open a colour picker, and choose a colour.
3. The change applies immediately everywhere that slice is drawn.

The dimmed version of a custom colour is worked out automatically (about 40 % of its brightness). **Reset All to Defaults** puts every slot back to the theme's colours. Switching back to **Use Aether defaults** keeps your custom choices stored, so you can switch between the two.

Custom colours are saved in the settings database (`AetherSDR.db`) under `SliceColorsUseCustom` and `SliceColor0`–`SliceColor7`. See [Settings and Backups](./settings-and-backups.md).

## Reference

### Default colours

By default the slice colours come from the active theme (see [Themes and Theme Editor](./themes-and-theme-editor.md)). In **Default Dark**:

| Slice | Colour |
|-------|--------|
| **A** | Cyan |
| **B** | Magenta |
| **C** | Green |
| **D** | Yellow |
| **E** | Orange |
| **F** | Teal |
| **G** | Pink |
| **H** | Purple |

**Default Light** uses darker shades of the same set so they stay readable on a light background. Each colour also has a dimmed variant for inactive slices.

### Where the colour shows up

The slice colour is used for the slice marker and filter band on the spectrum, the slice's VFO flag and badges, and the slice indicators in the RX applet.

### Theme authors

The defaults are the theme tokens `color.slice.a` … `color.slice.h` and `color.slice.dim.a` … `color.slice.dim.h`, so a custom theme can supply its own palette. Custom colours chosen in Radio Setup override the theme.

## Troubleshooting

### Changing the theme doesn't change the slice colours

Custom colours chosen in Radio Setup override the theme.

1. Open **Settings → Radio Setup... → Appearance & Behavior**.
2. In **Slice Colors**, choose **Use Aether defaults**. Your custom choices stay stored, so you can switch back later.
3. To clear your custom choices instead, click **Reset All to Defaults**.

## See also

- [Themes and Theme Editor](./themes-and-theme-editor.md)
- [VFO Widget](./vfo-widget.md)
- [Multi-Slice Operation](./multi-slice-operation.md)
- [Settings and Backups](./settings-and-backups.md)
