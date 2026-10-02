# AetherSDR — Project Context for AI Agents

This is the canonical project guide for any AI assistant working on
AetherSDR — Claude Code, OpenAI Codex, Cursor, GitHub Copilot, Gemini
Code Assist, Aider, AetherClaude (our orchestrator bot), or any other
tool. Several of those tools look for their own well-known file at a
different path (`CLAUDE.md`, `.github/copilot-instructions.md`,
`GEMINI.md`); those are thin pointers back here.

**How to read it.** Read this core file in full. Then read the sub-doc for
each area your change touches — they are part of this guide, split out so
nobody loads rules for code they are not changing:

| If your change touches… | Also read |
|---|---|
| `src/core/backends/`, `RadioModel`, `RadioSession`, `TransmitModel`, `ConnectionPanel`, discovery, `RadioCapabilities`, `src/aetherd/`, `src/core/control/`, a vendor wire header, or a `gui/`↔`core/` include | [`docs/agents/backends.md`](docs/agents/backends.md) — aetherd, the seam, engine-boundary ratchets, conformance checklist |
| The Flex wire path, slices/pans/status, Multi-Flex, KiwiSDR | [`docs/agents/flex-protocol.md`](docs/agents/flex-protocol.md) |
| `AppSettings`, `radio_settings`, `RadioStateMemory`, migrations, credentials, any setter that persists | [`docs/agents/settings.md`](docs/agents/settings.md) |
| `tests/`, `tests/tests.cmake`, `.github/workflows/`, the CI image | [`docs/agents/tests-ci.md`](docs/agents/tests-ci.md) |
| `src/gui/` — MainWindow, dialogs, applets, meters, theme, a11y; or you want to verify UI behavior | [`docs/agents/gui.md`](docs/agents/gui.md) |

**This file is documentation, not policy.** The rules that bind you live in
[`CONSTITUTION.md`](CONSTITUTION.md) and [`GOVERNANCE.md`](GOVERNANCE.md), and
they outrank everything here. Where this file appears to contradict either,
they win, and the contradiction is a defect here — fix it or open an issue.
That separation is what puts this file and `docs/agents/` at CODEOWNERS Tier 2
(infrastructure) while the Constitution and GOVERNANCE.md stay Tier 1
(maintainer-only).

One passage restates policy rather than describing practice:
§"Autonomous Agent Boundaries" below elaborates the autonomy limits that
[`GOVERNANCE.md`](GOVERNANCE.md) §AI Contributors defines. It may narrow or
illustrate them, never widen them — relaxing any of those bullets is an
amendment to GOVERNANCE.md and cannot be made in a Tier-2 PR.

## Project Goal

Replicate the **Windows-only FlexRadio SmartSDR client** (written in C#) as a
**native, cross-platform C++ application** using Qt6 and C++20. The aim is to mirror the
look, feel, and every function SmartSDR is capable of. The reference radio is a
**FLEX-8600 running firmware 4.2.18**, which speaks **SmartSDR protocol v1.4.0.0**.
Other radio families plug in behind `IRadioBackend` (see
[`docs/agents/backends.md`](docs/agents/backends.md)).

## AI Agent Guidelines

> **TEMPORARY — #5554.** Before opening any PR that touches
> `src/core/backends/`, `RadioModel`, `RadioSession`, `TransmitModel`,
> `ConnectionPanel`, discovery, or `RadioCapabilities`, read the #5554 notice
> in [`docs/agents/backends.md`](docs/agents/backends.md#standing-notice--5554):
> your change must not add to any item that review lists (family-string
> branches, raw Flex wire text above the seam, `dynamic_cast` to a concrete
> backend, a capability without a verb, copied HL2 scaffolding, a keying verb
> that skips the TX gate). This notice is removed when #5554's §2 items each
> have their own issue and #5262 M1 has landed.

When helping with AetherSDR:
- Prefer C++20 / Qt6 idioms (std::ranges, concepts if clean, Qt signals/slots over lambdas when possible)
- Keep classes small and single-responsibility
- Use RAII everywhere (no naked new/delete)
- When suggesting code: show **diff-style** changes or full function/class if small
- Test suggestions locally if possible — the build must work on Linux, macOS and Windows
- Never suggest Wine/Crossover workarounds — goal is native
- Flag any proposal that would break slice 0 RX flow
- If unsure about protocol behavior → ask for logs/wireshark captures first
- **Use `AppSettings`, never `QSettings`** — see [`docs/agents/settings.md`](docs/agents/settings.md)
- **New engine code goes in `libaethercore`** (`src/core/` or `src/models/`),
  exposed to the UI through models — never via a new gui→core header include.
- **Adding or changing UI?** Every colour is a ThemeManager token, and a
  control the radio lacks is dimmed with a stated reason, never hidden — see
  [`docs/agents/gui.md`](docs/agents/gui.md).
- **Read `CONTRIBUTING.md`** for contribution policy (what we accept, who
  reviews what) and `docs/DEVELOPER-GUIDE.md` for the contributor-facing
  coding conventions and the AI-to-AI debugging protocol (open a GitHub issue
  for cross-agent coordination)
- **Sign every commit you author.** `main` enforces `required_signatures`, so a
  PR with unsigned commits cannot merge without an admin override. If the
  contributor has not set up commit signing yet, walk them through
  `docs/COMMIT-SIGNING.md` **before** you commit — the top of that file is a
  step-by-step AI-assistant algorithm covering Windows / macOS / Linux / WSL /
  Raspberry Pi. Default to SSH signing; GPG is the fallback for existing GPG
  workflows. Verify with `git log --show-signature -1` after the first commit.
- **Read the AetherSDR Constitution before writing or reviewing code.**
  Canonical source: `.specify/memory/constitution.md`; byte-identical mirror at
  `CONSTITUTION.md`. AetherSDR-domain principles (FlexLib authority,
  radio-authoritative live state, radio-persistable settings, clean-room
  contributions, per-feature config ownership, transmit-on-intent, boundary
  input validation) plus defensive engineering principles adopted from Cisco's
  [Foundry Constitution](https://github.com/CiscoDevNet/foundry-security-spec/blob/main/constitution.md)
  (Evidence Over Assertion, Surface Only What Survives, Claims Are Atomic And
  Mortal, Fixes Are Demonstrated, Sandbox By Infrastructure, Operator Outranks
  Every Agent, Persist Atomically). Commit messages cite the most-load-bearing
  principle as `Principle <N>.` at the end of the subject line.

### Issue / PR Claim Protocol — Assign Yourself

When an AI agent is **actively reviewing an issue or PR — for comment, for
merge recommendation, or to implement a fix** — the agent MUST assign itself
using GitHub's `assignees` feature **before** posting the review, comment, or
merge action. This is the visible claim mechanism for multi-agent
coordination (Principle X: Claims Are Atomic And Mortal); it stops two agents
spending tokens on the same PR and posting conflicting recommendations.

1. **Before** posting a review, comment, or merge action, check the current
   assignees:
   - **Unassigned, or assigned ONLY to AetherClaude (`@aethersdr-agent`)**:
     add yourself alongside. AetherClaude auto-triages every new issue and PR,
     so its assignment is a triage signal, NOT a claim on active work.
   - **Already assigned to another human or AI agent** (not AetherClaude):
     leave a coordination comment instead of double-assigning, and do not
     proceed with overlapping work.
2. **While** working: stay assigned.
3. **After** finishing: the assignment can stay (GitHub clears it on
   close/merge). Unassign if you concluded but the issue/PR remains open.
4. **If your work is interrupted** (token limit, context loss, model failure):
   leave a brief comment ("Stepping away; unassigning so another agent can
   pick up") and unassign. The claim dies with the agent that held it.
5. **Quick read-only actions don't require assignment** (summarizing,
   listing, counting). Assignment is for engagement that produces a comment,
   review, or merge.

```bash
gh issue edit NNNN --add-assignee @me      # or: gh pr edit NNNN --add-assignee @me
gh issue edit NNNN --remove-assignee @me   # or: gh pr edit NNNN --remove-assignee @me
gh pr view NNNN --json assignees           # check current assignees
```

### Autonomous Agent Boundaries

> **Authority: [`GOVERNANCE.md`](GOVERNANCE.md) §AI Contributors.** That
> section defines these limits and is Tier 1 (maintainer-only). What follows
> is the worked-example elaboration for agent consumption — it may narrow or
> illustrate the limits, never widen them. If this list and GOVERNANCE.md
> differ, GOVERNANCE.md governs and the difference is a defect here. Do not
> relax any bullet below in a Tier-2 PR; that is an amendment to GOVERNANCE.md.

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
- **Includes**: carry them explicitly; never rely on a transitive include (Qt reshuffles transitive includes between releases, and a distro Qt newer than the 6.12 pin is a supported build).

### Comments

A comment states what is true **now** and why the code has this shape: the
invariant, the constraint, the non-obvious protocol fact (with firmware
version). Aim for five lines or fewer; a design that needs more goes in a
`docs/` file the comment links to.

History does not go in code. How the bug was found, what the code used to do,
which review caught it, who reported it, and the incident narrative belong in
the commit message and PR body, where `git blame` finds them. A bare `(#NNNN)`
tag is fine; retelling the issue is not.

Don't restate the code, don't add banner dividers, and don't cite
Constitution principles in code — that tag belongs in the commit subject.

Deleting or shortening a historical comment is welcome cleanup, as long as
the guard or test it describes stays. The guard and its test are the
protection; the prose is not.

## Build

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
./build/AetherSDR          # Windows: build\AetherSDR.exe
```

**Optional — DFNR (DeepFilterNet3) noise reduction.** Run
`scripts/setup/setup-deepfilter.sh` (Windows: `setup-deepfilter.ps1`) *once
before* `cmake` to fetch the prebuilt `libdeepfilter`; otherwise configure
reports `DFNR ... disabled — library not found` and gates the feature off.
Release workflows run this step automatically. NR still works without it —
RN2 (RNNoise) is bundled and always built.

**RNNoise architecture check.** The `third_party/rnnoise/src/x86` sources and
include directory belong only in x86 build graphs. After configuring any ARM
build, `rg 'rnnoise/src/x86' <build-dir>/build.ninja` must find no matches.

Full dependency list is in `README.md`. Adding a test: declare it in
`tests/tests.cmake` and do **not** add it to `ci.yml` — see
[`docs/agents/tests-ci.md`](docs/agents/tests-ci.md).

### Version and release files

Current version: **26.9.5**.
Versioning scheme is **CalVer** (`YY.M.patch[.hotfix]`) starting from v26.5.1,
the 1.0-equivalent. Hotfix sub-patches use a 4th component (e.g. 26.5.2.1).
Earlier tags used semver through v0.9.8.

The version is stated in **five** places, and a release is not prepped until
all five agree — check every row, not just the first two:

| file | what to change |
|---|---|
| `CMakeLists.txt` | `project(AetherSDR VERSION …)` — the only one that reaches the binary |
| `README.md` | the **Current version:** line |
| `AGENTS.md` | this line |
| `CHANGELOG.md` | a new section at the top, under `## [Unreleased]` |
| `packaging/linux/io.github.aethersdr.aethersdr.metainfo.xml` | a new `<release …/>` entry — AppStream and Flathub read this, not the git tag |

`ROADMAP.md`'s "Current cycle" heading names the release too.

Leave every *historical* mention alone. "shipped v26.7.4" and "(v26.7.4)" are
statements about when something landed and stay true forever, so a blanket
find-and-replace across a version bump silently corrupts them.

### `CHANGELOG.md` is a release-prep file. Do not touch it in a feature PR.

The table above is the **only** reason to edit `CHANGELOG.md`: a new version
section, at release prep. An ordinary PR — a fix, a feature, a refactor —
**must not add an entry**, however user-visible the change is. Describe it in
the PR body and the commit message instead. At release prep, the section is
written *from* those PR bodies — `gh pr list --state merged --search
'merged:>=<last-tag-date>'` is the source of truth for what shipped.

Every entry is prepended to the same `## [Unreleased]` list, so any two PRs
that both add one conflict with each other. Reviewers: do not ask for a
`CHANGELOG.md` entry, and flag one as a change to remove if a PR adds it.

## CI and branch protection

Required checks on `main`: `build`, `check-windows`, `check-macos`, and
`Static checks` (each job runs ~10 min; a PR run is ~15 min wall-clock). Signed
commits required, CODEOWNERS review required, conversations must be resolved,
stale approvals are dismissed on push, branches auto-delete after merge. CI
runs in Docker image `ghcr.io/aethersdr/aethersdr-ci:latest`; a new
`find_package(...)` needs its `-dev` package in `.github/docker/Dockerfile`.
Test registration, the frozen per-PR test gate, and gate integrity:
[`docs/agents/tests-ci.md`](docs/agents/tests-ci.md).

## Architecture Overview

Key source directories: `src/core/` (protocol, audio, DSP), `src/models/`
(RadioModel, SliceModel, etc.), `src/gui/` (MainWindow, SpectrumWidget,
applets), `src/core/backends/` (one directory per radio family).

**Key classes:**
- `RadioModel` — central state, owns connection + all sub-models
- `RadioSession` — per-radio aggregate that owns `RadioModel` + `TciServer` +
  `CatPorts`, giving teardown a structural order
- `AudioEngine` — RX/TX audio, NR2/RN2/NR4/BNR/DFNR DSP pipeline
- `SpectrumWidget` — GPU-accelerated FFT spectrum + waterfall (QRhiWidget)
- `MainWindow` — wires everything together; one class across `MainWindow.cpp`
  + `MainWindow_*.cpp` sibling TUs. New feature code goes in a sibling, NOT
  `MainWindow.cpp` ([`docs/agents/gui.md`](docs/agents/gui.md))
- `PanadapterStream` — VITA-49 UDP parsing, routes FFT/waterfall/audio/meters
- `IRadioBackend` — the radio seam; six implementors (flex, hl2, icom, sim,
  anan, rtl) ([`docs/agents/backends.md`](docs/agents/backends.md))

**Threading:** up to 13 threads — see `docs/architecture/pipelines.md` for the
full thread diagram, data flow, cross-thread signal map, and GPU rendering notes.

**Design principle:** RadioModel owns all sub-models on the main thread.
Worker threads communicate exclusively via auto-queued signals. Never hold
a mutex in the audio callback.

**Build targets:** `libaethercore` (`src/core/` + `src/models/`, never `gui/`
or QtWidgets), `AetherSDR` (the desktop app), `aetherd` (headless daemon,
never QtWidgets). The dependency direction is CI-enforced; details in
[`docs/agents/backends.md`](docs/agents/backends.md).

## Where moved sections live

Older code comments and docs cite sections of this file by name. They now
live here:

| Section | Now in |
|---|---|
| "Adding a test", "Test-layer boundary", "Gate integrity", socket-test carve-out | [`docs/agents/tests-ci.md`](docs/agents/tests-ci.md) |
| "In-flight: aetherd engine/UI decoupling", "Engine boundary ratchet — EB3", the conformance checklist, build-target table, seam routing | [`docs/agents/backends.md`](docs/agents/backends.md) |
| "Settings Persistence", "Radio-Scoped Feature Documents", "Client-Side Radio State Memory", "Settings Migration", "Settings Authority Policy" ("do NOT persist") | [`docs/agents/settings.md`](docs/agents/settings.md) |
| "SmartSDR Protocol", "GUI↔Radio Sync", "Auto-Reconnect", "Optimistic Updates Policy", "Multi-Panadapter", "Multi-Client", "KiwiSDR" | [`docs/agents/flex-protocol.md`](docs/agents/flex-protocol.md) |
| "Key Implementation Patterns" | split by topic: settings rules → `docs/agents/settings.md`; widget, MainWindow and band-data rules → `docs/agents/gui.md`; sync/reconnect/optimistic updates → `docs/agents/flex-protocol.md` |
| "Adding code to MainWindow", "Adding or converting a dialog", "Accessibility", three-state controls, "Meter Smoothing", "User-facing names", "Region-aware band data", "CHAIN widget", "Contributors list", "Agent Automation Bridge" | [`docs/agents/gui.md`](docs/agents/gui.md) |
