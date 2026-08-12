# AetherSDR Theme Style Guide

> **Read this before writing or changing any user-facing UI.** The
> rule it exists to serve is one sentence:
>
> **Every colour in AetherSDR resolves through a ThemeManager token.
> A contribution never introduces a new hardcoded colour literal.**

This guide is the semantic map from *what you are trying to say* on
screen (error, warning, success, notification, transmit, selection,
plain text…) to *the token that says it*. If you follow the map, your
UI is themeable for free, consistent with every existing screen, and
correct in both bundled themes. If you invent a colour instead, you
have created one more unique colour in a codebase that is actively
migrating its colours down onto the token set (#3184) — and a reviewer
will send you back here.

## 1. How colours work here

The theme system (RFC #3076) is two layers of named tokens, defined in
`resources/themes/default-dark.json` and `default-light.json` and
served by `AetherSDR::ThemeManager`:

- **Primitive palette** — a grey ramp (whose steps differ per theme:
  dark carries `gray.200`/`gray.850`, light carries
  `gray.100`/`gray.300`/`gray.750`), plus `color.blue.300/500/700`,
  `color.red.500`, `color.green.500`, `color.amber.500`. These are the
  raw pigments. **UI code never references primitives directly**; they
  exist so semantic tokens can share pigments consistently.
- **Semantic tokens** — `color.accent.danger`, `color.background.tx`,
  `color.text.secondary`, … Each names a *role*, not a colour. This is
  the layer UI code uses, and the layer this guide maps.

Tokens can also be **scoped**: the theme JSON can override a token for
a widget subtree (for example, the `applet/tx` scope turns the
checked-toggle accent red where the root theme has it blue — scope
paths are slash-joined, and a widget opts in by carrying the path in
its `themeContainer` property). The widget-aware accessors below
resolve scope automatically — prefer them inside applets.

### Consuming tokens

```cpp
auto& tm = AetherSDR::ThemeManager::instance();

// Painting code
painter.setPen(tm.color("color.accent.danger"));
painter.setBrush(tm.brush("color.background.1", rect()));  // gradient-safe

// Scope-aware (walks the widget's parent chain for applet overrides)
painter.setPen(tm.color(this, "color.accent.danger"));

// Stylesheets — {{token}} placeholders, re-applied on theme change
tm.applyStyleSheet(m_statusLabel,
    "QLabel { color: {{color.accent.danger}}; font-size: 10px; }");
```

A token the *loaded theme* omits resolves from the compiled-in seed
(`src/core/ThemeSeedGenerated.cpp`, generated — never hand-edited):
`loadThemeFromPath()` re-seeds it as a base layer before flattening the
JSON on top. A token that exists **nowhere** does not fall back — it
logs a warning and `color()` returns `Qt::transparent`, while a
`{{token}}` in a stylesheet resolves to an empty fragment that Qt
discards, leaving the widget looking unchanged. If a colour is missing
or refuses to change, check the log for `missing color token`.

## 2. The semantic map

When your UI needs to communicate one of these states, use exactly
these tokens. Do not approximate them with literals, and do not borrow
a token for a role it does not name.

### System states

| You are showing… | Token(s) |
|---|---|
| **Error / fault / danger / destructive action** | `color.accent.danger` (foreground); `color.button.danger.*.disabled` for a destructive button's disabled state — enabled-state button colours are not tokenised yet, so an enabled destructive button takes its colour from `color.accent.danger` |
| **Warning / caution / approaching a limit** | `color.accent.warning` (foreground); `color.background.warning` (panel tint); `color.toggle.warning.*` (checked warning toggles) |
| **Success / OK / enabled-and-healthy** | `color.accent.success` (foreground); `color.background.success` (panel tint); `color.toggle.success.*` (checked success toggles) |
| **Notification / message / attention ping** | `color.highlight.message`, with `color.highlight.fg` for text drawn on top |
| **Transmit-active** | `color.background.tx` + `color.border.tx` (panel), `color.highlight.tx` (marker), `color.slice.tx` (slice flag), `color.tx.mox.*` (MOX button) |
| **Receive / RX marker** | `color.highlight.rx` |
| **Selection / active / focused / brand accent** | `color.accent` (primary), `color.accent.bright` (hover/focus), `color.accent.dim` (checked fills, non-primary surfaces), `color.border.accent` |

### Surfaces, text, borders

| Role | Token(s) |
|---|---|
| Backgrounds, deepest → raised | `color.background.0` → `color.background.1` → `color.background.2` → `color.background.3`; `color.background.app` (main window); `color.background.spectrum` (display canvas) |
| Text, most → least emphasis | `color.text.primary` → `color.text.secondary` → `color.text.label` → `color.text.disabled` |
| Borders | `color.border.subtle` (hairlines), `color.border.strong` (containers), `color.border.accent` (active) |

### Control families

Shared widgets have per-state token families — reach for the family
before composing from the tables above. `color.toggle.*`,
`color.slider.*` and `color.knob.*` are complete; `color.button.*` is
not (see below):

- `color.button.*` — **disabled states only** today (`background`,
  `foreground`, `border` and their `danger.` counterparts, all
  `.disabled`); enabled-state button colours are not tokenised yet
- `color.toggle.*` (base + `.accent/.success/.warning` checked variants —
  see `docs/theming/toggle-button-tokens.md`)
- `color.slider.*` / `color.knob.*` (see
  `docs/theming/slider-knob-tokens.md`)
- `color.meter.*` (bars, peak/RMS/threshold marks)
- `color.spectrum.*`, `color.waterfall.*` (traces, grid, colormaps)
- `color.slice.a…h` + `color.slice.dim.*` (slice identity colours)

The full semantic-token inventory is greppable — 103 tokens, the same
set the bundled themes carry at root scope:

```bash
git grep -oh '"color\.[a-zA-Z0-9._]*"' src/core/ThemeSeedGenerated.cpp | sort -u
```

Keep the character class case-sensitive; a lowercase-only class
silently drops the four camelCase names. The design rationale and the
hex values each token canonicalised from live in
`docs/theming/canonical-tokens.md`.

## 3. Do not deviate

- **No new hex/RGB literals** in `src/` UI code — not in QSS strings,
  not in `QColor(...)`, not "just for this one label".
- **No near-miss variants.** `#1b2b3b` because `color.background.1`
  "felt one shade off" is exactly the drift this guide exists to stop;
  variants within ΔRGB ≤ 24 of a token are that token
  (`docs/theming/canonical-tokens.md` documents the collapse rule).
- **No borrowing across roles.** Amber is a warning, not a decoration;
  if you want an accent, that's `color.accent`.
- **Theme-edit, not code-edit.** If a token's *value* looks wrong,
  propose a change to the theme JSON — do not correct it locally with
  a literal.

## 4. Genuinely new UX: add a token, never a literal

New UX sometimes needs a colour role that does not exist yet — a new
meter zone, a new decoder state, a new overlay. That is the one
sanctioned way a new colour enters the codebase, and it enters as a
**token**, in the same PR as the UX:

1. Name it semantically in an existing namespace where possible
   (`color.meter.*`, `color.<feature>.*`), following the patterns
   above.
2. Add it to **both** `resources/themes/default-dark.json` and
   `default-light.json`. Prefer referencing an existing primitive
   (`{color.amber.500}`) over a fresh hex value — but check the alias
   resolves in **both** themes: the grey ramps differ, and an alias to a
   primitive a theme lacks is returned as the literal string
   (`ThemeManager::resolveAlias()`), yielding an invalid `QColor` with
   no warning. Nothing catches this for you — `gen_theme_seed.py` reads
   only `default-dark.json`.
3. Regenerate the compiled-in seed: `python tools/gen_theme_seed.py`
   (CI's `check_theme_seed` gate fails the PR if the seed and JSON
   drift).
4. Record it in `docs/theming/canonical-tokens.md` — and in this
   guide's map if it is a new *system* role.

A reviewer seeing a new token asks one question — "is this really a
new role?" — which is precisely the design conversation a new colour
deserves.

## 5. Colour-as-data (the exemptions)

A small set of colours are *data*, not theme, and are exempt from the
no-literals rule above:

- `CompactColorPicker.cpp` swatches — a colour picker's swatches are
  its content.
- The *definitions* of waterfall colormap palettes (the
  `color.waterfall.colormap.*` token values themselves).
- Canonical token default values in `src/core/ThemeManager.cpp` /
  `ThemeSeedGenerated.cpp` — those literals *are* the tokens.
- Brand assets (logos, icons with fixed brand colours).

**The exemption is not automatic in CI.** The ratchet in §6 counts every
hex literal in any file outside `COLOUR_ALLOWLIST`
(`tools/audit_colours.py`), which today lists only
`src/core/ThemeManager.cpp` and `ThemeSeedGenerated.cpp` — so adding one
swatch to `CompactColorPicker.cpp` fails the gate. If your colour is
genuinely data, add the FILE to `COLOUR_ALLOWLIST` with a reason in the
same PR.

If you believe you have found a new member of this list, say so in the
PR description rather than deciding silently.

## 6. Checking yourself

`tools/audit_colours.py` is the tool behind this guide — it inventories
hardcoded colours and suggests the token each one should map to:

```bash
python tools/audit_colours.py --src src --summary-only
```

CI runs the same tool as a **blocking** gate. The "Hardcoded-colour
ratchet" step in `.github/workflows/static-checks.yml` runs it with
`--compare-src <base>/src --strict`, which exits 1 if your branch raises
the unique-colour, total-reference or `setStyleSheet()` count above the
**tip of your base branch at CI time** — not above your merge base. To
see the same verdict before you push:

```bash
git worktree add /tmp/colour-base origin/main
python tools/audit_colours.py --src src \
    --compare-src /tmp/colour-base/src --summary-only --strict
```

Because the comparison is against `main`'s tip, a branch that has fallen
behind can fail on counts *`main` itself reduced*. If the gate reports a
rise you cannot find in your own diff, merge `main` and re-run before
hunting for it.

Conformance is an authoring-time responsibility: choose the token when
you write the code, and the gate has nothing to flag.

## See also

- `docs/theming/canonical-tokens.md` — the token taxonomy and the
  legacy-hex → token collapse tables (the migration map for #3184).
- `docs/style/applet-style-guide.md` — layout, typography, spacing,
  and widget conventions. Where its historical tables list raw hex
  values, the token map above is authoritative.
- `docs/style/dialog-patterns.md` — dialog behaviour conventions.
- `docs/a11y.md` — contrast and accessibility expectations.
