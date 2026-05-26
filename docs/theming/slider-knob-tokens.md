# Slider + knob token namespaces

First worked example of carving a control type out of the generic
theme categories (`color.accent` / `color.background.1` /
`color.text.primary`) into dedicated namespaces with per-applet
overrides through the v2 scope tree.  This document captures the
pattern so future passes (toggle buttons, spinboxes, progress bars,
…) follow the same shape.

## Namespaces

```
color.slider.background           ← groove + add-page (unfilled portion)
color.slider.foreground           ← sub-page (filled portion, the value indicator)
color.slider.handle               ← thumb
color.slider.background.disabled
color.slider.foreground.disabled
color.slider.handle.disabled

color.knob.background             ← outer ring / track
color.knob.foreground             ← indicator arc (value position)
color.knob.handle                 ← center pointer / tick
color.knob.background.disabled
color.knob.foreground.disabled
color.knob.handle.disabled
```

State suffix (`.disabled`) matches the existing
`color.text.disabled` / `color.accent.bright` pattern.  Future state
variants (`.hover`, `.pressed`) follow the same suffix slot when
they're needed.

## Cascade — root → applet → applet/&lt;name&gt;

The bundled themes seed sensible root-scope defaults aliased to the
primitives palette (`{color.blue.500}` etc.) for visual continuity
with the pre-namespace look:

```json
"color": {
  "slider": {
    "background":          "{color.gray.800}",
    "foreground":          "{color.blue.500}",
    "handle":              "{color.gray.200}",
    "background.disabled": "{color.gray.850}",
    "foreground.disabled": "{color.gray.600}",
    "handle.disabled":     "{color.gray.500}"
  }
}
```

Per-applet overrides live under `scopes.applet.scopes.<name>.tokens`:

```json
"scopes": {
  "applet": {
    "tokens": {},
    "scopes": {
      "tx":   { "tokens": { "color.slider.foreground": "{color.red.500}",
                            "color.knob.foreground":   "{color.red.500}" } },
      "rx":   { "tokens": { "color.slider.foreground": "{color.green.500}",
                            "color.knob.foreground":   "{color.green.500}" } },
      "comp": { "tokens": { "color.slider.foreground": "{color.amber.500}",
                            "color.knob.foreground":   "{color.amber.500}" } }
    }
  }
}
```

Result at runtime: a `QSlider` inside `TxApplet` walks
`applet/tx → applet → root`, finds the `{color.red.500}` override at
`applet/tx`, resolves to `#ff4d4d`.  A `QSlider` outside any applet
walks straight to root and resolves to `#00b4d8`.

## seedBuiltinDefaults — compile-time safety net

`ThemeManager::seedBuiltinDefaults()` also seeds:

1. Root-scope hex values for all 12 new tokens (raw hex, not aliases,
   so the seeds don't depend on the primitives palette being loaded).
2. Per-applet scope overrides (`applet/tx`, `applet/rx`,
   `applet/comp`) created via `scopeOrCreate()` with the
   red/green/amber foreground values.

This means **older user themes** (forked before this PR) that have
no slider/knob entries in their on-disk JSON still get the canonical
look + per-applet differentiation.  The bundled themes' JSON
re-asserts the same values via primitive aliases (idempotent).

## The QSS migration

Two surfaces in `gui/Theme.h`:

### Global QSS (`appStylesheetTemplate`)

Every slider rule (groove / sub-page / add-page / handle, horizontal
+ vertical, normal + disabled) reads from `color.slider.*`.  Applied
to the main window via `applyAppTheme()` — descendant sliders
inherit unless they have their own per-widget stylesheet.

### Per-slider helper (`applyPrimarySliderStyle`)

The canonical helper that ~20 call sites use.  Default `accentToken`
arg is `"color.slider.foreground"` — call sites that pass nothing
pick up the namespace automatically.  Sites that pass an explicit
token (e.g. `"color.accent.warning"` for TX-adjacent amber, or
`"color.slice.a"` for per-slice colour) still work — the explicit
override wins.

## The knob paint code (`ClientCompKnob`)

Knobs paint via custom `QPainter` calls, not QSS.  The colour helper
functions take a `const QWidget*` parameter so they can call the
widget-aware lookup:

```cpp
inline QColor kRingArc(const QWidget* w) {
    return ThemeManager::instance().color(w, "color.knob.foreground");
}
```

Passing `this` from `paintEvent` lets the widget-aware overload walk
the knob's container chain, so the `applet/comp` scope override
naturally reaches the rendered arc colour — no per-knob
`applyStyleSheet` plumbing needed.

## The ParentChange event filter (the subtle one)

**Symptom we hit:** sliders inside `TxApplet` still rendered blue
even though the editor confirmed the `applet/tx` scope override was
stored correctly.

**Root cause:** helpers like `applyPrimarySliderStyle` are commonly
called **before** the widget is added to its parent layout.
Example from `TxApplet::buildUI()`:

```cpp
m_rfPowerSlider = new GuardedSlider(Qt::Horizontal);   // no parent
m_rfPowerSlider->setRange(0, 100);
applyPrimarySliderStyle(m_rfPowerSlider);              // <-- resolved here
row->addWidget(m_rfPowerSlider, 1);                    // parent assigned here
```

`applyStyleSheet` resolves the template at apply time.  With no
parent yet, `containerPathFor` walks nothing and returns `""` (root
scope).  The QSS gets locked to root's blue, then the slider is
reparented to TxApplet — too late, the QSS was already resolved.

**Fix:** `ThemeManager` installs itself as an event filter on every
widget tracked through `applyStyleSheet`.  When the widget receives
`QEvent::ParentChange`, the template is re-resolved against the
now-correct scope chain and re-applied.  Cost: trivial — only fires
during initial construction (and rare deliberate reparents).

```cpp
bool ThemeManager::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::ParentChange) {
        QWidget* w = qobject_cast<QWidget*>(watched);
        if (w) {
            const auto it = m_trackedWidgets.constFind(w);
            if (it != m_trackedWidgets.constEnd()
                && !it.value().stylesheetTemplate.isEmpty()) {
                w->setStyleSheet(resolveFor(w, it.value().stylesheetTemplate));
            }
        }
    }
    return QObject::eventFilter(watched, event);
}
```

This is a general fix — every helper that configures a widget
pre-parent (`applyAppTheme`, `applyPrimarySliderStyle`, the dozens
of inline `applyStyleSheet` sites in ctor bodies) now works
correctly without any per-call-site change.

**Implication for future control-type carve-outs:** you don't need
to audit call sites for "is this slider attached to its parent yet?"
The filter handles it.

## Pattern for adding new control namespaces

When carving the next control type out (toggles, spinboxes, …):

1. **Define the namespace.**  Match the slider/knob shape:
   `color.<type>.background`, `.foreground`, `.handle`, each with a
   `.disabled` variant.
2. **Seed both layers.**
   - Add JSON entries to both bundled themes' root scope, aliased to
     primitives.
   - Add raw-hex root entries to `seedBuiltinDefaults()` for forked
     user themes that pre-date the new namespace.
3. **Migrate the QSS or paint code.**
   - QSS-styled controls (`QToolButton`, `QSpinBox`, …): swap the
     borrowed tokens in `Theme.h`'s `appStylesheetTemplate` and any
     per-call-site helpers.  The ParentChange filter handles the
     pre-reparent gotcha automatically.
   - Paint-code controls: change the colour helpers to take a
     `const QWidget*` and route through the widget-aware
     `tm.color(widget, token)` overload.
4. **Optional: per-applet overrides.**  Add nested-scope JSON
   entries under `scopes.applet.scopes.<name>` for the applets that
   should differ, and seed them in `seedBuiltinDefaults()` for
   pre-existing user themes.
5. **Document.**  Add a section to this directory referencing the
   pattern.

## Files touched in this PR

| File | Role |
|---|---|
| `resources/themes/default-dark.json` | Token defs + nested scope overrides |
| `resources/themes/default-light.json` | Same |
| `src/core/ThemeManager.h` / `.cpp` | seedBuiltinDefaults + ParentChange filter |
| `src/gui/Theme.h` | Global QSS + `applyPrimarySliderStyle` migration |
| `src/gui/TitleBar.cpp` | Master volume + headphone slider QSS |
| `src/gui/TxApplet.cpp` / `RxApplet.cpp` / `ClientCompApplet.cpp` | Applet-level QSS for un-helper'd sliders |
| `src/gui/ClientCompKnob.cpp` | Widget-aware paint-code migration |

## Out of scope (follow-ups)

- **`PhaseKnob` paint code** still uses 6 hardcoded `QColor` literals
  (no `tm.color()` lookups at all).  Migrating it needs more design
  judgment than a rename — separate sweep.
- **Hardcoded-hex slider sites** in `PanadapterApplet`,
  `EqApplet`, `RadioSetupDialog`, `FlexControlDialog` bypass the
  theme system entirely.  Each needs its local stylesheet dropped
  in favour of the global QSS + per-applet override path.
- **`color.knob.*` overrides at non-applet scopes** — e.g. if a
  knob inside a dialog should differ from the same knob inside an
  applet.  Available today (the namespace + scope tree both
  support it); nothing's been declared yet.
