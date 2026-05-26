# Toggle button token namespaces

Second control-type carve-out following the slider + knob pattern
established in [`slider-knob-tokens.md`](slider-knob-tokens.md).  Where
sliders have one canonical visual identity (track + fill), toggle
buttons carry *semantic* colour: a checkable button's "ON" state
communicates meaning — enable / activate / warning / generic-mode —
not just a value.  The carve-out therefore splits the checked-state
colours across three **tribes**; unchecked + disabled styling is
shared across tribes.

## Tribes

```cpp
enum class ToggleTribe { Accent, Success, Warning };
```

| Tribe | Semantic | Root-scope checked-bg | Per-applet override? |
|---|---|---|---|
| **Accent**  | Generic on / off, mode selectors | `color.accent.dim`         | yes — TX red / RX green / comp amber |
| **Success** | Enable, activate, connect        | `color.background.success` | no  — semantic colour is the point |
| **Warning** | Caution, armed, high-stakes      | `color.background.warning` | no  — semantic colour is the point |

Pick a tribe by what the toggled state *means*, not by what colour it
should be in any particular applet.  The Accent tribe surface-tints
by applet automatically through the scope tree; the Success and
Warning tribes look identical wherever they live.

## Namespaces

```
color.toggle.background           ← unchecked surface  (alias: color.background.1)
color.toggle.foreground           ← unchecked text     (alias: color.text.primary)
color.toggle.border               ← unchecked border   (alias: color.background.2)
color.toggle.background.disabled
color.toggle.foreground.disabled
color.toggle.border.disabled

color.toggle.accent.background.checked    ← color.accent.dim
color.toggle.accent.foreground.checked    ← color.accent
color.toggle.accent.border.checked        ← color.accent

color.toggle.success.background.checked   ← color.background.success
color.toggle.success.foreground.checked   ← color.accent.success
color.toggle.success.border.checked       ← color.accent.success

color.toggle.warning.background.checked   ← color.background.warning
color.toggle.warning.foreground.checked   ← color.accent.warning
color.toggle.warning.border.checked       ← color.accent.warning
```

The `color.background.warning` primitive is **new in this PR** —
introduced for tribe symmetry with the existing
`color.background.success`.  Dark theme value `#5a3a0a`, light theme
value `#f5e8d0`.

State suffix (`.disabled`, `.checked`) matches the slider + knob
convention so the editor's columnar view groups them naturally.
Future state variants (`.hover`, `.pressed`) follow the same suffix
slot when they're needed — deferred per the same design call that
deferred them on sliders.

## Cascade — root → applet → applet/&lt;name&gt; (Accent tribe only)

The bundled themes seed root-scope aliases that resolve to the
primitives palette so designers can retint either layer:

```json
"color": {
  "toggle": {
    "accent.background.checked": "{color.accent.dim}",
    ...
  }
}
```

Per-applet overrides live under `scopes.applet.scopes.<name>.tokens`
alongside the existing slider + knob overrides:

```json
"scopes": {
  "applet": {
    "scopes": {
      "tx":   { "tokens": { "color.toggle.accent.background.checked": "{color.red.500}" } },
      "rx":   { "tokens": { "color.toggle.accent.background.checked": "{color.green.500}" } },
      "comp": { "tokens": { "color.toggle.accent.background.checked": "{color.amber.500}" } }
    }
  }
}
```

Result at runtime: an Accent-tribe toggle inside `TxApplet` walks
`applet/tx → applet → root`, finds the `{color.red.500}` override at
`applet/tx`, resolves to `#ff4d4d`.  A toggle outside any applet
walks straight to root and resolves to `#0090e0` (`color.accent.dim`).
Success and Warning tribes don't have per-applet overrides — they
resolve straight to their root-scope value regardless of scope.

Only `background.checked` carries a per-applet override on first
pass — single-token override matches the slider precedent.  Future
refinement (overriding `foreground.checked` + `border.checked` to
maintain visual coherence with the per-applet background) is captured
in [Out of scope](#out-of-scope-follow-ups) below.

## seedBuiltinDefaults — compile-time safety net

`ThemeManager::seedBuiltinDefaults()` seeds:

1. Root-scope hex values for all 16 new tokens (raw hex, not aliases,
   so the seeds don't depend on the primitives palette being loaded).
2. Per-applet `applet/tx`, `applet/rx`, `applet/comp` overrides on
   `color.toggle.accent.background.checked` via `scopeOrCreate()`.

This means **older user themes** (forked before this PR) that have no
toggle entries in their on-disk JSON still get the canonical look +
per-applet differentiation.  The bundled themes' JSON re-asserts the
same values via primitive aliases (idempotent).

## The helper

[`src/gui/Theme.h`](../../src/gui/Theme.h):

```cpp
inline void applyToggleButtonStyle(QWidget* btn,
                                   ToggleTribe tribe = ToggleTribe::Accent);
```

Resolution is widget-aware — the helper routes through
`ThemeManager::applyStyleSheet`, which registers the widget for
free live re-theme on theme switches and resolves the template
against the widget's scope chain (so a button inside `TxApplet`
picks up the `applet/tx` override automatically without per-call-site
plumbing).

Sites that need site-specific styling (different padding, font size,
icon, etc.) can either:
- Call the helper, then `setStyleSheet()` an *additional* fragment for
  the local-only properties (Qt merges the rules), or
- Skip the helper and write a complete local stylesheet that
  references the `color.toggle.*` tokens directly — same templates,
  no enum.

## No change to the global QSS

Unlike sliders, **this PR does NOT add a `QPushButton:checked` rule to
`appStylesheetTemplate`**.  Every checkable button in the codebase
that currently has no explicit `:checked` styling would have suddenly
acquired the Accent-tribe look — a subtle but real visual regression
for ~30+ sites that aren't part of this sweep.  Opt-in via the
helper keeps the namespace landing isolated; the global QSS rule can
land in the follow-up after per-site auditing.

## Sites migrated in this PR

| File | Tribe | Notes |
|---|---|---|
| `src/gui/CatControlApplet.cpp` | Success | Both `m_enableBtn` (docked) and `m_floatingEnableBtn` (pop-out) — drops the inline `kGreenToggle` constant. |

The Success-tribe refactor is the proof site for the helper; the
per-applet Accent cascade is provable via Theme Editor inspection +
`theme_manager_test` assertions (`color.toggle.accent.background.checked`
resolves to blue at root, red at `applet/tx`, green at `applet/rx`,
amber at `applet/comp`).  The visual per-site demo lands in the
follow-up sweep.

## Out of scope (follow-ups)

- **Per-site QSS migration** — `AetherDspWidget`, `AetherialAudioStrip`,
  `AntennaGeniusApplet`, `AppletPanel`, `ClientChainApplet`,
  `ClientCompApplet` (the bypass toggle), `ClientCompEditor`, etc.
  Each currently has its own inline `QPushButton:checked` stylesheet
  — each needs its local stylesheet dropped in favour of the helper.
  Multiple sites per file, mostly mechanical but visual judgement on
  which tribe each toggle belongs to.
- **Indicator-style toggles** — `QCheckBox::indicator` /
  `QRadioButton::indicator` (small box / circle) are a different
  visual primitive and get their own namespace + sweep.
- **Hover / pressed state tokens** — same deferral as sliders.  Add
  `color.toggle.<tribe>.background.hover` etc. when a redesign needs
  them.
- **Global `QPushButton:checked` rule** in `appStylesheetTemplate` —
  needs per-site auditing of every existing checkable button to be
  sure the new default doesn't disturb buttons that are intentionally
  visually-subtle when checked.
- **Multi-token per-applet overrides** — `applet/tx` overrides only
  `color.toggle.accent.background.checked`; `foreground.checked` and
  `border.checked` stay at the root-scope blue, which on a red
  background reads visually inconsistent.  Adding `red.700`, `green.700`,
  `amber.700` primitives (or per-applet overrides of all three checked
  tokens) is a follow-up design decision.
- **`color.toggle.panel.*` tribe** — `AppletPanel.cpp`'s deep blue
  `#0a3060` for tab-like toggles doesn't fit Accent / Success / Warning
  cleanly.  A fourth `panel` or `muted` tribe may earn its keep in a
  later sweep.

## Pattern for the next control-type carve-out

When the next carve-out lands (spinboxes, progress bars, combo
boxes…):

1. **Decide whether the control carries semantic colour.**  If yes,
   tribes (this PR).  If no, single namespace + per-applet override
   (slider + knob PR).
2. **Match the namespace shape.**  Base tokens + state suffixes,
   with `<tribe>.<property>.<state>` if tribed.
3. **Seed both layers** (bundled JSON + `seedBuiltinDefaults`) so
   user themes forked pre-PR still render correctly.
4. **Helper in `Theme.h`** with the tribe enum (if applicable),
   widget-aware resolution via `applyStyleSheet`.
5. **Document this directory** with the namespace, cascade, helper
   signature, and migration follow-ups.
6. **Skip the global QSS rule** if the control has many existing
   sites with intentionally subtle / absent state styling — opt-in
   via the helper is safer.

## Files touched in this PR

| File | Role |
|---|---|
| `resources/themes/default-dark.json`  | Token defs + applet-scope overrides + new `color.background.warning` primitive |
| `resources/themes/default-light.json` | Same |
| `src/core/ThemeManager.cpp`           | seedBuiltinDefaults extension (16 tokens + 3 applet-scope overrides) |
| `src/gui/Theme.h`                     | `ToggleTribe` enum + `applyToggleButtonStyle` helper + template |
| `src/gui/CatControlApplet.cpp`        | Drop inline `kGreenToggle`, route through helper (Success tribe) |
| `tests/theme_manager_test.cpp`        | Toggle token + per-applet cascade assertions |
| `docs/theming/toggle-button-tokens.md`| This document |
