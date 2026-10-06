# AetherSDR Style Guide — the visual canon

> **Adding or changing UI?** This is the direction every new or reworked
> surface follows (RFC #6226, approved). Most of the app predates it.
> Apply it to the UI your issue already changes; **do not restyle other UI
> on your own** — screens move to the canon when an issue or RFC asks them
> to, never as a side effect of unrelated work (GOVERNANCE.md §AI
> Contributors). Colours still resolve through ThemeManager tokens exactly
> as [`theme-style-guide.md`](theme-style-guide.md) describes; this guide
> says which tokens and what the result should look like.

The canon exists so the app, aethersdr.com, the Contributor Logbook and the
agent dashboard read as one product. Visual design is reserved to the
maintainer; this document is that direction written down, so agents and
contributors can apply it without guessing.

The first implementation is the **About window** (#6227):
`src/gui/CanonWindow.{h,cpp}` and the About block in
`src/gui/MainWindow_Menus.cpp`. When in doubt, look at it.

## Principles

- **Light, not paint.** Depth comes from soft blooms, a faint sheen and
  glows on dark surfaces — not from bevels or heavy borders.
- **One accent, used as light.** Cyan and the brand gradient mark what is
  live, selected or clickable. **Gold is reserved for recognising people**
  (awards, the Contributor Logbook) and nothing else.
- **State is never colour alone.** Good / warn / crit always travel with a
  word, sign or icon (`docs/a11y.md`).
- **Transmit is the one loud state.** Red means TX and nothing else.
  Instrument faces (SmartMTR, the analog meter faces) keep their own
  palettes exactly.

## Foundations

Dark is the canon and the priority. These are the dark-theme values; they
live in the `color.canon.*` token group of
`resources/themes/default-dark.json`.

| Role | Token | Dark value |
|---|---|---|
| Ground (window background) | `color.canon.ground` | `#060b13` |
| Raised surface | `color.canon.raised` | `#0a121e` |
| Nested surface (cards inside a surface) | `color.canon.nested` | `#0e1a2a` |
| Control fill (buttons, fields) | `color.canon.control` | `#12233a` |
| Ink (primary text) | `color.canon.ink` | `#eaf2fb` |
| Ink-soft (body text, values) | `color.canon.inkSoft` | `#c4d4e8` |
| Muted (labels, secondary text) | `color.canon.muted` | `#8598b4` |
| Hairline | `color.canon.line` | `rgba(120,165,210,.12)` |
| Emphasised line | `color.canon.lineHi` | `rgba(120,190,230,.28)` |
| Accent (cyan) | `color.canon.cyan` | `#5de3ff` |
| Aqua (hover, active) | `color.canon.aqua` | `#8ef7e6` |
| Text on accent | `color.canon.onAccent` | `#041019` |
| Spark tip (cyan) | `color.canon.sparkHot` | `#e6fdff` |
| Recognition gold / its tip | `color.canon.sparkGold` / `sparkGoldHot` | `#ffd970` / `#fff6d8` |
| Blooms | `color.canon.bloom.blue` / `bloom.teal` | blue 16 % top-right, teal 10 % top-left |
| Grid | `color.canon.grid` | 64 px grid at 3.5 %, fading out |
| Brand gradient | `color.brand.gradient` | `#3aa7ff → #5de3ff → #8ef7e6` at 100° |

The theme JSON writes alpha colours in Qt's `#AARRGGBB` order
(`#1f78a5d2` is the hairline above).

**Light theme.** Every role exists in `default-light.json` too. All
light-mode values are AI-generated and not managed by humans; they are
chosen to meet the `docs/a11y.md` contrast floors, which
`tests/theme_manager_test.cpp` checks for the About window's text and focus
pairs in both themes.

**Type and geometry stay as they are.** Inter for UI, DSEG7 for instrument
readouts; 22 px buttons, 26 px combos, the 52 px title bar.

## Building blocks

| You want | Use |
|---|---|
| A window in the canon (no title bar, 16 px rounded corners, ambient ground, round close button, drag to move, Escape / ⌘W to close) | `CanonWindow`; put content in `bodyWidget()` |
| A logo or avatar with a contrast ring and a cyan spark | `SparkRing` |
| A recognition control (gold spark around a button) | `SparkBorder` |
| The "AetherSDR" wordmark ("Aether" in ink, "SDR" in the gradient) | `BrandMark::paintWordmark` |
| A primary action | `color.brand.gradient` fill with `color.canon.onAccent` text |
| A secondary control | `color.canon.control` fill, `color.canon.lineHi` border, `color.canon.cyan` text; `color.canon.nested` + `color.canon.aqua` on hover |
| A card of label / value rows | `color.canon.nested` fill, `color.canon.line` border, 12 px radius; keys in `muted`, values in `inkSoft` |

### `CanonWindow` is a deliberate exception to the dialog pattern

Canon windows are **frameless and translucent on every platform**. That is
a deliberate split from the main window, which follows the per-platform
policy in [`../architecture/window-chrome.md`](../architecture/window-chrome.md)
(#6198). A `CanonWindow` does not follow `View → Frameless Window`, does not
restore a saved geometry, and is not tracked by `trackPersistentDialog()`.
[`dialog-patterns.md`](dialog-patterns.md) §"The exception: `CanonWindow`
windows" lists what it keeps (the `FramelessMoveHelper` drag, Escape and
⌘W, `WA_DeleteOnClose` plus a `QPointer` so a second open raises it).
`FramelessWindowTitleBar` is deprecated for new windows; existing dialogs
move to `CanonWindow` one at a time, each in its own PR.

### Motion

The spark is the only animation in the canon: a point of light circling a
border, once every 7 s for the brand (cyan) and every 3 s for recognition
(gold). Sparks run only while visible, tick slowly while their window is not
exposed, and **hold still when the OS asks for reduced motion**
(`QAccessibilityHints::motionPreference`). Do not add other animation
without an RFC.

## Migrating a surface

The canon changes token *values*, not the theming mechanism. The base
tokens are shared aliases (`color.background.0` and
`color.background.app` both resolve to one primitive, which other tokens
use too), so **a base token cannot move one surface at a time**. The order
is fixed by the RFC's approval:

1. **Now:** surfaces use the `color.canon.*` group directly.
2. **One surface at a time, each in its own PR under an issue:** move that
   surface's widgets from the base tokens to `color.canon.*`.
3. **Once, at the end:** flip the base tokens to canon values in a single
   PR, after the surfaces have moved:

   | Base token | Becomes |
   |---|---|
   | `color.background.0` | ground |
   | `color.background.1` | control |
   | `color.text.primary` | ink-soft |
   | `color.text.secondary` | muted |
   | `color.border.subtle` / `strong` | hairline / emphasised line |
   | `color.accent` | cyan |
   | `color.accent.dim` | `#3aa7ff` |

**A migration PR never changes a base token's value.** Slice colours, meter
zones, TX / RX / dynamics scopes and spectrum black stay exactly as users
know them.

Every migration keeps the colour-audit ratchet (`tools/audit_colours.py`)
flat or lower. The inventory of existing literals and the base-token set is
[`../theming/canonical-tokens.md`](../theming/canonical-tokens.md).

## See also

- [`theme-style-guide.md`](theme-style-guide.md) — the semantic state map
  and the no-new-literals rule (still binding).
- [`dialog-patterns.md`](dialog-patterns.md) — persistent dialogs, and the
  `CanonWindow` exception.
- [`../theming/canonical-tokens.md`](../theming/canonical-tokens.md) — the
  base-token inventory the migration works through.
- [`../a11y.md`](../a11y.md) — contrast floors and the colour-alone rule.
- RFC #6226 and its approval comment for the rulings behind this document.
