# Canonical token taxonomy for AetherSDR theming (RFC #3076 Phase 2)

This document fixes the design-system token set that the Phase 2
stylesheet migration converts every hardcoded colour to.  Generated
from `tools/audit_colours.py` output (3621 references across 524
unique colours) by clustering close variants into a smaller canonical
set.

## Methodology

The audit produced 524 unique colours.  Frequency distribution:

| Count bucket | Unique colours |
|---|---|
| Used 50+ times | 10 |
| Used 20-49 times | 25 |
| Used 10-19 times | 21 |
| Used 5-9 times | 62 |
| Used 2-4 times | 195 |
| Used exactly once | 211 |

**Canonicalisation rule:** every colour within ΔRGB ≤ ~24 of a canonical
token's hex value collapses onto that token.  In practice this means
that close variants — for instance `#1a2a3a`, `#203040`, `#1e2e3e`,
`#2a3a4a`, `#1a2230`, `#2a3744` are all "blue-grey panel mid-tier" —
become a single `color.background.1` token.  The post-migration UI
will have sub-perceptual visual differences (most variants differ by
1-3 RGB units, well below the eye's discrimination threshold at the
GUI's display brightness).

Result: **42 canonical tokens** covering ~95% of references.  The 211
single-use colours snap to the nearest canonical neighbour.

## Token set

### Backgrounds (6 tiers)

| Token | Canonical | Refs (collapses) |
|---|---|---|
| `color.background.0` | `#0a0e14` | `#0a0a14` (46), `#0a0a18` (40), `#08121d` (23), `#0e1b28` (23), `#0f0f1a` (84 — spectrum), `#1a1a2e` (24), `#0a1420` (19) ≈ 259 refs |
| `color.background.1` | `#1a2a3a` | `#1a2a3a` (182), `#203040` (131), `#204060` (40), `#243a4e` (30), `#1e2e3e` (26), `#2a3a4a` (26), `#2a3744` (18), `#1a2230` (18), `#2a4458` (22) ≈ 493 refs |
| `color.background.2` | `#304050` | `#304050` (142), `#205070` (45), `#506070` (43), `#404858` (16), `#1a3a5a` (17), `#0070c0` (27 — selected) ≈ 290 refs |
| `color.background.3` | `#506070` | raised surface — popups, dialog headers |
| `color.background.tx` | `#3a2a0e` | TX-active panel tint: `#3a2a0e` (22), `#4a3a1e` (13) ≈ 35 refs |
| `color.background.spectrum` | `#000000` | spectrum/oscilloscope: `#000000` (29) — pure black, distinct from background.0 because the spectrum has its own canvas |

### Text (4 tiers)

| Token | Canonical | Refs (collapses) |
|---|---|---|
| `color.text.primary` | `#e6f0fa` | `#c8d8e8` (369 — most-referenced colour in codebase), `#d7e7f2` (29), `#d7e4f2` (12), `#d4deea` (9), `#e0f0ff` (6), `#ffffff` (94 — high-emphasis) ≈ 519 refs |
| `color.text.secondary` | `#8ea8c0` | `#8aa8c0` (132), `#8090a0` (57), `#7f93a5` (30), `#8d99ad` (20), `#a0b0c0` (11), `#a0b4c8` (7) ≈ 257 refs |
| `color.text.label` | `#506070` | `#808080` (47), `#607080` (16), `#6a8090` (15) ≈ 78 refs — dimmed labels, axis text |
| `color.text.disabled` | `#3a4a5a` | currently overlaps with label tier; will surface a distinct tone in default-dark v2 for accessibility |

### Accents (cyan family)

| Token | Canonical | Refs (collapses) |
|---|---|---|
| `color.accent` | `#00b4d8` | `#00b4d8` (170) — primary brand cyan, untouched |
| `color.accent.bright` | `#00c8f0` | `#00c8f0` (20), `#00c8ff` (15) ≈ 35 refs — hover / focused states |
| `color.accent.dim` | `#0070c0` | `#0090e0` (35), `#0070c0` (27), `#008ba8` (13), `#4db8d4` (17) ≈ 92 refs — desaturated cyan for non-primary surfaces; canonical value matches the legacy `kBlueActive` checked-button fill so tokenised active states render identically to pre-refactor sites |

### Status accents

| Token | Canonical | Refs (collapses) |
|---|---|---|
| `color.accent.success` | `#4dd87a` | `#00ff88` (39), `#00a060` (25), `#66d19e` (15), `#00e060` (14), `#30d050` (14), `#006040` (25 — background variant) ≈ 132 refs |
| `color.accent.warning` | `#ffb84d` | `#f2c14e` (81), `#ffd070` (9), `#e8a540` (9), `#e8b977` (7), `#ff8c00` (6), `#ffff00` (8) ≈ 120 refs |
| `color.accent.danger` | `#ff4d4d` | `#ff4444` (12), `#ff4040` (11), `#ff6b6b` (10), `#c03030` (9), `#ff8080` (8) ≈ 50 refs |

### Borders

| Token | Canonical | Refs |
|---|---|---|
| `color.border.subtle` | `#1a2330` | hairline panel separators (~80 refs collapsed) |
| `color.border.strong` | `#2a3a4d` | container borders, focused outlines (~60 refs) |
| `color.border.accent` | `#00b4d8` | active / selected border highlight |
| `color.border.tx` | `#5a4a28` | TX-active border (matches `background.tx` family) |

### Meter colours (specialised — paint code only)

| Token | Canonical | Refs |
|---|---|---|
| `color.meter.crst` | `#ff4d4d` | crest factor peak indicator |
| `color.meter.rms` | `#00b4d8` | RMS bar fill |
| `color.meter.thresh` | `#ffb84d` | threshold marker |
| `color.meter.peak` | `#e6f0fa` | peak-hold tick |
| `color.meter.gainReduction` | `#f2c14e` | GR meter bar |
| `color.meter.bar.fill` | `#405060` | meter bar inactive fill (~31 refs) |

### Spectrum / waterfall (specialised — paint code only)

| Token | Canonical | Notes |
|---|---|---|
| `color.spectrum.trace` | `#00b4d8` | live FFT trace |
| `color.spectrum.peakHold` | `#ffb84d` | peak-hold overlay |
| `color.spectrum.average` | `#8ea8c0` | averaged trace |
| `color.spectrum.grid` | `#1a2330` | dB/frequency grid lines |
| `color.waterfall.colormap` | (gradient — Phase 2 gradient support) | the 8-stop RF colormap |

### Slice indicators

| Token | Canonical | Notes |
|---|---|---|
| `color.slice.a`–`color.slice.h` | TBD per slice | one per slice letter; current code uses scattered per-slice colours that need a consolidated palette |
| `color.slice.tx` | `#ff4d4d` | active-TX slice highlight |

### Font tokens

| Token | Value | Notes |
|---|---|---|
| `font.family.ui` | `Inter` | |
| `font.family.mono` | `monospace` | |
| `font.family.segment7` | `DSEG7 Modern` | bundled (SIL OFL 1.1) via `third_party/dseg/`; all six weights loaded at startup from `:/fonts/DSEG7Modern-*.ttf` |
| `font.family.segment14` | `DSEG14 Modern` | bundled (SIL OFL 1.1) via `third_party/dseg/`; all six weights loaded at startup from `:/fonts/DSEG14Modern-*.ttf` |
| `font.family.weather` | `DSEGWeather` | bundled (SIL OFL 1.1) via `third_party/dseg/DSEGWeather/`; icon font keyed off ASCII letters. |
| `font.family.freq` | `DSEG7 Modern` | widget-class token — `VfoWidget` + `RxApplet` frequency labels read this so the operator can swap font families across both surfaces with one pick in the Theme Editor.  Defaults to `DSEG7 Modern` so a fresh install gets the segment look out of the box; reset to `Inter` or `monospace` for a plain proportional / fixed look. |
| `font.family.temp` | `DSEG7 Modern` | widget-class token — temperature readouts (band-conditions / propagation surfaces, future temperature widgets) read this so the operator can theme them independently from `font.family.freq`.  Same default as `freq` for visual consistency on a fresh install. |
| `font.size.tiny` | 9 | |
| `font.size.small` | 10 | |
| `font.size.normal` | 12 | |
| `font.size.large` | 14 | |

### Sizing tokens (unchanged from Phase 1 seed)

| Token | Value |
|---|---|
| `sizing.panel.padding` | 4 |
| `sizing.panel.spacing` | 4 |
| `sizing.panel.cornerRadius` | 4 |
| `sizing.border.subtle` | 1 |
| `sizing.border.strong` | 2 |

## Token count summary

- Backgrounds: **6**
- Text: **4**
- Accents: **6** (3 cyan family + 3 status)
- Borders: **4**
- Meters: **6**
- Spectrum / waterfall: **5** (4 scalar + 1 gradient)
- Slice: **9** (A–H + TX)
- Font: **6**
- Sizing: **5**

**Total: 51 tokens** — comfortably inside the 50-80 envelope from the RFC.

## Migration risks

- **Pure black for spectrum** (`color.background.spectrum = #000000`) is intentionally distinct from `color.background.0 = #0a0e14`.  Spectrum widgets paint a pure-black canvas for maximum contrast against the trace; collapsing them onto background.0 would make the spectrum harder to read.  Keep separate.
- **Warning family is wide** (`#f2c14e` to `#ffff00` to `#ff8c00`).  The canonical `#ffb84d` is a middle.  Sites using `#ffff00` (pure yellow) will shift visibly.  Audit those specific sites before conversion — if the visual change is unacceptable, add a `color.accent.warning.bright` token.
- **`color.text.disabled` currently overlaps with `color.text.label`** in the existing codebase (both are around #506070-ish).  Migration introduces a distinct token even though they look identical today, so Phase 4's Light theme can give them different contrast values without breaking the dark theme.
- **Per-slice colours need a separate small audit** before locking the slice taxonomy — current code has them scattered across SliceLabel, SliceModel, indicator paint code.

## Acceptance criteria for the migration

- All ~3621 colour references replaced with a canonical token lookup
- `default-dark.json` expanded from 24 to ~51 tokens
- Default Dark UI runs side-by-side with v26.5.3 and shows no perceptible visual diff except for the documented intentional shifts (warning bright yellow → middle yellow, etc.)
- Audit re-run on post-migration `src/` shows 0 hardcoded colours (or only documented exemptions like resource files)

## Next steps

1. **Lock the taxonomy** — review this doc, edits, agreement
2. **Expand `default-dark.json`** with the 51 tokens
3. **Pilot conversion on one shared stylesheet** — pick `src/gui/SliceLabel.h` or `src/gui/CommonStyles.h` to validate the mechanical conversion process
4. **Mass conversion in batches** — file-by-file, with diff-screenshot review
5. **Resolver records widget→token reverse-map** as conversions land (feeds Phase 5 inspector)
