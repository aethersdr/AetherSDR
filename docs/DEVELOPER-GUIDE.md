# AetherSDR Developer Guide

The working reference for writing code in this repository: architecture,
the SmartSDR protocol, coding conventions, and commit signing.

This is the *operational* half of contributing. The policy half — what we
accept, who reviews what, the Constitution requirement, the Code of Conduct
— stays in [`CONTRIBUTING.md`](../CONTRIBUTING.md). Read that first if you
have not contributed here before.

For the exhaustive version of everything below, [`AGENTS.md`](../AGENTS.md)
is the canonical project guide (~830 lines). This document is the contributor-
facing condensation.

---

## Project Architecture

The full architecture is documented in [`AGENTS.md`](../AGENTS.md) including
the complete file tree, data pipelines, thread architecture (12 threads),
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
  Booleans are `"True"` / `"False"` strings. The store is SQLite
  (`AetherSDR.db`, RFC #4603) — never include `sqlite3.h` outside
  `SettingsDatabase.cpp`, and **never put a credential in the settings
  store**: QtKeychain only (see AGENTS.md "Settings Persistence").
- **Settings authority is capability-shaped** (RFC #4603): on a radio that
  persists its own state (Flex), never persist or override radio-managed
  settings client-side (frequency, mode, filter, AGC, TX power, per-pan
  state, …) and never write a radio-echoed status value into a setter that
  also persists (the recurring #4261 anti-pattern). On a radio that persists
  NOTHING (HL2), the client is its memory — but only for the domains the
  backend declares in `RadioCapabilities::clientSettingsDomains`, and only
  through `RadioStateMemory`'s document, never flat `AppSettings` keys or
  ad-hoc paths. See AGENTS.md "Settings Authority Policy" for the full rules.
- **Radio-scoped config** goes in `radio_settings` feature documents via
  `RadioModel::settingsScope()` — one versioned JSON document per feature
  (Principle V), atomic whole-document writes, write failures surfaced. See
  AGENTS.md "Radio-Scoped Feature Documents".

### Working in MainWindow

`MainWindow` was a ~19,500-line monolith; **#3351 decomposed it** into one class
spread across `MainWindow.cpp` + a family of nine `MainWindow_*.cpp` sibling TUs
(controllers, menus, shortcuts, wiring, digital modes, SWR sweep, spots,
session, DSP applets). It's still one class — the siblings hold `MainWindow::`
method bodies.

**Don't add new feature code to `MainWindow.cpp`.** Put a feature's
lifecycle/handlers in the matching sibling TU, signal wiring in
`MainWindow_Wiring.cpp`, and reserve `MainWindow.{h,cpp}` for genuinely
cross-cutting code. The full TU map and a "where does my change go?" table are in
[`docs/architecture/mainwindow-decomposition.md`](architecture/mainwindow-decomposition.md)
— read it before touching anything named `MainWindow*`.

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
reference (Constitution Principle I). Use it to understand behavior, but
**write clean-room C++** — do not copy-paste.

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

All commits to `main` must be signed (branch protection enforces this).
SSH and GPG signing are both supported; **SSH signing is recommended**
if you already push via SSH because it reuses your existing key.

**Full setup guide:** [`docs/COMMIT-SIGNING.md`](COMMIT-SIGNING.md)
— covers Windows, macOS, Linux, WSL, and Raspberry Pi OS, with both
SSH and GPG paths. **The top of that doc has explicit AI-assistant
instructions**, so if you'd rather have your AI coding assistant walk
you through setup, just tell it
*"read `docs/COMMIT-SIGNING.md` and help me set up commit signing"*
and it will follow the algorithm there.

#### Quick reference (SSH signing, the simple path)

```bash
# 1. Confirm or generate an SSH key
ls -la ~/.ssh/id_ed25519.pub || ssh-keygen -t ed25519 -C "you@example.com"

# 2. Configure git to sign with it
git config --global gpg.format ssh
git config --global user.signingkey ~/.ssh/id_ed25519.pub
git config --global commit.gpgsign true
git config --global tag.gpgsign true
git config --global user.email "you@example.com"   # must match GitHub

# 3. Register the key on GitHub
cat ~/.ssh/id_ed25519.pub
# Paste at GitHub > Settings > SSH and GPG keys > New SSH key
# Set Key Type: "Signing Key" (NOT Authentication — that's a different
# role on the same key; you may need both entries for the same pubkey)

# 4. Verify
git commit --allow-empty -m "signing test"
git log --show-signature -1   # expect "Good \"git\" signature"
```

For GPG, Windows-specific tweaks, Touch ID integration on macOS, or
troubleshooting "Unverified" badges, see
[`docs/COMMIT-SIGNING.md`](COMMIT-SIGNING.md).

---

## Notes for AI Agents

Read [`AGENTS.md`](../AGENTS.md) first — it is the authoritative project
context for every AI tool. Policy that constrains agent behaviour lives in
[`CONSTITUTION.md`](../CONSTITUTION.md) and [`GOVERNANCE.md`](../GOVERNANCE.md),
which outrank both this file and `AGENTS.md`.

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
