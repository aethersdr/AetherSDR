# AGENTS.md staged updates — aetherd migration

Pre-drafted `AGENTS.md` blocks for the aetherd migration
([`aetherd-headless-engine-design.md`](aetherd-headless-engine-design.md)).
`AGENTS.md` is executable law for this repo's AI contributors: it must
describe the tree as it **is**, never as the RFC hopes it will be. So each
block below lands in `AGENTS.md` **in the same PR** as the milestone it
describes — never earlier, never later.

**Landing rules:**

1. **Atomic.** A milestone PR that changes structure without its
   `AGENTS.md` block is incomplete and must not merge.
2. **Reconcile before landing.** File names, script names, and CI check
   names below are proposals. If the implementation chose different names,
   fix the block, not the code.
3. **Burn down.** When a block lands in `AGENTS.md`, delete it from this
   file. When this file is empty, delete it and drop the pointer to it
   from `AGENTS.md` and the RFC.
4. **Track the RFC.** If a §10 step changes shape during sign-off or
   implementation, update its block here in the same PR that changes
   the RFC.

---

## Block 1 — LANDED 2026-07-04 with RFC step 1 (`libaethercore` extraction)

> This block has been merged into `AGENTS.md` (In-flight subsection +
> build-targets table + agent-guidelines bullet), reconciled to the real
> target name (`aethercore`) and the EB1-now-zero state. Kept here for
> history; delete on the next staging-doc edit.

### 1a. In the "In-flight: aetherd engine/UI decoupling" subsection, replace the status sentence

> Implementation follows the RFC's §10 staged order; **step 1
> (`libaethercore`) has landed** — the engine is a library, the app is a
> shell that links it. Steps 2+ have not landed.

### 1b. Append to "Architecture Overview"

```markdown
**Build targets (post-RFC step 1):**

| Target | Contents | May link |
|---|---|---|
| `libaethercore` | `src/core/` + `src/models/` — the engine | Qt Core/Network/Multimedia/WebSockets, DSP libs. **Never `gui/`, never Qt Widgets** |
| `AetherSDR` | `src/gui/` + `main.cpp` — the desktop app | `libaethercore` + Qt Widgets |

The engine→gui dependency direction is now CI-enforced
(`tools/check_engine_boundary.py`, warning-free build required): no file
under `core/` or `models/` may include a `gui/` header, and
`libaethercore` may not link `Qt6::Widgets`. If your change trips the
check, restructure the change — do not move the file, weaken the check,
or add an exemption.
```

### 1c. Append one bullet to "AI Agent Guidelines"

```markdown
- **New engine code goes in `libaethercore`** (`src/core/` or
  `src/models/`), exposed to the UI through models — never via a new
  gui→core header include. See "Build targets" under Architecture
  Overview.
```

---

## Block 2 — PARTIALLY LANDED with RFC step 2.2 (FlexBackend skeleton)

> The In-flight status (2a) and the "where radio-facing code goes" routing
> table (a trimmed 2b) landed in `AGENTS.md` with the 2.2 FlexBackend
> skeleton, reconciled to the skeleton reality (FlexBackend exists but the
> wire stack hasn't moved behind it yet). **Still pending, to land when 2.3
> per-touchpoint conversions begin:** the touchpoint-claim protocol, the
> before/after `verify_slice0_rx.py` verification recipe, and the
> Autonomous Agent Boundaries carve-out (2c) that authorizes agents to
> convert enumerated touchpoints — premature before the first conversion
> proves the process. The blocks below are what remains for 2.3.

### 2a. In the "In-flight" subsection, replace the status sentence

> Implementation follows the RFC's §10 staged order; **steps 1–2 have
> landed** — `libaethercore` and the `IRadioBackend` seam exist, and the
> SmartSDR stack lives in `FlexBackend`. The versioned protocol (step 3+)
> has NOT landed: UI code still calls models directly, and that remains
> correct.

### 2b. New top-level section, placed after "Adding code to MainWindow"

```markdown
### Engine boundary & radio backends (aetherd migration)

**Status:** RFC steps 1–2 landed. `libaethercore` + `IRadioBackend`
exist; the SmartSDR stack is `FlexBackend`
(`src/core/backends/flex/`). The versioned protocol has NOT landed —
UI code still consumes models directly, and that remains correct.

Full design:
[`docs/aetherd-headless-engine-design.md`](docs/aetherd-headless-engine-design.md).
Touchpoint burndown:
[`docs/architecture/aetherd-touchpoints.md`](docs/architecture/aetherd-touchpoints.md).

Route your change:

| Your change | Goes |
|---|---|
| Code speaking a vendor wire protocol (commands, discovery, stream parsing) | that family's backend under `src/core/backends/<family>/`, behind `IRadioBackend` — never in models, never in `gui/` |
| A new radio family | a new `IRadioBackend` implementation — requires an approved design doc naming its open protocol authority (Constitution Principles I & IV apply per backend) |
| New engine feature | `libaethercore`, exposed through models — never via a new gui→core header |
| A new gui→engine touchpoint | **don't create one** — consume an existing model surface; if none fits, stop and flag the maintainer (it's a protocol-surface decision, RFC §2) |
| Converting an enumerated touchpoint | claim it first (below); one touchpoint per PR; run the verification recipe |

**Ratchets (CI-enforced — restructure your change rather than working
around them):**

- `core/`+`models/` include no `gui/` headers; `libaethercore` never
  links `Qt6::Widgets` (`tools/check_engine_boundary.py`)
- Only backends touch vendor wire protocols; models hold normalized
  state only
- The touchpoint manifest's "converted" column only ever grows

**Claiming a touchpoint:** the Issue/PR Claim Protocol (above) applies
at touchpoint granularity. Assign yourself on the migration tracking
issue and mark the manifest row "in progress" in your PR's first
commit. One touchpoint per PR. If you step away, revert the row and
unassign — claims are mortal (Principle X).

**Verification recipe (required in every conversion PR):** conversions
must be behavior-neutral. Build, launch with `AETHER_AUTOMATION=1`, and
run `tools/verify_slice0_rx.py` (bridge-driven: connect, assert
slice-0 frequency/mode/filter round-trip via `get`, `grab` the
panadapter) before and after your change. Paste the script's summary
in the PR body. "It compiles" is not evidence (Principle VIII).
```

### 2c. Amend "Autonomous Agent Boundaries"

In the "must NOT autonomously change" list, extend the
**Architecture** bullet:

```markdown
- **Architecture** — adding new threads, changing signal routing, new
  dependencies. *One carve-out:* converting an already-enumerated
  touchpoint per the approved aetherd RFC **is** authorized mechanical
  work (claimed via the manifest, behavior-neutral, bridge-verified —
  see "Engine boundary & radio backends"). Creating new protocol
  surface, new touchpoints, new backends, or reordering RFC steps
  remains maintainer-only.
```

---

## Block 3 — land with RFC steps 3–5 (protocol, TX arbitration, data plane)

### 3a. In the "Engine boundary & radio backends" section, replace the status paragraph

> **Status:** RFC steps 1–5 landed. The versioned control protocol,
> engine-side TX arbitration, and the binary data plane exist. Subsystems
> are converting to the protocol surface one at a time — **check the
> manifest before writing UI code.**

### 3b. Add two rows to the routing table

```markdown
| UI feature code in a **converted** subsystem (per the manifest) | consume the protocol surface (subscribe / verbs / data-plane frames) — do not add direct model calls back |
| UI feature code in an **unconverted** subsystem | the legacy direct-model path is still correct — do NOT half-convert a subsystem as a side effect of a feature PR |
```

### 3c. Add a TX must-know bullet to the section

```markdown
- **TX keying goes through engine-side arbitration only** (RFC §6):
  single-holder TX lock, per-client auth, fail-closed. Never add a
  keying path that bypasses the guard; backends translate the engine's
  keying decision, they never self-key. Anything TX-adjacent gets the
  `safety` label and maintainer review — same rule as today, one layer
  down.
```

### 3d. In the "In-flight" subsection (top of Architecture Overview)

Replace the whole subsection with two sentences: the migration is in its
conversion phase, rules live in "Engine boundary & radio backends", and
the burndown manifest is the source of truth for per-subsystem status.
