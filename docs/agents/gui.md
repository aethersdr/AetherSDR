# Agent guide — GUI code

Part of [`AGENTS.md`](../../AGENTS.md). Read this before touching `src/gui/`:
MainWindow, dialogs, applets, meters, theme, accessibility, or the TX DSP
chain UI. Verifying the change? See [Automation bridge](#agent-automation-bridge--verify-the-gui-without-pixels).

## Adding code to MainWindow

`MainWindow` is one class split across `MainWindow.cpp` + a family of
`MainWindow_*.cpp` sibling TUs (#3351). Every sibling-TU function is a
`MainWindow::` member declared in `MainWindow.h`; the split is about *which
file* a body lives in.

**Do not add new feature code to `MainWindow.cpp`.** Route it by subsystem:

| Your change | Goes in |
|---|---|
| Feature lifecycle/handler fitting an existing subsystem | that subsystem's TU — demods (RADE/FreeDV/DAX/RTTY/WFM) → `MainWindow_DigitalModes.cpp`; physical controllers → `MainWindow_Controllers.cpp`; SWR sweep → `MainWindow_SwrSweep.cpp`; spot clients → `MainWindow_Spots.cpp`; discovery/connection/pan-lifecycle → `MainWindow_Session.cpp`; client-DSP applets → `MainWindow_DspApplets.cpp` |
| Wiring a newly-created radio object (slice/pan/VFO/DSP) to the UI | `MainWindow_Wiring.cpp` |
| A menu item / action | `MainWindow_Menus.cpp` |
| A keyboard shortcut | `MainWindow_Shortcuts.cpp` |
| A stateless helper with no `MainWindow` dependency | `MainWindowHelpers.{h,cpp}` |
| A whole new subsystem with no TU home | a **new** `MainWindow_<Subsystem>.cpp` sibling — only if it's a cohesive subsystem ~500+ lines; smaller waits in the closest sibling |
| A member field, or a guard inside a function that can't move | stays in `MainWindow.{h,cpp}` (keep minimal) |

A new TU is not free — every sibling re-parses the ~1,000-line `MainWindow.h`,
and any header edit rebuilds all of them. If tempted to subdivide one
subsystem into several thin TUs, extract a real class instead (the #3557
direction).

Sibling TUs must **carry their includes explicitly** — Qt reshuffles its
transitive includes between releases, and a distro Qt newer than the 6.12 pin
is a supported build, so a header that resolves transitively on one Qt need
not on another. When you move the last user of a header out of
`MainWindow.cpp`, drop that `#include` too.

Full map + decision guide:
[`docs/architecture/mainwindow-decomposition.md`](../architecture/mainwindow-decomposition.md).

## Adding or converting a dialog

See [`docs/style/dialog-patterns.md`](../style/dialog-patterns.md) before
writing or modifying a `QDialog`: the canonical lazy-construct + non-modal +
geometry-persist + frameless-chrome pattern, its pitfalls, and reference
dialogs (cleanup tracked in #2605, `PersistentDialog` base class).

A window the style guide covers is built on `CanonWindow` instead. It is a
documented exception to two rules below: it is always frameless, whatever
the `FramelessWindow` setting, and it centres itself on its parent on first
show (once it has a valid size), because it has no saved geometry to
restore. See the `CanonWindow` section of `dialog-patterns.md` and
[`docs/style/aethersdr-style-guide.md`](../style/aethersdr-style-guide.md).

Any new popout window, floating tool window, or `QDialog` must respect the
global `FramelessWindow` setting unless there is a specific reason not to:

- Add a `FramelessWindowTitleBar` at the top of the dialog/window layout.
- Install `FramelessResizer::install(this)` for resizable popouts.
- Add `setFramelessMode(bool on)` using the same pattern as
  `NetworkDiagnosticsDialog`: capture geometry, toggle
  `Qt::FramelessWindowHint`, restore geometry only if the window was already
  visible, show again only if it was already visible, and hide/show the custom
  title bar based on the setting.
- Initialize from `AppSettings::instance().value("FramelessWindow", "True")`.
- Do not use `QSettings`.

Do not manually move first-show dialogs to `(0,0)` or restore
constructor-time geometry. For first show, let Qt/window-manager placement
handle it, or match the closest existing dialog. If centering is explicitly
required, do it after the dialog has a valid size and say why.

## Theme and control availability

- **Every colour resolves through a ThemeManager token** (error/warning/
  success/notification/TX all have one); never hardcode a colour literal.
  Read [`docs/style/theme-style-guide.md`](../style/theme-style-guide.md)
  first. CI's hardcoded-colour ratchet fails a PR that raises the count above
  its base branch.
- **New or reworked surfaces follow the visual canon** —
  [`docs/style/aethersdr-style-guide.md`](../style/aethersdr-style-guide.md)
  (RFC #6226): the `color.canon.*` tokens, `CanonWindow`, the sparks. Apply
  it to UI your issue already changes; never restyle other screens on your
  own, and never change a base token's value in a migration PR.
- **Gating a control on a radio capability? Dim it, never hide it.** Controls
  render in one of three states — unavailable (the radio lacks it, dimmed
  **with a stated reason**), inactive (supported, not engaged), active. Hiding
  survives only at applet granularity for a cohesive radio-specific cluster.
  Register with `ControlAvailabilityRegistry` rather than writing another
  `setVisible()`; the reason must reach a screen reader via
  `accessibleDescription` (widgets) or `statusTip` (`QAction`s), because a
  tooltip is never announced. `tools/check_a11y.py` warns on a disabled
  control whose reason lives only in a tooltip. Doctrine:
  theme-style-guide §"Three-state controls" (#5262 M3a, #4896).

## Accessibility

Touching any file under `src/gui/`? Read [`docs/a11y.md`](../a11y.md)
**before** adding or modifying a widget. Canonical Qt patterns:
`setAccessibleName` / `setAccessibleDescription`, `QAccessibleValueChangeEvent`
on every value-change method, `QAccessibleInterface` subclass for any
`paintEvent` override, and the interactive-`QLabel` anti-pattern (replace with
`QPushButton` or add keyboard activation). `tools/check_a11y.py` runs on every
PR in `Static checks` and emits inline diff annotations; it is warning-only.

## Widget conventions

- **Meter smoothing — use `MeterSmoother`.** Every meter / level-bar / GR
  readout drives its display value through `MeterSmoother`
  (`src/gui/MeterSmoother.h`). Don't write new envelope-follower code.
- **User-facing names match the on-screen labels.** In prose (issue comments,
  README, What's-New strings, error toasts, support requests) call a control
  by the label the user sees — e.g. the **CAT applet** (class `CatControlApplet`),
  Help → Support & Diagnostics… → **Discovery**, **Connection / Commands**, **Protocol / Status**.
- **Region-aware band data — read from `BandPlanManager`, not `BandDefs.h`.**
  Band edges, segment sizes, and per-band metadata come from the active plan
  (`AppSettings["BandPlanName"]` + `resources/bandplans/`). `BandDefs.h::kBands[]`
  is ARRL/US-only; users span IARU regions 1/2/3.
- **TX DSP stages integrate with the CHAIN widget.** New TX DSP stages must be
  ordered, toggleable, and inspectable through the CHAIN widget rather than a
  parallel UI entry.
- **Contributors are recognised in the Contributor Logbook**
  (contributors.aethersdr.com), which the About window links to. The app
  carries no contributor list; don't add names to the About window.
- **PWR applet cross-needle geometry.** Before touching
  `CrossNeedleMeterGeometry`'s response model, SWR contours, or label
  placement, read
  [`docs/cross-needle-meter-math.md`](../cross-needle-meter-math.md).

## Agent Automation Bridge — verify the GUI without pixels

AetherSDR ships an in-process, agent-drivable bridge (off in production).
Launch with `AETHER_AUTOMATION=1` and drive a `QLocalServer` that speaks
newline-delimited JSON:

- `dumpTree` → semantic snapshot of the widget tree (objectName,
  accessibleName, enabled, geometry, live `value`, slider `range`).
- `grab <widget>` → PNG of any widget, including a correct GPU-framebuffer
  readback of the panadapter.
- `invoke <target> <action> [value]` → click/toggle/setValue/setText/… a
  control. **Refuses any control marked transmit-keying (`markTxKeying()` /
  `aetherTxKeying` — MOX/PTT, TUNE, ATU, CWX send, packet/APRS send) unless
  `AETHER_AUTOMATION_ALLOW_TX=1`**, and refuses disabled controls. Marked
  controls show `"keying": true` in `dumpTree`. Disambiguate duplicate names
  with a scoped target: `"RxApplet/AF gain"`.
- `get radio|transmit|equalizer|slice|slices|pan|pans [selector] [property]` →
  live JSON model snapshot.

Quick start: `python3 tools/automation_probe.py demo`. The same bridge is
exposed as an **MCP server** (Radio Setup toggle, token auth) with typed tools
and the same TX gate. Regenerate the verb→tool tables with
`tools/gen_bridge_docs.py` (a CI check fails on drift). Full reference:
[`docs/automation-bridge.md`](../automation-bridge.md) (issue #3646).
