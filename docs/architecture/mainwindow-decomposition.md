# MainWindow decomposition — where new code goes

`MainWindow` used to be a ~19,500-line monolith (`src/gui/MainWindow.cpp`).
Issue **#3351** split it into one class spread across many translation units
(TUs): `MainWindow.cpp` plus a family of `MainWindow_*.cpp` siblings. **It is
still one `MainWindow` class** — the split is purely about *which file* a method
body lives in, not about class boundaries. Every sibling-TU function is a
`MainWindow::` member declared in `MainWindow.h`.

Read this before adding code to anything named `MainWindow*`. The one rule:

> **`MainWindow.cpp` is not the default home for new feature code.** New
> feature lifecycle/handlers go in the matching sibling TU; per-object signal
> wiring goes in `MainWindow_Wiring.cpp`. `MainWindow.{h,cpp}` is reserved for
> genuinely cross-cutting code.

## The TU map

| File | Phase | Holds |
|---|---|---|
| `MainWindow.cpp` / `MainWindow.h` | — | The core: constructor (now mostly `wireXxx()` calls), central state members, the signal-routing hub, and cross-cutting code that belongs to no single feature. |
| `MainWindow_Controllers.cpp` | 1a | Physical-controller subsystems: FlexControl, HID encoders (RC-28 / TMate 2 / Ulanzi / PowerMate / Shuttle), StreamDeck labels, controller + meter wiring. |
| `MainWindow_Menus.cpp` | 1b | `buildMenuBar()` — every `QMenu`/`QAction`, their enable/disable wiring, and the inline lambdas they trigger. |
| `MainWindow_Shortcuts.cpp` | 1c | The keyboard-shortcut system + its shared state accessors. |
| `MainWindow_Wiring.cpp` | 1d | Per-object signal wiring: the `wirePanadapter()` / `wireSlice()` / `wireVfoWidget()` / `wireDsp…()` methods that connect each dynamically-created radio object to the UI. |
| `MainWindow_DigitalModes.cpp` | 1e | Demod / mode subsystems and their activate/deactivate lifecycles: RADE, FreeDV, DAX, AX.25 / KISS TNC, RTTY, **WFM**. |
| `MainWindow_SwrSweep.cpp` | 1e | The AetherSweep SWR-sweep engine (lock → step → state machine → pan overlay). |
| `MainWindow_Spots.cpp` | 2b | `wireSpotSubsystem()` — DX Cluster / RBN / WSJT-X / SpotCollector / POTA clients and their UI plumbing. |
| `MainWindow_Session.cpp` | 2c | `wireDiscovery()` / `wireRadioModel()` / `wirePanLifecycle()` — LAN + SmartLink discovery, heartbeat/disconnect detection, connection-state routing, and pan-stream lifecycle: the wiring that constitutes "a connected radio". Seed of the future `RadioSession` aggregate (#3445). |
| `MainWindow_DspApplets.cpp` | 2d | `wirePooDooTiles()` + `wireDspApplets()` — PooDoo RX status tiles and the client-DSP applet family (Compressor / Gate / De-esser / Tube / Reverb / AetherDSP / PUDU TX+RX) plus TX signal-chain wiring. |
| `MainWindowHelpers.{h,cpp}` | 0 | Stateless formatters / value transforms with **no** `MainWindow` dependency (tooltip builders, spot-ID math, client-list parsing, small pixmap painters). |
| `MainWindowShortcutState.h` | 1b | Internal shared shortcut state — **not** a public API; only `MainWindow*.cpp` TUs include it. |

## Decision guide — "where does my change go?"

| Your change | Goes in |
|---|---|
| A new feature's activate/deactivate or event handler that fits an existing subsystem above | That subsystem's TU (e.g. a new demod → `MainWindow_DigitalModes.cpp`, next to RADE/WFM) |
| Wiring a newly-created slice / pan / VFO / DSP *widget* to the UI | `MainWindow_Wiring.cpp` |
| Discovery / connection-state / pan-stream-lifecycle wiring | `MainWindow_Session.cpp` (not `_Wiring`) |
| Client-DSP applet or PooDoo-tile wiring | `MainWindow_DspApplets.cpp` (not `_Wiring`) |
| A new menu item or action | `MainWindow_Menus.cpp` |
| A new keyboard shortcut | `MainWindow_Shortcuts.cpp` |
| A stateless formatter/helper with no `MainWindow` dependency | `MainWindowHelpers.{h,cpp}` |
| A whole new subsystem with no existing TU home | A **new** `MainWindow_<Subsystem>.cpp` sibling (copy the header-comment style) — not `MainWindow.cpp` |
| A member field/declaration a sibling method needs | `MainWindow.h` (unavoidable — C++ requires members on the class), kept minimal |
| A small guard/condition inside a function that itself lives in `MainWindow.cpp` and can't move | Stays inline in `MainWindow.cpp` (e.g. the startup-geometry reapply guard inside `showEvent()`) |

## When a new TU is warranted

Splitting `MainWindow` across TUs is a **readability + parallel-compile** move —
it is **not** decoupling. Every sibling `#include`s the ~1,000-line
`MainWindow.h` (which transitively pulls ~50 heavy headers, with no precompiled
header), so:

- Each new TU adds a **full re-parse** of that header stack to the build.
- Any edit to `MainWindow.h` (e.g. adding a member for a feature) **rebuilds
  every sibling TU** — there are 9 today (10 files include the header, counting
  `MainWindow.cpp` itself).
- Siblings share all private state through the header, so they are separate
  *files*, not separate *modules*. No boundary is enforced.

So a new TU has real, recurring cost. Create one only when it earns its keep:

1. **Default to an existing sibling.** If the work belongs to a subsystem
   already housed (a new demod → `DigitalModes`, a menu → `Menus`, wiring → `Wiring`),
   it goes there. Do not spawn a TU for work that has a home.
2. **A new TU is a distinct, cohesive subsystem with no existing home** — a
   feature-family with its own lifecycle/state, for which you could write a
   one-line charter that fits *none* of the existing siblings. Not a stray
   method or two.
3. **Size is a signal, not a trigger.** The existing siblings run ~500–2,500
   lines, each one subsystem. A new area that is only ~50 lines can wait in the
   closest sibling (or `MainWindow.cpp` if genuinely cross-cutting) until it
   grows. Do **not** pre-create near-empty TUs — that is pure cost for no
   readability gain.
4. **Split an existing sibling when it accretes a *second* unrelated
   subsystem.** The driver is cohesion, not a hard line count: a sibling that
   has grown to cover more than one subsystem is the split candidate (e.g. if
   `DigitalModes` later also swallowed all CW/keyer logic, that splits out).

### The ceiling — split to cohesion, then extract a class, don't slice finer

There is a floor *and* a ceiling. Once a TU is a single cohesive subsystem at a
reviewable size, **stop splitting.** If you are tempted to subdivide *below*
that — carving one subsystem into several thin TUs — that is the signal to
**extract a real class instead**, not to add more `MainWindow_*.cpp` files.

Slicing thinner adds build cost and arbitrary seams while the actual coupling
(the god-class with shared private state) is untouched — it can masquerade as
decoupling work and substitute for it. A real class (e.g. a `WfmController` that
owns its own state and *removes* members from `MainWindow.h`) is the only move
that shrinks the shared header, cuts the rebuild fan-out, and creates a boundary
the compiler enforces. That is the **#3557** direction; prefer it over a
finer-grained TU split.

**Naming/shape for a justified new TU:** `MainWindow_<Subsystem>.cpp`; methods
stay `MainWindow::` members declared in `MainWindow.h`; open the file with the
standard header-comment charter (what it holds + the issue ref), matching the
existing siblings. When torn between a new TU and `MainWindow.cpp`, pick the new
TU — the whole point is to stop `MainWindow.cpp` from re-accreting.

## Conventions when moving or adding a sibling-TU method

- **It's the same class.** Define `void MainWindow::foo()` in the sibling TU;
  declare `foo()` in `MainWindow.h` as usual. No `friend`, no new class.
- **Carry includes explicitly.** The Linux CI floor is **Qt 6.4.2**; do not rely
  on transitive includes that only resolve on newer Qt. A sibling TU must
  `#include` every header for the symbols *it* uses, even if `MainWindow.cpp`
  already included them. (This bit #3532; grep moved code for `Q[A-Z]` symbols
  and add the includes.)
- **Don't leave orphaned includes behind.** When you move the last user of a
  header out of `MainWindow.cpp`, remove that `#include` from `MainWindow.cpp`
  too. (But verify the header isn't used by something else first — e.g.
  `PanadapterStream.h` looks WFM-adjacent but is used pervasively for pan audio.)
- **Wiring split mirrors the widget's lifecycle.** A singleton wired in the
  constructor (e.g. the `RxApplet`) keeps its `connect()` in `MainWindow.cpp`;
  a per-instance object (e.g. each `VfoWidget`) is wired in its
  `wireXxx()` in `MainWindow_Wiring.cpp`. RADE and WFM both follow this split —
  match the nearest sibling feature rather than forcing artificial consistency.

## Ownership

The **whole** MainWindow surface — the core `MainWindow.{h,cpp}` and every
`MainWindow_*.cpp` sibling TU — sits at the broad reviewer tier (Tier 3,
`@aethersdr/reviewers`). The core files were formerly maintainer-gated, but
now that the decomposition is complete that gate is gone: opening up
review/approval of the extracted feature code — and the shrunk core along with
it — to more of the team was a primary goal of the split, so nothing here
bottlenecks on a single maintainer. See [`CONTRIBUTING.md`](../../CONTRIBUTING.md)
and [`.github/CODEOWNERS`](../../.github/CODEOWNERS).

## Further direction

- **#3557** — extract per-feature *controllers* (RADE / FreeDV / WFM as a
  family) out of the `MainWindow` class entirely, so they stop being members.
- **#3558** — table-drive the menu construction in `MainWindow_Menus.cpp`.

Until those land, keep adding to the sibling TUs as above.
