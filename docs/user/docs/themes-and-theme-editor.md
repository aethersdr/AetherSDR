---
title: "Themes and Theme Editor"
slug: "/themes-and-theme-editor"
description: "Every colour, font and size in AetherSDR comes from the active theme."
---

Every colour, font and size in AetherSDR comes from the active **theme**. You can switch themes live, with no restart, and create your own in the built-in Theme Editor.

## Choosing a theme

**View → Theme** lists every installed theme, with a check mark on the active one:

<img src="/img/screens/default-light-theme.png" width="1600" alt="AetherSDR main window in the Default Light theme: a light title bar and status bar, with the panadapter, waterfall and applets." />

*The main window in the Default Light theme.*

- **Default Dark**: the standard look, and the default.
- **Default Light**: a light theme for bright rooms.
- Any themes you have created or imported.

The change applies immediately across the whole application.

## Using the Theme Editor

**View → Theme Editor…** opens the editor for the active theme. It is a normal window, so you can keep it open while you look at the result. The controls are listed under [Reference](#reference).

Edits are saved as you make them.

<img src="/img/screens/theme-editor.png" width="622" alt="Theme Editor window for Profile: Default Dark. A colour picker with Flat color and Gradient types, a hue strip, RGB and hex fields and recent colours fills the top, beside a gradient stops preview. Font controls sit below, then a Scope selector with Inspect and Reset to default, a token filter, and a list of colour tokens with their values, such as color.accent #00b4d8. Theme actions, Save As… and Close buttons run along the bottom." />

*The Theme Editor, editing the built-in Default Dark theme.*

### Built-in themes are protected

Default Dark and Default Light can't be changed, renamed or deleted. When you edit a built-in theme, the editor asks you to **Save As** a copy (suggested name "My &lt;theme name>") and your changes go there. Deleting the theme you are using switches back to Default Dark.

### Sharing theme files

- Exported themes use the `.aethertheme` extension (a JSON file). **Import from file…** accepts `.aethertheme` and `.json`, and you can also drag a theme file onto the editor window.
- Your own themes are stored in a `themes` folder inside the AetherSDR configuration folder (on Linux, `~/.config/AetherSDR/themes/`). A user theme with the same name as a built-in one is ignored, so use Save As with a new name.

## What a theme controls

Themes cover the whole interface, including:

- **Slice colours**: the default colours for slices A–H (see [Slice Colors](./slice-colors.md)).
- **Waterfall colour schemes**: the palettes chosen under the panadapter's **Display → Scheme** (see [Panadapter Controls](./panadapter-controls.md)). Switching theme or scheme recolours the waterfall already on screen, including scrollback.
- The 7- and 14-segment display fonts used for frequency readouts (the DSEG fonts, under the SIL Open Font License).

Analog meter faces have their own **Face theme** choice on each meter (see [Meters](./meters.md)).

## Reference

| Control | What it does |
|---------|-------------|
| **Scope:** | Which part of the interface you are editing. "(root)" is the whole application; a nested scope (for example one applet) applies your change only inside that container. |
| **🎯 Inspect** | Click it, then point at any part of the main window to find the tokens that paint it. Esc cancels. |
| **Filter tokens…** | Narrows the token list (for example *accent*, *slice*, *meter*). |
| Token editors | Change colours (with transparency), gradients, fonts and sizes. Changes show live. |
| **Clear override** | Puts one token back to the theme's value. |
| **Theme actions** | Rename theme…, Delete theme…, Export to file…, Import from file…. |
| **Save As…** | Saves the current edits as a new theme. |

Theme authors can find the token names and their meanings in the repository:

- [docs/theming/](https://github.com/aethersdr/AetherSDR/tree/main/docs/theming): the token reference
- [docs/style/theme-style-guide.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/style/theme-style-guide.md): how the interface uses tokens

Contributors writing new interface code take every colour from theme tokens rather than hard-coded values; see [Contributing Guide](./contributing-guide.md).

## Known issues

- The Theme Editor's token list can draw light text on white rows, so tokens such as `font.family.freq` are hard to read ([#5934](https://github.com/aethersdr/AetherSDR/issues/5934)).

## Troubleshooting

### Import says the theme matches a built-in theme name

The theme file's `"name"` field is Default Dark or Default Light. Built-in names are reserved, and a user theme with one of them is ignored.

1. Open the theme file in a text editor and change its `"name"` field to a new name.
2. Import it again with **Theme actions → Import from file…**.

### Your edits to Default Dark or Default Light went into a new theme

Built-in themes are protected, so the editor saved your changes to a copy named "My &lt;theme name>".

1. Pick the copy from **View → Theme** to use it.
2. Rename it with **Theme actions → Rename theme…** if you like.

## See also

- [Slice Colors](./slice-colors.md)
- [Panadapter Controls](./panadapter-controls.md)
- [Meters](./meters.md)
- [Accessibility](./accessibility.md)
- [docs/theming/](https://github.com/aethersdr/AetherSDR/tree/main/docs/theming)
