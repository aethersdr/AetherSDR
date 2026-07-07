# AetherSDR — Project Context for AI Agents

This is the canonical project guide for any AI assistant working on
AetherSDR — Claude Code, OpenAI Codex, Cursor, GitHub Copilot, Gemini
Code Assist, Aider, AetherClaude (our orchestrator bot), or any other
tool. Each tool has its own well-known file at a different path
(`CLAUDE.md`, `.github/copilot-instructions.md`, `GEMINI.md`,
`CONVENTIONS.md`, etc.); those are thin pointers back here. Everything
project-wide lives in **this** file.

If you are an AI assistant: read this file end-to-end before writing
code or recommending merges. The file is ~440 lines; that's the cost
of doing the job right on this codebase.

## Project Goal

Replicate the **Windows-only FlexRadio SmartSDR client** (written in C#) as a
**native, cross-platform C++ application** using Qt6 and C++20. The aim is to mirror the
look, feel, and every function SmartSDR is capable of. The reference radio is a
**FLEX-8600 running firmware 4.2.18**, which speaks **SmartSDR protocol v1.4.0.0**.

## AI Agent Guidelines

When helping with AetherSDR:
- Prefer C++20 / Qt6 idioms (std::ranges, concepts if clean, Qt signals/slots over lambdas when possible)
- Keep classes small and single-responsibility
- Use RAII everywhere (no naked new/delete)
- Comment non-obvious protocol decisions with firmware version
- When suggesting code: show **diff-style** changes or full function/class if small
- Test suggestions locally if possible (assume Arch Linux build env)
- Never suggest Wine/Crossover workarounds — goal is native
- Flag any proposal that would break slice 0 RX flow
- If unsure about protocol behavior → ask for logs/wireshark captures first
- **Use `AppSettings`, never `QSettings`** — see "Settings Persistence" below
- **New engine code goes in `libaethercore`** (`src/core/` or `src/models/`),
  exposed to the UI through models — never via a new gui→core header include.
  See "Build targets" and "In-flight: aetherd" under Architecture Overview.
- **Read `CONTRIBUTING.md`** for full contributor guidelines, coding conventions,
  and the AI-to-AI debugging protocol (open a GitHub issue for cross-agent coordination)
- **Sign every commit you author.** `main` enforces `required_signatures`, so a
  PR with unsigned commits cannot merge without an admin override. If the
  contributor has not set up commit signing yet, walk them through
  `docs/COMMIT-SIGNING.md` **before** you commit — the top of that file is a
  step-by-step AI-assistant algorithm covering Windows / macOS / Linux / WSL /
  Raspberry Pi. Default to SSH signing; GPG is the fallback for existing GPG
  workflows. Verify with `git log --show-signature -1` after the first commit.
- **Read the AetherSDR Constitution before writing or reviewing code.**
  Canonical source: `.specify/memory/constitution.md`. Byte-identical
  mirror at `CONSTITUTION.md` in repo root for discoverability. 14
  principles total (constitution v2.0.0): 7 AetherSDR-domain governance
  principles (FlexLib authority, radio-authoritative live state,
  radio-persistable settings, clean-room contributions, per-feature
  config ownership, transmit-on-intent, boundary input validation) + 7
  defensive engineering principles adopted from Cisco's
  [Foundry Constitution](https://github.com/CiscoDevNet/foundry-security-spec/blob/main/constitution.md)
  (Evidence Over Assertion, Surface Only What Survives, Claims Are
  Atomic And Mortal, Fixes Are Demonstrated, Sandbox By Infrastructure,
  Operator Outranks Every Agent, Persist Atomically). The defensive
  set codifies how AetherSDR's multi-agent contribution model (≥6
  distinct AI tools touching the codebase) avoids the failure modes
  of confident-but-wrong AI changes, stale-snapshot reverts, and
  prompt-injection escalation. Commit messages cite the most-load-bearing
  principle as `Principle <N>.` at the end of the subject line.

### Issue / PR Claim Protocol — Assign Yourself

When an AI agent is **actively reviewing an issue or PR — for comment,
for merge recommendation, or to implement a fix** — the agent MUST
assign itself to the issue or PR using GitHub's `assignees` feature
**before** posting the review, comment, or merge action.

This is the visible claim mechanism for multi-agent contribution
coordination (Principle X: Claims Are Atomic And Mortal). The
`aetherclaude-eligible` label gates implementation work; the
assignees list signals "an agent is actively engaged on this right
now" to every other agent and to the maintainer.

**Concrete rules**:

1. **Before** posting a review, comment, or merge action: check the
   current assignees. Then:
   - **Unassigned, or assigned ONLY to AetherClaude
     (`@aethersdr-agent`)**: add yourself alongside.
     **AetherClaude auto-triages every new issue and every new
     PR**, so its assignment is the persistent triage-engagement
     signal, NOT a claim on active merge work. Adding yourself
     alongside AetherClaude is expected and correct.
   - **Already assigned to another human agent or AI agent**
     (not AetherClaude): leave a coordination comment instead of
     double-assigning, and do not proceed with overlapping work.
2. **While** working: stay assigned. Other agents will see the
   non-AetherClaude assignment and route around you.
3. **After** posting the comment / completing the review / merging:
   the assignment can stay. GitHub auto-clears assignees when an
   issue closes or a PR merges. Manual unassign is optional but
   appropriate when you've concluded but the issue/PR remains open
   (e.g., you reviewed and recommended merge but didn't merge
   yourself).
4. **If your work is interrupted** (token limit, context loss,
   model failure): leave a brief comment ("Stepping away;
   unassigning so another agent can pick up") and unassign. The
   claim is mortal — it dies with the agent that held it
   (Principle X). AetherClaude's assignment is separate and stays.
5. **Quick read-only actions don't require assignment**: pulling an
   issue's title to summarize, listing PRs in a status report,
   counting open issues. Assignment is for engagement that produces
   a comment, review, or merge.

**GitHub CLI command**:

```bash
# Assign yourself to issue NNNN or PR NNNN
gh issue edit NNNN --add-assignee @me
gh pr edit NNNN --add-assignee @me

# Unassign yourself
gh issue edit NNNN --remove-assignee @me
gh pr edit NNNN --remove-assignee @me

# Check current assignees
gh issue view NNNN --json assignees
gh pr view NNNN --json assignees
```

**Why this exists**: without a visible claim signal, two agents
working from different orchestrators or contributor IDEs can both
spend tokens reviewing the same PR, post conflicting recommendations
within minutes of each other, and waste the maintainer's review time
reconciling them. The assignees list is the cheap, persistent,
multi-agent-visible claim mechanism that prevents this. It is the
operational implementation of Principle X.

### Autonomous Agent Boundaries

AI agents (including AetherClaude/pi-claude) may autonomously fix:
- **Bugs with clear root cause** — persistence missing, guard missing, crash fix
- **Protocol compliance** — matching SmartSDR behavior confirmed by pcap/FlexLib
- **Build/CI fixes** — missing dependencies, platform compat

AI agents must **NOT** autonomously change:
- **Visual design** — colors, fonts, layout, theme (user preferences ≠ project direction)
- **UX behavior** — how controls work, what clicks do, keyboard shortcuts
- **Architecture** — adding new threads, changing signal routing, new dependencies
- **Feature scope** — adding features beyond what the issue describes
- **Default values** — changing defaults that affect all users based on one report

When in doubt, the agent should implement the fix and note in the PR that
design decisions need maintainer review. The project maintainer (Jeremy/KK7GWY)
is the sole authority on visual design and UX direction.

## C++ Style Guide

- **No `goto`** — use early returns, break, or restructure the logic
- **No raw `new`/`delete`** — use `std::unique_ptr`, `std::make_unique`, or Qt parent ownership
- **No `#define` macros for constants** — use `constexpr` or `static constexpr`
- **Braces on all control flow** — even single-line `if`/`else`/`for`/`while`
- **`auto` sparingly** — use explicit types unless the type is obvious from context (e.g. `auto* ptr = new Foo` is fine, `auto x = foo()` is not)
- **Naming**: classes `PascalCase`, methods/variables `camelCase`, constants `kPascalCase`, member variables `m_camelCase`
- **Platform guards**: prefer `Q_OS_WIN` / `Q_OS_MAC` / `Q_OS_LINUX` for new code. Existing `_WIN32`/`__APPLE__` guards can be migrated opportunistically — don't do a blanket rewrite.
- **Don't remove code you didn't add** — if rebasing, ensure upstream changes are preserved. Review the diff before submitting.
- **Atomic parameters for cross-thread DSP** — main thread writes via `std::atomic`, audio thread reads. Never hold a mutex in the audio callback for parameter updates.
- **Error handling**: log with `qCWarning(lcCategory)`, don't throw exceptions

## Build

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
./build/AetherSDR
```

Full dependency list is in `README.md` — don't duplicate it here.

Current version: **26.7.1** (set in both `CMakeLists.txt` and `README.md`).
Versioning scheme is **CalVer** (`YY.M.patch[.hotfix]`) starting from v26.5.1,
the 1.0-equivalent. Hotfix sub-patches use a 4th component (e.g. 26.5.2.1).
Earlier tags used semver through v0.9.8.

---

## CI/CD Workflow

CI runs in Docker image `ghcr.io/ten9876/aethersdr-ci:latest` (~3.5 min builds).
**If you add a new `find_package(...)` to CMakeLists.txt, also add the
corresponding `-dev` package to `.github/docker/Dockerfile` and push.** The
`docker-ci-image.yml` workflow rebuilds the image automatically (~3 min); wait
for that before the next CI run can use it.

**`git ship`** alias — squashes local commits ahead of origin/main, creates a
branch, pushes, opens a PR with auto-squash-merge enabled. Commit freely
locally, then ship once.

Branch protection: signed commits required on main, CI must pass, CODEOWNERS
review required, branches auto-delete after merge.

**Helping a contributor set up commit signing?** Read
`docs/COMMIT-SIGNING.md` — the top of that file has explicit
AI-assistant instructions (algorithm, anti-patterns, completion
message). Works for Windows / macOS / Linux / WSL / Raspberry Pi
contributors. Default to SSH signing; GPG is the fallback for
contributors with existing GPG workflows.

---

## Architecture Overview

Key source directories: `src/core/` (protocol, audio, DSP), `src/models/`
(RadioModel, SliceModel, etc.), `src/gui/` (MainWindow, SpectrumWidget, applets).

**Key classes:**
- `RadioModel` — central state, owns connection + all sub-models
- `RadioSession` — per-radio aggregate that owns `RadioModel` + `TciServer` +
  `CatPorts`, giving teardown a structural order (#3351 / #3445)
- `AudioEngine` — RX/TX audio, NR2/RN2/NR4/BNR/DFNR DSP pipeline
- `SpectrumWidget` — GPU-accelerated FFT spectrum + waterfall (QRhiWidget)
- `MainWindow` — wires everything together, signal routing hub. **Decomposed
  (#3351)** into one class across `MainWindow.cpp` + `MainWindow_*.cpp` sibling
  TUs; new feature code goes in a sibling, NOT `MainWindow.cpp` — see
  [Adding code to MainWindow](#adding-code-to-mainwindow)
- `PanadapterStream` — VITA-49 UDP parsing, routes FFT/waterfall/audio/meters

**Threading:** up to 12 threads — see `docs/architecture/pipelines.md` for the
full thread diagram, data flow, cross-thread signal map, and GPU rendering notes.

**Design principle:** RadioModel owns all sub-models on the main thread.
Worker threads communicate exclusively via auto-queued signals. Never hold
a mutex in the audio callback.

### In-flight: aetherd engine/UI decoupling (RFC accepted 2026-07-04)

The accepted RFC at
[`docs/aetherd-headless-engine-design.md`](docs/aetherd-headless-engine-design.md)
(tracking issue #3849) splits this codebase into an engine library
(`libaethercore`), a headless engine daemon (`aetherd`), and thin UI
clients, with pluggable radio backends (`IRadioBackend`). Implementation
follows the RFC's §10 staged order; **step 1 (`libaethercore`) and the
step-2 seam have landed** — the engine is a static library, and
`IRadioBackend` (`src/core/backends/`) with its first implementor
`FlexBackend` (`src/core/backends/flex/`) exist. `FlexBackend` is a
skeleton so far: RadioModel owns it and it observes the connection
lifecycle, but the SmartSDR wire stack moves behind it incrementally
(2.2b–2.4). The versioned protocol (step 3+) has not landed — UI code
still consumes models directly, and that remains correct.

**Build targets (post-RFC step 1):**

| Target | Contents | May link |
|---|---|---|
| `libaethercore` (`aethercore`) | `src/core/` + `src/models/` — the engine | Qt Core/Network/Multimedia/WebSockets/SerialPort/DBus, the DSP + third-party libs. **Never `gui/`; QtWidgets only via the tracked-legacy files below, shrinking to zero** |
| `AetherSDR` | `src/gui/` + `main.cpp` — the desktop app | `aethercore` + Qt Widgets + qgeoview + QRhi private |

The dependency direction is CI-enforced (`tools/check_engine_boundary.py`,
`engine-boundary.yml`, `--strict`) by three ratchets:
- **EB1** — no `core/`/`models/` file may include a `gui/` header (now
  zero; any finding is an error).
- **EB2** — no `core/`/`models/` file may use QtWidgets (a shrinking
  tracked-legacy set warns, new usage errors).
- **EB3** — no file **above the radio seam** (all of `src/gui/`,
  `src/core/`, `src/models/` **except** the backend tree
  `src/core/backends/`) may include a **vendor header** — the
  family-specific wire classes the RFC keeps behind `IRadioBackend`
  (SmartSDR/FlexLib + KiwiSDR; the headers tagged `vendor(...)` in
  `docs/architecture/aetherd-touchpoint-tags.json` — standalone *accessory*
  devices like the 4O3A tuner/switch/amp are tagged `peripheral(...)`, not
  `vendor`, so they are NOT gated by EB3). Today's coupling is
  frozen as a per-file, shrink-only baseline; a **new** above-seam vendor
  include, or an **increase** in a tracked file, errors. (RFC step 2.4;
  see "Engine boundary ratchet — EB3" below.)

If your change trips any of these, restructure the change — do not move
the file, weaken the check, or add an exemption. Engine code that needs a
UI callback defines a gui-free interface in `core/` (e.g.
`IConnectionAutomation`) that the gui implements — never a `gui/` include.

**Until migration rules appear in this file, nothing changes for you.**
Do not pre-emptively restructure code toward the RFC — no new engine/UI
seams, no backend interfaces, no speculative library targets. Each
migration step lands together with an update to this file stating the new
rules (pre-drafted in
[`docs/aetherd-agents-md-staging.md`](docs/aetherd-agents-md-staging.md));
if a rule isn't in this file, its step hasn't landed. Architecture changes
ahead of the RFC steps remain maintainer-only (see Autonomous Agent
Boundaries above). The CI-enforced rules so far are EB1/EB2/EB3 above
(`tools/check_engine_boundary.py`, warning for tracked baselines, error
for new violations).

**Engine boundary ratchet — EB3 (vendor includes).** As of RFC step 2.4,
`check_engine_boundary.py` also enforces that nothing above the radio seam
reaches around `IRadioBackend` to a vendor wire class. What this means for
you:

- **Nothing was relocated.** Step 2.4 is *ratchet-only*: the vendor
  headers stay where they are (`src/core/…`, `src/models/…`) for now. EB3
  just makes the existing boundary enforceable *in place*, so the
  decoupling can proceed without new coupling piling up behind it.
- **The rule.** Each tracked file's baseline row is the exact **set** of
  vendor headers it may include. Adding a vendor `#include` (e.g.
  `KiwiSdrManager.h`, `RadioConnection.h`, `TunerModel.h`) to a `gui/`,
  `core/`, or `models/` file that isn't tracked — or adding a header not
  in a tracked file's set, *including a lateral swap that keeps the count
  flat* (drop `RadioConnection.h`, add `KiwiSdrManager.h`) — fails the
  check. The per-file baseline (`KNOWN_VENDOR_INCLUDE_BASELINE`) lives at
  the top of `tools/check_engine_boundary.py`; the vendor vocabulary is
  **derived at runtime from the touchpoint audit**
  (`docs/architecture/aetherd-touchpoint-tags.json`, the single source of
  truth), so a header newly tagged `vendor` there is enforced without
  editing the checker.
- **Adding a radio feature?** Don't include the vendor class above the
  seam. Put the wire code in the family backend
  (`src/core/backends/<family>/`) and surface it through `IRadioBackend`
  (a canonical verb/signal, or the namespaced
  `invokeExtension`/`extensionStatus` channel for vendor-specifics), then
  consume *that* from the model/UI. Backend code (under
  `src/core/backends/`) and the vendor translation units themselves may
  include vendor headers freely — they're below the seam.
- **Removing coupling (the goal).** When you convert a file's radio access
  to the seam and drop a vendor include, **remove that stem from the
  file's row** in `KNOWN_VENDOR_INCLUDE_BASELINE` (delete the row when it
  empties). The set only shrinks — never add a stem or a row to make a
  build pass. If EB3 blocks you and the include is genuinely unavoidable,
  that's a design conversation for a maintainer, not a baseline edit.
- **`src/gui/**` is in the CI trigger** for `engine-boundary.yml` now
  (EB3 guards gui files), so a gui-only PR that adds vendor coupling is
  still caught.

**Where radio-facing code goes now that the seam exists.** Route by kind:

| Your change | Goes |
|---|---|
| Code speaking a vendor wire protocol (commands, discovery, stream parsing) | that family's backend under `src/core/backends/<family>/`, behind `IRadioBackend` — never in `gui/`, and increasingly not in the models (they're being decoupled from the wire over 2.2b–2.4) |
| A new radio family | a new `IRadioBackend` implementation under `src/core/backends/<family>/` — requires an approved design doc naming its open protocol authority (Constitution Principles I & IV apply per backend) |
| A new engine feature | `libaethercore`, exposed through models — never via a new gui→core header |

Do **not** reroute existing model↔wire code through `FlexBackend`
wholesale — the per-touchpoint conversion is staged work
(`docs/architecture/aetherd-touchpoints.md`). The five `mixed` models
(Radio/Slice/Transmit/Panadapter/Meter) have been split (2.3): their
SmartSDR status decode now lives in `FlexBackend` behind typed deltas, and
the models apply normalized signals. The remaining vendor headers are
**not** relocated yet — step 2.4 landed the EB3 ratchet (above) that
freezes today's above-seam vendor coupling and lets it be decoupled
subsystem-by-subsystem. Converting a touchpoint still follows the claim
protocol + before/after `tools/verify_slice0_rx.py` recipe; a converted
file drops its vendor include and lowers its EB3 baseline.

---

## SmartSDR Protocol (v1.4.0.0)

### Message Types

| Prefix | Dir | Meaning |
|--------|-----|---------|
| `V` | Radio→Client | Firmware version |
| `H` | Radio→Client | Hex client handle |
| `C` | Client→Radio | Command: `C<seq>\|<cmd>\n` |
| `R` | Radio→Client | Response: `R<seq>\|<hex_code>\|<body>` |
| `S` | Radio→Client | Status: `S<handle>\|<object> key=val ...` |
| `M` | Radio→Client | Informational message |

Status object names are **multi-word** (`slice 0`, `display pan 0x40000000`,
`interlock band 9`). The parser finds the split between object name and
key=value pairs by locating the last space before the first `=` sign.

### Connection Sequence

1. TCP connect → radio sends `V<version>` then `H<handle>`
2. `sub <topic> all` for each of: `slice`, `pan`, `tx`, `amplifier`, `atu`,
   `meter`, `audio`, `gps`, `apd`, `client`, `xvtr`
3. `client gui` + `client program AetherSDR` + `client station AetherSDR`
4. Bind UDP socket, send `\x00` to radio:4992 (port registration)
5. `client udpport <port>` (returns error 0x50001000 on v1.4.0.0 — expected)
6. `slice list` → if empty, create default slice (14.225 MHz USB ANT1)
7. `stream create type=remote_audio_rx compression=none` → radio starts sending
   VITA-49 audio to our UDP port

### Protocol / Firmware Quirks (v1.4.0.0 protocol on fw 4.x)

- `client set udpport` returns `0x50001000` — use the one-byte UDP packet method
- `client set enforce_local_ptt=1` returns `0x50001000` — correct command is `client set local_ptt=1`; the radio echoes a full `connected` status to ALL clients updating their `local_ptt` field when ownership changes
- Slice frequency is `RF_frequency` (not `freq`) in status messages
- Streams are discriminated by **PacketClassCode** (PCC), NOT by packet type
- `audio_level` is the status key for AF gain (not `audio_gain`)
- The radio **never sends `mox=` in transmit status messages**. Use
  `isTransmitting()` (interlock state machine), NOT `isMox()`
- Three separate tune command paths all need interlock inhibit:
  `transmit tune 1`, `tgxl autotune`, `atu start`
- `cw key immediate` not supported — use netcw UDP stream for CW keying
- `transmit set break_in=1` wrong — correct: `cw break_in 1`

VITA-49 packet format, PCC codes, FFT bin conversion, waterfall tile format,
audio payload, meter data — see `docs/architecture/vita49-format.md`.

---

## Key Implementation Patterns

### Adding code to MainWindow

`MainWindow` was a ~19,500-line monolith; **#3351 split it into one class across
`MainWindow.cpp` + a family of `MainWindow_*.cpp` sibling TUs.** It is still one
`MainWindow` class — every sibling-TU function is a `MainWindow::` member
declared in `MainWindow.h`. The split is about *which file* a body lives in.

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
and any header edit rebuilds all of them. Split only to a cohesive, reviewable
granularity, then **stop**: if tempted to subdivide one subsystem into several
thin TUs, extract a real class instead (the #3557 direction) — that's the only
move that actually decouples.

Sibling TUs must **carry their includes explicitly** — the Linux CI floor is
Qt 6.4.2; don't rely on transitive includes (this broke #3532). When you move
the last user of a header out of `MainWindow.cpp`, drop that `#include` too.

Full map + decision guide:
**[`docs/architecture/mainwindow-decomposition.md`](docs/architecture/mainwindow-decomposition.md)**.

### Adding or converting a dialog

See **[`docs/style/dialog-patterns.md`](docs/style/dialog-patterns.md)** before writing
or modifying a `QDialog`. It documents the canonical
lazy-construct + non-modal + geometry-persist + frameless-chrome pattern,
the common pitfalls that have hit real PRs, and the existing dialogs to
use as reference. Tracked for cleanup in #2605 (`PersistentDialog` base
class).

Any new popout window, floating tool window, or `QDialog` must respect the
global `FramelessWindow` setting unless there is a specific reason not to.
Use the existing frameless helpers instead of custom window chrome:

- Add a `FramelessWindowTitleBar` at the top of the dialog/window layout.
- Install `FramelessResizer::install(this)` for resizable popouts.
- Add `setFramelessMode(bool on)` using the same pattern as
  `NetworkDiagnosticsDialog`: capture geometry, toggle
  `Qt::FramelessWindowHint`, restore geometry only if the window was already
  visible, show again only if it was already visible, and hide/show the custom
  title bar based on the setting.
- Initialize from `AppSettings::instance().value("FramelessWindow", "True")`.
- Do not use `QSettings`.

Do not manually move first-show dialogs to `(0,0)` or restore constructor-time
geometry. For first show, either let Qt/window-manager placement handle it, or
use the same placement behavior as the closest existing dialog. If centering is
explicitly required, do it deliberately after the dialog has a valid size and
document why.

### Settings Persistence (AppSettings — NOT QSettings)

**IMPORTANT:** Do NOT use `QSettings` anywhere in AetherSDR. All client-side
settings are stored via `AppSettings` (`src/core/AppSettings.h`), which writes
an XML file at `~/.config/AetherSDR/AetherSDR.settings`. Key names use
PascalCase (e.g. `LastConnectedRadioSerial`, `DisplayFftAverage`). Boolean
values are stored as `"True"` / `"False"` strings.

```cpp
auto& s = AppSettings::instance();
s.setValue("MyFeatureEnabled", "True");
bool on = s.value("MyFeatureEnabled", "False").toString() == "True";
```

### Settings Migration

One-time migrations when renaming or restructuring keys (e.g. `Applet_DIGI` →
`Applet_CAT`, `DaxTxGain` → `TciTxGain`):

```cpp
auto& s = AppSettings::instance();
if (s.contains("OldKey") && !s.contains("NewKey")) {
    s.setValue("NewKey", s.value("OldKey", "default").toString());
    s.remove("OldKey");
    s.save();
}
```

Run once at app or feature startup, not on every access.

### Radio-Authoritative Settings Policy

**The radio is always authoritative for any setting it stores** (Constitution
Principles II & III). AetherSDR must never save, recall, or override radio-side
settings from client-side persistence. Only save client-side settings for things
the radio does NOT save.

**Radio-authoritative (do NOT persist):** frequency, mode, filter, step size,
AGC, squelch, DSP flags, antennas, TX power, panadapter *count* and per-pan
state (center, bandwidth, min/max dBm, etc.).

**Client-authoritative (persist in AppSettings):** window geometry, layout
arrangement (`PanadapterLayout`, applet order/visibility), client-side DSP
(NR2/RN2/NR4/DFNR), UI preferences, display preferences, spot settings.

**Why:** When both persist the same setting, they fight on reconnect. The
radio's GUIClientID session restore is always more current than our saved state.

### GUI↔Radio Sync (No Feedback Loops)

- Model setters emit `commandReady(cmd)` → `RadioModel` sends to radio
- Radio status pushes update models via `applyStatus(kvs)`
- Use `m_updatingFromModel` guard or `QSignalBlocker` to prevent echo loops

### Auto-Reconnect

`RadioModel` has a 3-second `m_reconnectTimer` for unexpected disconnects.
Disabled by `m_intentionalDisconnect` flag on user-initiated disconnect.

### Optimistic Updates Policy

Some radio commands lack status echo (e.g. `tnf remove`). Update the local
model optimistically. **File a GitHub issue** tagged `protocol` + `upstream`
for each missing status echo — optimistic updates break Multi-Flex.

### Meter Smoothing — use MeterSmoother

Every meter / level-bar / GR readout in the GUI must drive its display
value through `MeterSmoother` (`src/gui/MeterSmoother.h`). Don't write
new envelope-follower code or copy smoothing logic from other widgets
— `MeterSmoother`'s header has the API and a usage example.

### User-facing names match the on-screen UI labels

In prose (issue comments, README, What's-New strings, error toasts, support
requests) call a control by the label the user sees, not the C++ class name —
e.g. the **DIGI applet** (class `CatApplet`), and the Help → Support logging
toggles **Discovery / Commands / Status** (not backend names like
`radio.connection`). The on-screen label wins for prose, so users can find the
control you're naming.

### Region-aware band data — read from BandPlanManager, not BandDefs.h

Anything needing band edges, segment sizes, or per-band metadata reads the
active plan via `BandPlanManager` (`AppSettings["BandPlanName"]` + the JSON in
`resources/bandplans/`). `src/models/BandDefs.h::kBands[]` is ARRL/US-only and
not region-aware — don't source new features from it; AetherSDR's users span
IARU regions 1/2/3.

### TX DSP stages integrate with the CHAIN widget

The TX DSP chain is stage-per-applet and the visual **CHAIN** widget is the
primary entry point. New TX DSP stages must be ordered, toggleable, and
inspectable through the CHAIN widget rather than adding a parallel UI entry —
it's the user's mental model for the TX signal path.

### The About-dialog Contributors list is auto-generated

The Contributors list in the About dialog is built at runtime from the GitHub
API; manual edits are overwritten on the next build. If someone is missing, fix
the GitHub-side attribution (commit authorship / `Co-Authored-By` trailer),
don't patch the dialog string.

---

## Multi-Panadapter Support

**Architecture:** PanadapterModel (per-pan state), PanadapterStream (VITA-49
routing by stream ID), PanadapterStack (QSplitter), wirePanadapter() (per-pan
signal wiring), spectrumForSlice() (overlay routing).

**Key protocol facts:**
- Click-to-tune: `slice m <freq> pan=<panId>` — NOT `slice tune`
- Never send `slice set <id> active=1` — managed client-side only
- Push `xpixels`/`ypixels` on pan creation (radio defaults to 50×20)
- FFT stream ID = pan ID (0x40xx), waterfall stream ID = waterfall ID (0x42xx)

See `docs/architecture/multi-pan-pitfalls.md` for 20 numbered lessons learned.

---

## Multi-Client (Multi-Flex) Support

Filter all status and VITA-49 packets by `client_handle` — three layers:
1. **Slice ownership**: track `m_ownedSliceIds` from `client_handle` field
2. **Panadapter status**: only claim `display pan`/`display waterfall` matching our handle
3. **VITA-49 UDP**: `setOwnedStreamIds(panId, wfId)` drops non-matching packets

Early status messages arrive WITHOUT `client_handle`. Create SliceModels for
all initially, remove other clients' when handle arrives.

---

## KiwiSDR Public-Receiver Browser

The KiwiSDR browser is a clean-room, API-policy-aware public-receiver directory
(#3679) — find and connect to public KiwiSDR receivers, independent of the
FlexRadio protocol path. Kiwi panadapters are receive-only (TX is inhibited).
See `docs/kiwisdr-public-directory.md` (directory / API-policy behaviour) and
`docs/kiwisdr-cleanroom-design.md` (clean-room design notes, Principle IV).

---

## Accessibility — `src/gui/` Rules

Touching any file under `src/gui/`? Read [`docs/a11y.md`](docs/a11y.md)
**before** adding or modifying a widget. Canonical Qt patterns:
`setAccessibleName` / `setAccessibleDescription`, `QAccessibleValueChangeEvent`
on every value-change method, `QAccessibleInterface` subclass for any
`paintEvent` override, and the interactive-`QLabel` anti-pattern (replace
with `QPushButton` or add keyboard activation).

CI enforcement: [`tools/check_a11y.py`](tools/check_a11y.py) runs on every
PR via [`.github/workflows/a11y-check.yml`](.github/workflows/a11y-check.yml)
and emits inline diff annotations for the patterns above. Warning-only
(`exit 0`); never blocks a build.

## Agent Automation Bridge — verify the GUI without pixels

Need to assert on UI state, drive a control, confirm a widget rendered, or
capture the panadapter while verifying a change? AetherSDR ships an in-process,
agent-drivable bridge (off in production). Launch with `AETHER_AUTOMATION=1`
and drive a `QLocalServer` that speaks newline-delimited JSON:

- `dumpTree` → semantic snapshot of the whole widget tree (objectName,
  accessibleName, enabled, geometry, live `value`) — your "DOM" for controls.
- `grab <widget>` → PNG of any widget, including a correct GPU-framebuffer
  readback of the panadapter (`SpectrumWidget`).
- `invoke <target> <action> [value]` → click/toggle/setValue/setText/… a
  control. **Refuses any control marked transmit-keying (`markTxKeying()` /
  `aetherTxKeying` property — MOX/PTT, TUNE, ATU, CWX send, packet/APRS send)
  unless `AETHER_AUTOMATION_ALLOW_TX=1`** — the bridge can never key a live
  radio by accident; setpoint sliders ("Tune power", "RF power") stay drivable.
  Marked controls show `"keying": true` in `dumpTree`. Also **refuses disabled
  controls** (no silent no-op). Disambiguate duplicate names with a scoped
  target: `"RxApplet/AF gain"` vs `"PanadapterApplet/AF gain"`.
- `get radio|transmit|equalizer|slice|slices|pan|pans [selector] [property]` →
  live JSON model snapshot (frequency, mode, filter, NB/NR, squelch/AGC/APF,
  center MHz, min/max dBm, RF/mic/CW TX-chain, EQ bands, …). Assert on truth
  without screenshots. Sliders/spinboxes also report `range` in `dumpTree`.

Quick start: `python3 tools/automation_probe.py demo` (no Qt dependency); also
`get radio`, `invoke 'Master volume' setValue 35`. Full reference — protocol,
JSON schemas, targeting rules, recipes, gotchas — in
**[`docs/automation-bridge.md`](docs/automation-bridge.md)**. This is the
deterministic, cross-OS way to do "snapshot → act → assert" on the native UI
(issue [#3646](https://github.com/aethersdr/AetherSDR/issues/3646)).
