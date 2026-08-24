# Workspace canvas token namespace

The workspace canvas (RFC #4887) carves its two painted surfaces out of the
generic categories into a dedicated `color.canvas.*` namespace, following the
pattern `slider-knob-tokens.md` established for controls.

## Namespaces

```
color.canvas.background     ← the canvas surface, wherever items leave it exposed
color.canvas.dots           ← the 1 px snap-grid intersection dots
```

## Defaults and their derivation

| Token | Default Dark | Default Light | Derivation |
|---|---|---|---|
| `color.canvas.background` | `#08080d` | `#7b7b7c` | `color.background.app` darkened 50% (each channel halved: `#0f0f1a` → `#08080d`, `#f5f5f8` → `#7b7b7c`), so exposed canvas reads as a distinct working surface below the app background |
| `color.canvas.dots` | `#50e6f0fa` | `#501a2a3a` | the theme's primary text colour at ~31% alpha (`0x50`), stored as `#AARRGGBB` |

Both are literals rather than `{alias}` references: the background is a
*derived* value with no matching primitive, and the dots need baked-in alpha,
which alias references cannot add.

## How they are consumed

`WorkspaceCanvas` paints both with raw `QPainter` via
`ThemeManager::color(widget, token)` — not `applyStyleSheet()` — and
re-paints on `themeChanged`. The dots token is used **verbatim by both dot
passes** (the always-on background pass and the drag-time overlay pass), so a
theme controls the dots with one value; there is no per-pass alpha adjustment
in code.

## Fallback

Both tokens are in the generated seed (`tools/gen_theme_seed.py`), so a user
theme written before this namespace resolves to the Default Dark values
rather than transparent — the same degradation the slider and toggle
namespaces chose (#3184 established why transparent-on-missing is the worst
outcome).

## Not yet tokenised

The selection frame, grips, and snap guides still paint from
`QPalette::highlight()`; the item title bars keep their existing container
styling. Carving those out is a follow-up pass in this document's namespace
when the canvas chrome gets its theme treatment.
