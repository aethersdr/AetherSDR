# Contributing to MasterSDR

Thanks for your interest in MasterSDR! We're building a native SmartSDR
client for FlexRadio on Linux, macOS, and Windows. Community contributions
are welcome.

See [GOVERNANCE.md](GOVERNANCE.md) for project roles, decision-making, and
the RFC process for significant changes.

---

## Quick Start

1. Browse [open issues](https://github.com/MasterSDR/MasterSDR/issues) —
   issues labeled `good first issue` are great starting points.
2. Fork the repo and create a feature branch from `main`.
3. Implement the fix or feature (one issue per PR).
4. Open a pull request referencing the issue number (`Fixes #42`).

---

## Reporting Bugs

- Use the **lightbulb button** in MasterSDR's title bar for AI-assisted bug
  reports, or open a [GitHub issue](https://github.com/MasterSDR/MasterSDR/issues/new) directly.
- Include: OS/distro, MasterSDR version, radio model, firmware version.
- Attach logs (`~/.config/MasterSDR/MasterSDR.log`) or use Help → Support → Send to Support.
- Check existing issues first to avoid duplicates.

## Suggesting Features

- Open a GitHub issue or use the lightbulb button for an AI-assisted feature request.
- Describe the problem you're solving, not just the solution.
- Reference SmartSDR behavior where applicable — screenshots help.
- One feature per issue.

---

## Submitting Code

**Development tool:** MasterSDR is developed using [Claude Code](https://claude.com/claude-code)
as the primary development environment. We **strongly encourage all contributors to use
Claude Code** — it has full codebase context via `CLAUDE.md` and naturally produces code
that matches our conventions.

1. **Fork the repo** and create a feature branch from `main`.
2. **One issue per PR.** Keep changes focused and reviewable.
3. **Follow the coding conventions** below.
4. **Test your changes** against a real FlexRadio if possible.
5. **Sign your commits** with GPG (required by branch protection).
6. **Open a pull request** against `main` with a clear description.

---

## Project Architecture

The full architecture is documented in [CLAUDE.md](CLAUDE.md) including
the complete file tree, data pipelines, thread architecture (11 threads),
protocol specification, and implementation patterns. Read it before making
changes.

### Key Patterns

- **Model → Radio**: Model setters emit `commandReady(cmd)` →
  `RadioModel` sends to radio via TCP.
- **Radio → Model**: Status messages (`S` lines) → `RadioModel::onStatusReceived()`
  → routes to model's `applyStatus()`.
- **Model → GUI**: Models emit signals → GUI widgets update via slots.
- **GUI → Model**: GUI widgets call model setters. Use `QSignalBlocker` or
  `m_updatingFromModel` guards to prevent echo loops.
- **Settings**: Use `AppSettings`, **never** `QSettings`. Keys are PascalCase.
  Booleans are `"True"` / `"False"` strings.
- **Radio-authoritative**: Never persist or override settings the radio manages
  (frequency, mode, filter, step size, AGC, antennas, TX power).

### Thread Architecture

| Thread | Components |
|--------|-----------|
| **Main** | GUI rendering, RadioModel, all sub-models, user input |
| **Connection** | RadioConnection (TCP 4992 I/O) |
| **Audio** | AudioEngine (RX/TX audio; NR2/RN2/NR4/DFNR/BNR/MNR DSP) |
| **Network** | PanadapterStream (VITA-49 UDP parsing) |
| **ExtControllers** | FlexControl, MIDI, SerialPort |
| **Spot** | DX Cluster, RBN, WSJT-X, POTA, FreeDV clients |

Cross-thread communication uses auto-queued signals exclusively.

### Multi-Flex (Multi-Client) Safety

When another client (SmartSDR, Maestro) is connected, filter all status
updates and VITA-49 packets by `client_handle`. Do not process data from
other clients' slices or panadapters.

---

## SmartSDR Protocol Reference

ASCII over TCP (port 4992) + VITA-49 binary over UDP.

| Prefix | Direction | Meaning |
|--------|-----------|---------|
| `V` | Radio→Client | Firmware version |
| `H` | Radio→Client | Client handle (hex) |
| `C` | Client→Radio | Command: `C<seq>\|<cmd>\n` |
| `R` | Radio→Client | Response: `R<seq>\|<hex_code>\|<body>` |
| `S` | Radio→Client | Status: `S<handle>\|<object> key=val ...` |
| `M` | Radio→Client | Informational message |

### FlexLib Reference

The FlexLib C# source at `~/build/FlexLib/` is the authoritative protocol
reference. Use it to understand behavior, but **write clean-room C++** —
do not copy-paste.

Key files: `Slice.cs`, `Radio.cs`, `Panadapter.cs`, `Transmit.cs`,
`Meter.cs`, `APD.cs`, `TNF.cs`, `CWX.cs`, `DVK.cs`.

---

## Coding Conventions

### C++ Style

- **C++20 / Qt6** — modern idioms (`std::ranges`, `auto`, structured bindings).
- **RAII everywhere.** No naked `new`/`delete`. Use Qt parent-child ownership.
- **Qt signals/slots** for cross-object communication.
- **`QSignalBlocker`** to prevent feedback loops.
- **Keep classes small** and single-responsibility.

### Naming

- Classes: `PascalCase` (`SliceModel`, `SpectrumWidget`)
- Methods: `camelCase` (`setFrequency()`, `applyStatus()`)
- Members: `m_camelCase` (`m_frequency`, `m_sliceId`)
- Signals: past tense (`frequencyChanged`, `commandReady`)
- AppSettings keys: `PascalCase` (`LastConnectedRadioSerial`)

### Widget Guidelines

- All GUI follows the dark theme: `#0f0f1a` background, `#c8d8e8` text,
  `#00b4d8` accent, `#203040` borders.
- Use `GuardedSlider` (from `GuardedSlider.h`) instead of `QSlider` — it
  prevents wheel events from leaking to parent widgets.
- Use `GuardedComboBox` for combo boxes in scrollable areas.
- Disable `autoDefault` on QPushButtons inside QDialogs.

### Optional Dependencies

Features gated behind compile-time flags:

| Flag | Package | Feature |
|------|---------|---------|
| `HAVE_SERIALPORT` | `Qt6::SerialPort` | FlexControl, serial PTT/CW |
| `HAVE_WEBSOCKETS` | `Qt6::WebSockets` | FreeDV Reporter, TCI server |
| `HAVE_KEYCHAIN` | `Qt6Keychain` | SmartLink credential persistence |
| `HAVE_MIDI` | Bundled RtMidi | MIDI controller mapping |
| `HAVE_RADE` | Bundled RADE/Opus | FreeDV digital voice |
| `HAVE_SPECBLEACH` | libspecbleach (clang-cl on Win) | NR4 spectral noise reduction |
| `HAVE_DFNR` | Bundled DeepFilterNet3 | DFNR neural noise reduction |
| `HAVE_BNR` | NVIDIA NIM container | GPU noise removal |
| `HAVE_MQTT` | Bundled libmosquitto | MQTT applet |

Use `#ifdef HAVE_*` guards. Features must degrade gracefully when unavailable.

### Commit Messages

- Imperative mood: "Add band stacking" not "Added band stacking".
- First line under 72 characters.
- Reference issues: `Fixes #42` or `Closes #42`.

### Commit Signing

All commits to `main` must be GPG-signed. Setup:

```bash
# Generate key
gpg --quick-gen-key "Your Name <email@example.com>" ed25519 sign 0

# Add to GitHub: Settings → SSH and GPG keys → New GPG key
gpg --armor --export <KEY_ID>

# Configure git
git config --global user.signingkey <KEY_ID>
git config --global commit.gpgsign true
```

If `gpg` hangs, set `export GPG_TTY=$(tty)` in your shell profile.

---

## Reviews and merging

PR review responsibility is divided into three tiers via
[`.github/CODEOWNERS`](.github/CODEOWNERS). Self-approval is blocked by
GitHub on every tier — your own PR always needs review from someone else.

| Tier | Paths | Who can approve |
|---|---|---|
| **Default** | Everything not listed below | @ten9876, @jensenpat |
| **Mechanical / safe** | `tests/`, `docs/`, `*.md`, `.github/dependabot.yml`, `.github/docker/`, `.github/ISSUE_TEMPLATE/` | @ten9876, @jensenpat, @MasterClaude |
| **Maintainer-only** | `src/gui/MainWindow.{h,cpp}`, `src/core/RadioModel.{h,cpp}`, `src/core/AudioEngine.{h,cpp}`, `src/core/PanadapterStream.{h,cpp}`, `CMakeLists.txt`, `CLAUDE.md`, `CONTRIBUTING.md`, `.github/CODEOWNERS`, `.github/workflows/` | @ten9876 |

The maintainer-only tier covers *direction-impacting* paths: visual/UX,
threading and central-state architecture, protocol bedrock, build
configuration, and project policy. Per
[CLAUDE.md](CLAUDE.md#autonomous-agent-boundaries), changes here need
maintainer eyes regardless of who wrote them.

The mechanical tier exists so the @MasterClaude bot can land low-risk
changes (test additions, documentation tweaks, dependency bumps,
template updates) without queueing on human review.

### Draft PR conventions

Draft status carries different meaning depending on who opened the PR:

- **Human-authored draft** — work-in-progress; reviewers should skip
  these until the author marks Ready for Review.
- **`@MasterClaude` / `MasterSDR-agent[bot]` draft** — auto-generated
  from an issue and **awaiting human review**. The draft state holds
  the PR back from auto-merge; it is not "WIP". Treat it like a
  ready-to-review PR for triage purposes.

Triage scripts and review agents should include bot drafts in their
sweep and skip only human drafts.

### Stale-branch policy

We **do not** require PR branches to be up to date with `main` before
merging. The reasoning:

- Squash-merge already runs a fresh three-way merge against `main`, so
  textual conflicts are caught at merge time regardless of branch age.
- Forcing every PR to rebase after every other merge cost ~15–25 min
  of CI per stale PR per batch day, which adds up fast when MasterClaude
  is processing a queue of triaged issues.
- Post-merge CI on `main` runs on every commit (see
  [`.github/workflows/ci.yml`](.github/workflows/ci.yml) and
  [`.github/workflows/codeql.yml`](.github/workflows/codeql.yml)), so
  semantic conflicts that slip through three-way merge are caught on
  the merged result within ~10 min.

### Recovering from a red main

If post-merge CI on `main` fails after a merge:

1. Check the failing workflow run linked from the email/GitHub
   notification. Identify the offending merge commit.
2. **Prefer fix-forward** if the issue is small (one or two file edits):
   open a normal PR titled `fix(ci): repair main after <SHA>` and let
   it land through the usual flow.
3. **Use revert** if fix-forward isn't obvious or the regression is
   broad: `git revert -m 1 <merge-sha>` on a new branch, push, open a
   PR, merge. Never force-push `main`.
4. For agent-authored regressions: re-open the source issue, remove
   the `MasterClaude-eligible` label, then re-add it. The orchestrator's
   State Override C (`failed` → `implement` re-entry) creates a fresh
   worktree from current `main` and retries the implementation.

## What We Will Not Accept

- **Wine/Crossover workarounds.** The goal is fully native.
- **Copied proprietary code.** Clean-room implementations from observed
  protocol behavior and FlexLib source are fine.
- **Changes that break the core RX path.** Test: discovery → connect →
  FFT display → audio output.
- **Large reformatting PRs.** Fix style only in files you're modifying.
- **UX, visual, or architecture changes without an approved RFC.** Open
  a `[RFC]` issue first — see [GOVERNANCE.md](GOVERNANCE.md).

---

## AI-Assisted Feature Requests

**You don't need to be a developer to contribute.** Click the lightbulb
button in MasterSDR's title bar — it copies a structured prompt to your
clipboard and opens your choice of AI assistant. Describe your idea in
plain English, and the AI generates a well-structured GitHub issue.

### What makes a good request

- **Be specific.** "Add a noise gate with adjustable threshold" not "better audio."
- **Describe the problem.** Tell us *why*, not just *what*.
- **Reference SmartSDR.** Screenshots of the Windows client are very helpful.
- **One feature per issue.**

---

## Notes for AI Agents

Read [CLAUDE.md](CLAUDE.md) first — it is the authoritative project context.

### Quick reference

| Task | Start here |
|------|-----------|
| New slice property | `SliceModel.h/.cpp` — getter/setter/signal, parse in `applyStatus()` |
| New TX property | `TransmitModel.h/.cpp` — same pattern |
| New GUI control | `RxApplet.cpp` for patterns, `VfoWidget.cpp` for tab panels |
| New applet | Copy `EqApplet` as template, register in `AppletPanel` |
| New overlay sub-menu | `SpectrumOverlayMenu.cpp` — `buildBandPanel()` as template |
| New status object | `RadioModel::onStatusReceived()` — add routing |
| New meter display | `MeterModel` parses all meters — wire to a gauge |
| New Radio Setup tab | `RadioSetupDialog.cpp` — follow existing tab patterns |
| New spot source | `DxClusterDialog.cpp` — follow existing tab patterns |
| Protocol command | Check FlexLib for syntax, test with radio logs |

### AI-to-AI coordination

If your AI agent hits an issue requiring maintainer coordination, open a
GitHub issue with: your analysis, relevant log output, code references,
and proposed fix. The maintainer's Claude instance monitors issues and
will respond.

---

## Code of Conduct

Be respectful, constructive, and patient. Ham radio has a long tradition
of helping each other learn — bring that spirit here.

73 de KK7GWY

## License

By contributing to MasterSDR, you agree that your contributions will be
licensed under the [GNU General Public License v3.0](LICENSE).
