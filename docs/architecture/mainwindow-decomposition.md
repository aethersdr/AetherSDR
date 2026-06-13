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
| `MainWindow.cpp` / `MainWindow.h` | — | The core: constructor (now mostly `wireXxx()` calls), central state members, the signal-routing hub, and cross-cutting code that belongs to no single feature. **Maintainer-gated.** |
| `MainWindow_Controllers.cpp` | 1a | Physical-controller subsystems: FlexControl, HID encoders (RC-28 / TMate 2 / Ulanzi / PowerMate / Shuttle), StreamDeck labels, controller + meter wiring. |
| `MainWindow_Menus.cpp` | 1b | `buildMenuBar()` — every `QMenu`/`QAction`, their enable/disable wiring, and the inline lambdas they trigger. |
| `MainWindow_Shortcuts.cpp` | 1c | The keyboard-shortcut system + its shared state accessors. |
| `MainWindow_Wiring.cpp` | 1d | Per-object signal wiring: the `wirePanadapter()` / `wireSlice()` / `wireVfoWidget()` / `wireDsp…()` methods that connect each dynamically-created radio object to the UI. |
| `MainWindow_DigitalModes.cpp` | 1e | Demod / mode subsystems and their activate/deactivate lifecycles: RADE, FreeDV, DAX, AX.25 / KISS TNC, RTTY, **WFM**. |
| `MainWindow_SwrSweep.cpp` | 1e | The AetherSweep SWR-sweep engine (lock → step → state machine → pan overlay). |
| `MainWindow_Spots.cpp` | 2b | `wireSpotSubsystem()` — DX Cluster / RBN / WSJT-X / SpotCollector / POTA clients and their UI plumbing. |
| `MainWindowHelpers.{h,cpp}` | 0 | Stateless formatters / value transforms with **no** `MainWindow` dependency (tooltip builders, spot-ID math, client-list parsing, small pixmap painters). |
| `MainWindowShortcutState.h` | 1b | Internal shared shortcut state — **not** a public API; only `MainWindow*.cpp` TUs include it. |

## Decision guide — "where does my change go?"

| Your change | Goes in |
|---|---|
| A new feature's activate/deactivate or event handler that fits an existing subsystem above | That subsystem's TU (e.g. a new demod → `MainWindow_DigitalModes.cpp`, next to RADE/WFM) |
| Connecting a newly-created radio object (slice / pan / VFO / DSP widget) to the UI | `MainWindow_Wiring.cpp` |
| A new menu item or action | `MainWindow_Menus.cpp` |
| A new keyboard shortcut | `MainWindow_Shortcuts.cpp` |
| A stateless formatter/helper with no `MainWindow` dependency | `MainWindowHelpers.{h,cpp}` |
| A whole new subsystem with no existing TU home | A **new** `MainWindow_<Subsystem>.cpp` sibling (copy the header-comment style) — not `MainWindow.cpp` |
| A member field/declaration a sibling method needs | `MainWindow.h` (unavoidable — C++ requires members on the class), kept minimal |
| A small guard/condition inside a function that itself lives in `MainWindow.cpp` and can't move | Stays inline in `MainWindow.cpp` (e.g. the WFM guard inside `setPanFollow()`) |

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

`MainWindow.{h,cpp}` is maintainer-gated (central architecture). The
`MainWindow_*.cpp` sibling TUs are **deliberately at the broad reviewer tier** —
opening up review/approval of extracted feature code to more of the team was a
primary goal of the decomposition. See [`CONTRIBUTING.md`](../../CONTRIBUTING.md)
and [`.github/CODEOWNERS`](../../.github/CODEOWNERS).

## Further direction

- **#3557** — extract per-feature *controllers* (RADE / FreeDV / WFM as a
  family) out of the `MainWindow` class entirely, so they stop being members.
- **#3558** — table-drive the menu construction in `MainWindow_Menus.cpp`.

Until those land, keep adding to the sibling TUs as above.
