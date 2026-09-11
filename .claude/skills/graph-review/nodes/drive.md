# Node: drive — verify runtime claims against the running app

Read `stance.md` first. Inputs: the head build dir (and base build dir if
one exists), the PR number, a scratch dir, and a list of **runtime claims**
gathered from the PR body and the audit nodes, each with a suggested way to
test it. **Any claim about runtime behavior gets tested against the running
app, not argued from the diff.** UI state, a control's effect, a dialog's
lifecycle, what the panadapter renders, connect/disconnect and slice-recreate
paths, whether a setting survives a restart — the automation bridge reaches
all of it. `docs/automation-bridge.md` is the reference; it is written for
agents and is copy-pasteable.

## Demo mode, never the live radio

**Every bridge session runs against the built-in demo simulator
(`SimBackend`, RFC #4288) — never the operator's FLEX-8600.** The demo
generates its own RX audio and matching panadapter, exercises the same
`RadioModel` / slice / pan / settings paths as real hardware, and by
construction **cannot key** (Principle VI).

The trap is that this is opt-*out*: `AutoConnectToLastRadio` defaults on, and
on the operator's machine the saved settings point at the real radio, so an
instance launched with the default config **will connect itself to the live
FLEX** before you issue a verb. Two things prevent it, and you do both:

1. **Isolate the settings store** — `AETHER_SETTINGS_DIR` pointed at a fresh
   scratch dir. A clean store also leaves `ShowDemoRadio` at its default
   (on), so the demo entry is in the list.
2. **Connect explicitly to the demo, and to nothing else.** Its discovery
   serial is `DEMO-0001` (`SimBackend::demoSerial()`), family `sim`, shown
   as "Simulator (not on the air)".

```sh
export SCRATCH=<your scratch dir>
AETHER_AUTOMATION=1 QT_QPA_PLATFORM=offscreen \
AETHER_SETTINGS_DIR="$SCRATCH/settings" \
AETHER_AUTOMATION_IDENTITY=pr-<PR>-review \
AETHER_AUTOMATION_SOCKET=aethersdr-pr<PR> \
AETHER_AUTOMATION_NO_TX=1 \
setsid nohup <buildDir>/AetherSDR >"$SCRATCH/app.log" 2>&1 &
```

Then, before anything else:

```text
connect list                      → verify DEMO-0001 is offered
connect local serial DEMO-0001
connect wait 30000                → asserts connected, returns the radio block
get radio                         → confirm model "AetherSDR Demo", family sim
```

If `get radio` ever shows a FLEX serial, you are on the operator's hardware:
`disconnect` immediately, fix the isolation, and report it in `incidents`.

The `sim` verb injects faults while connected (`swr`, `dropslice`,
`stallscope`, `disconnect`, `malformed`, `clear`) — that is how you exercise
error handling rather than only reading it. Its limits, so you don't mistake
one for a finding: it advertises a **single slice**, it cannot transmit, and
it does not implement every Flex-specific protocol surface. When a claim
genuinely cannot be reached in demo mode, return `outcome: unreachable` with
the reason. That is **not** a licence to reach for the real radio.

## Driving it

Use the `aethersdr-automation` MCP tools (load via ToolSearch):
`bridge_status` → `dump_tree filter=<widget>` → `invoke` / `gesture` /
`shortcut` / `tune` / `slice` / `menu` → `assert_state` / `wait_for` on the
model property that should have changed → `grab_widget` when the claim is
visual. `bridge_command` is the escape hatch for verbs without a typed tool.
If MCP is unavailable, `python3 tools/automation_probe.py` or raw
line-delimited JSON over the socket does the same job — the bridge being
awkward to reach is not a reason to skip it.

The loop that produces evidence, per claim:

1. **Reproduce on the merge base first** when the PR claims a fix and a base
   build exists. A "before" you cannot make fail is itself a finding — either
   the repro is wrong or the fix is fixing nothing.
2. **Re-run identically on the PR head.** Prefer `assert_state`/`wait_for`
   over eyeballing a screenshot. Grab pixels when the claim is about pixels.
3. **Then attack past the happy path**, which the author almost certainly
   did not drive: the second slice, the second pan, disconnect
   mid-operation, the dialog closed and reopened, the value at its range
   limits, the same action twice. Persistence claims are only proven across
   a process boundary — relaunch and re-read.
4. **Quote what the app returned** — the `get_state`/`assert_state` JSON,
   the `get_log` lines, the PNG path — verbatim in `evidence`.

Anything you break that no claim predicted goes in `newFindings` with the
same finding shape as the audit nodes, `needsRuntime: false`, and the
evidence attached.

Non-negotiable operating rules:

- **`pgrep -a AetherSDR` first.** Instances that are not yours — especially
  any launched from the shared checkout — are the operator's session. Never
  drive, close, or kill one.
- **Always pass an explicit socket.** The discovery file is
  last-writer-wins and will point you at another agent's instance.
- **Never key TX.** Launch with `AETHER_AUTOMATION_NO_TX=1`; a review never
  needs the transmit verbs.
- **Close your instance when done** and record which instance you drove and
  that it was the demo (`instance` in the output).

When you genuinely cannot drive — the build failed, the change is headless —
return `drove: false` with the reason and leave every claim `unreachable`.
An unverified claim reported as unverified is honest; one reported as
observed is not.
