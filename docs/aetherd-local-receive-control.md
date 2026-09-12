# AetherD v1 — local receive-control milestone

This Stage 3 milestone of #3849 extends the existing local opt-in with typed
controls for existing owned receivers. It does not enable transmit, remote
access, arbitrary commands, new slices/pans, or desktop migration. The default
daemon remains observe-only. `--allow-local-control` grants all current-user
local clients observe and non-TX control, not per-application consent.

## Requests

Every request uses the negotiated `sessionId` and requires `control` before
schema validation or resource lookup. Unknown fields are rejected.

| Method | Required parameters, in addition to `radioSession` and `expectedRevision` |
|---|---|
| `slice.setMode` | `slice`, `mode` |
| `slice.setFilter` | `slice`, `lowHz`, `highHz` |
| `slice.setAudioGain` | `slice`, `gain` |
| `slice.setAudioMute` | `slice`, `muted` |
| `panadapter.setCenter` | `panadapter`, `hz` |
| `panadapter.setBandwidth` | `panadapter`, `hz` |

`radioSession` must identify the current `radio-1`. Slice IDs are canonical
nonnegative decimal strings fitting int32; pan IDs are exact opaque resource
IDs, not backend IDs. `expectedRevision` is the addressed slice/pan revision,
a positive exact JSON integer at most 2^53-1. Numeric strings, booleans used as
numbers, fractional numbers, nulls and unknown fields are not coerced.

`mode` is 1–32 uppercase ASCII letters/digits and must be in the backend's
qualified mode list. Filter cuts are signed int32 **carrier-relative Hz**;
both are required together. The requested lower/upper cut and width must fit
the current observed mode's declared limits. No pitch-dependent CW, RTTY
shift-dependent, FM preset, or digital-voice lease behavior is guessed.
`gain` is an integer 0–100 (backend linear receive level, not RF gain or dB).
`muted` is a JSON boolean. Pan `hz` is a positive exact JSON integer; coverage
uses the same conservative whole-MHz observation ceiling as frequency control.
Center and bandwidth must leave a positive-frequency viewport.

```json
{"v":1,"id":"mode-1","sessionId":"<negotiated>","method":"slice.setMode","params":{"radioSession":"radio-1","slice":"0","expectedRevision":42,"mode":"LSB"}}
```

Success is `{"accepted":true}`: one typed backend intent was dispatched, **not
hardware acknowledgement, DSP completion, or a promise of the requested value**.
The target does not call optimistic desktop setters. A synchronous publication
may precede the response, and a same-value command need not create an event.
Clients converge through resource snapshots/events, not by copying their request.

## Admission, concurrency and safety

The target binds once to an idle engine before serving requests. Every action
rechecks the owning thread, dependency lifetime, ready connection, current
capabilities and exact ownership. Slice locks, foreign slots, diversity child
receivers and external receive replacement refuse slice control. Unknown pan
ownership is not sufficient; wire-less pans must resolve through the current
backend mapping. Pan center must not implicitly retune a slice; bandwidth must
not create/remove/retune receivers. Backends with those couplings stay disabled.

Receive mode, filter, gain/mute and geometry need their own backend observations;
constructor defaults are not evidence. Mode readback changes invalidate old
filter cuts, and both cuts must be observed again. A pending mode intent blocks
another mode or filter intent until its matching mode publication, disconnect
or backend replacement. It cannot accumulate a command queue. If a backend
never reports the selected mode, clients must reconnect or wait for fresh
matching state; timeout does not fabricate success or unlock old-mode filters.
An unrelated/old-mode publication cannot safely release this interlock: it may
have been queued before the intent took effect and is not an explicit rejection.

Revisions are freshness checks, not compare-and-swap or exclusive ownership.
Other receive intents may be accepted against the same observed revision before
a backend publishes; they dispatch in engine order. Meter revisions are separate
and do not invalidate slice/pan selections. On conflict or an uncertain response,
refresh and make a new decision; do not replay the old request automatically.

The shared receive guard preserves the frequency-control TX policy. TX-capable
backends need explicit normalized idle readback, and radio/local TX, MOX or TUNE
activity invalidates that permission. Default false and a falling command edge
are not idle evidence. This is **not** the Stage 4 TX arbiter or a guarantee
against external PTT after the last observation. No TX method, lease or grant is
introduced. Frequency tuning remains governed by its separate coverage contract.

All methods share the existing 100/s, burst-200 session budget. Grant revocation
is terminal and cannot undo an already dispatched intent. Error meanings match
[frequency control](aetherd-local-slice-frequency-control.md): invalid schema,
missing resource/ownership, conflict, unavailable capability/observation, or
out-of-range request. Methods never accept raw commands, endpoint overrides,
credentials, reflection paths, TX flags or `force`.

## Qualified backend surface

Optional per-operation records in `RadioCapabilities` are declarations, not
grants. `capabilities.get` additionally checks current eligibility and advertises
a method only while at least one resource can use it. Every request rechecks
its own resource. Null records mean unavailable. Refresh after lifecycle,
capability, ownership and resource changes; advertisement reserves nothing.

| Backend | Mode | Filter | Gain/mute | Pan center | Pan bandwidth |
|---|---|---|---|---|---|
| Sim | USB, LSB | unavailable: echo-only | unavailable | unavailable: fixed VFO scene | unavailable: fixed span |
| Flex | USB/LSB, DIGU/DIGL, AM/SAM/DSB, CW, FM/NFM | USB/LSB, DIGU/DIGL, AM/SAM/DSB | unavailable: legacy wire route | unavailable: unknown coverage | unavailable: coupled legacy geometry |
| HL2 | USB/LSB, DIGU/DIGL, AM/SAM, CW | USB/LSB, DIGU/DIGL, AM/SAM | supported | 100 kHz–38.4 MHz | unavailable: radio-wide rate can retire receivers |
| ANAN | not yet qualified | not yet qualified | not yet qualified | unavailable: also retunes slice | 48 kHz–1.536 MHz; backend selects actual rate; rate changes rebuild DSP and restart the P2 session, interrupting streams |
| RTL-SDR | AM/SAM, FM/FMN/WFM, USB/LSB, CW/CWR | unavailable: DSP does not consume cuts | supported | unavailable: also retunes slice | 225001 Hz–3 MHz; observe actual result |
| Icom | not yet qualified | profile/preset contract needed | not yet qualified | not yet qualified | not yet qualified |

Flex and ordinary TX-enabled HL2 still lack the normalized idle readback needed
by this daemon path, so their declarations do **not** make those methods live.
Explicitly TX-disabled HL2 can qualify. Icom is not offered by the daemon's
catalogue. Optional RTL builds and hardware paths require their own validation;
Demo evidence is not RF or hardware certification. Flex filter limits are grounded
in FlexLib 4.2.18 `Slice.cs` (`FilterLow`/`FilterHigh`); in-process mode/filter
records refer to the backend's actual DSP dispatch path, not a guessed radio API.

Desktop routing/default requests are not migrated. HL2 now publishes its actual
receive mixer gain/mute, including unity gain (100) before a user override; this
can correct a previously default-looking display of 50 without changing audio.
RTL publishes actual DDC gain/mute and resets observations to the new DDC's
unity/unmuted initial state on reconnect.

## Verification

`control_receive_test` is socket-free: a recording intent seam plus separately
injected normalized observations exercise the production service, target,
ownership, thread/lifetime/TX gates, ranges, mode/filter ordering and revisions.
Its backend declaration checks never connect hardware. `control_telemetry_test`
uses an injected monotonic clock for bounded samples, freshness, redefinition,
reconnect, independent command revisions, revocation and resync. Existing codec,
authorization, connection, frequency and meter tests remain applicable.

The optional `aetherd_receive_smoke` executable is **not in the default build or
CTest graph**. It starts our actual daemon on a unique local socket / Windows
named pipe with isolated settings, `--discover-sim`, and automation TX disabled.
Two real clients negotiate, observe, change Demo mode/frequency, read the meter
delivery contract and RX-only transmit state, disconnect and reconnect. Normal
Demo RX has no meter samples; sample delivery/freshness is verified by the
socket-free telemetry tests, not this diagnostic. It exits 77 on listen refusal, never scans LAN/USB,
and never stands in for third-party firmware. Run it only when explicitly
authorized:

```sh
cmake --build <native-build-dir> --target aetherd aetherd_receive_smoke
<native-build-dir>/aetherd_receive_smoke <absolute-path-to-aetherd>
```

The native desktop automation bridge/MCP separately checks the exact rebuilt
Demo app and legacy control convergence. Neither path certifies live radio
behavior or the as-yet-unimplemented TX arbiter, binary data plane or thin UI.
