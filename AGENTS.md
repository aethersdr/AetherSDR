# AetherSDR — Project Context for AI Agents

This is the canonical project guide for any AI assistant working on
AetherSDR — Claude Code, OpenAI Codex, Cursor, GitHub Copilot, Gemini
Code Assist, Aider, AetherClaude (our orchestrator bot), or any other
tool. Each tool has its own well-known file at a different path
(`CLAUDE.md`, `.github/copilot-instructions.md`, `GEMINI.md`,
`CONVENTIONS.md`, etc.); those are thin pointers back here. Everything
project-wide lives in **this** file.

If you are an AI assistant: read this file end-to-end before writing
code or recommending merges. The file is ~330 lines; that's the cost
of doing the job right on this codebase.

## Project Goal

Replicate the **Windows-only FlexRadio SmartSDR client** (written in C#) as a
**Linux-native C++ application** using Qt6 and C++20. The aim is to mirror the
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
- **Read `CONTRIBUTING.md`** for full contributor guidelines, coding conventions,
  and the AI-to-AI debugging protocol (open a GitHub issue for cross-agent coordination)
- **Read the AetherSDR Constitution before writing or reviewing code.**
  Canonical source: `.specify/memory/constitution.md`. Byte-identical
  mirror at `CONSTITUTION.md` in repo root for discoverability. 14
  principles total: 7 AetherSDR-domain conventions (FlexLib authority,
  MeterSmoother, UI labels, BandPlanManager, nested-JSON config, CHAIN
  widget, auto-generated Contributors) + 7 defensive engineering
  principles adopted from Cisco's
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

Current version: **26.5.2.1** (set in both `CMakeLists.txt` and `README.md`).
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
- `AudioEngine` — RX/TX audio, NR2/RN2/NR4/BNR/DFNR DSP pipeline
- `SpectrumWidget` — GPU-accelerated FFT spectrum + waterfall (QRhiWidget)
- `MainWindow` — wires everything together, signal routing hub
- `PanadapterStream` — VITA-49 UDP parsing, routes FFT/waterfall/audio/meters

**Threading:** up to 11 threads — see `docs/architecture/pipelines.md` for the
full thread diagram, data flow, cross-thread signal map, and GPU rendering notes.

**Design principle:** RadioModel owns all sub-models on the main thread.
Worker threads communicate exclusively via auto-queued signals. Never hold
a mutex in the audio callback.

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

**The radio is always authoritative for any setting it stores.** AetherSDR
must never save, recall, or override radio-side settings from client-side
persistence. Only save client-side settings for things the radio does NOT save.

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
