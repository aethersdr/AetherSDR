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
| `radioSession.setCwPitch` | `hz` |
| `slice.setAgcThreshold` | `slice`, `threshold` |
| `slice.setNoiseBlanker` | `slice`, `enabled` |
| `slice.setNoiseReduction` | `slice`, `enabled` |
| `slice.setRit` | `slice`, `enabled`, `offsetHz` |
| `panadapter.setRfGain` | `panadapter`, `gainDb` |

`radioSession` must identify the current `radio-1`. Slice IDs are canonical
nonnegative decimal strings fitting int32; pan IDs are exact opaque resource
IDs, not backend IDs. `expectedRevision` is the addressed slice/pan revision
(the `radioSession` revision for `radioSession.setCwPitch`),
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
CW pitch `hz` is an int32 receive BFO offset shared by every CW receiver of the
session; it must lie inside the backend's declared `receiveCwPitchControl`
range and on its step. The backend must have reported a pitch in this session
(`receiveObservation.cwPitch` on the `radioSession` resource). A saved pitch
restored at connect counts as reported; a pitch the client set but no backend
or restore has reported does not.

`threshold` is an integer 0–100 on the slice's AGC-T knob scale; only the
threshold changes, and the slice's current AGC mode rides along. `enabled` is
a JSON boolean. `offsetHz` is an int32 receive offset whose
magnitude must not exceed the backend's `maximumRitOffsetHz`; enable and offset
reach the backend as one intent. `gainDb` is an int32 inside the declared
`receivePanRfGainControl` range and on its step. AGC threshold, noise blanker,
noise reduction and RIT are admitted only when `receiveDspControl` declares the
feature, with a known authority, for the slice's **observed** mode, and the
backend has published the matching `receiveObservation` field. Like a filter, they
wait out a pending mode write. The noise blanker toggles on/off only: its kind,
level and fill ride along as the backend last reported them.

There is no lock method. The per-slice tune lock is client-side
(`requestSliceLock` is local-only), so a client that wants one holds it itself
and stops sending tune intents. A desktop lock on the engine's own slice
still refuses every method here with `request.conflict`.

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

| Backend | Mode | Filter | Gain/mute | Pan center | Pan bandwidth | CW pitch |
|---|---|---|---|---|---|---|
| Sim | USB, LSB | unavailable: echo-only | unavailable | unavailable: fixed VFO scene | unavailable: fixed span | unavailable |
| Flex | USB/LSB, DIGU/DIGL, AM/SAM/DSB, CW, FM/NFM | USB/LSB, DIGU/DIGL, AM/SAM/DSB | unavailable: legacy wire route | unavailable: unknown coverage | unavailable: coupled legacy geometry | unavailable: radio-owned `cw pitch` |
| HL2 | USB/LSB, DSB, DIGU/DIGL, AM/SAM, CW/CWL, FM | USB/LSB, DIGU/DIGL, AM/SAM, DSB, CW/CWL, FM | supported | 100 kHz–38.4 MHz | unavailable: radio-wide rate can retire receivers | 100–6000 Hz, step 10 |
| ANAN | USB/LSB, DSB, DIGU/DIGL, AM/SAM, CW/CWL, FM | USB/LSB, DIGU/DIGL, AM/SAM, DSB, CW/CWL, FM | supported | unavailable: also retunes slice | 48 kHz–1.536 MHz; backend selects actual rate; rate changes rebuild DSP and restart the P2 session, interrupting streams | not yet qualified |
| RTL-SDR | AM/SAM, FM/FMN/WFM, USB/LSB, CW/CWR | unavailable: DSP does not consume cuts | supported | unavailable: also retunes slice | 225001 Hz–3 MHz; observe actual result | 100–6000 Hz, step 10 |
| Icom | not yet qualified | profile/preset contract needed | not yet qualified | not yet qualified | not yet qualified | not yet qualified |

HL2 and RTL demodulate on this host, so the pitch is their receive BFO and the
record is `receiveCwPitchControl`. The HL2 reports the pitch it adopted on every
`setCwPitch` while connected, including the connect-time push of the model's
pitch; RTL reports it after DSP adoption.

ANAN gain/mute is applied in the backend's own mixer, because it demodulates on
this host: the per-slice mute and AF gain (and balance, which has no record here)
act on the demodulated speaker feed before it leaves the backend (the per-slice
tap for TCI and decoders stays pre-mute and pre-gain), and both gain and mute
are echoed in `SliceDelta` so the observation this path requires exists. Balance is
applied without a record because `SliceDelta` carries no field for it.

| Backend | AGC threshold | Noise blanker | Noise reduction | RIT | Pan RF gain |
|---|---|---|---|---|---|
| ANAN | every offered mode | every offered mode | every offered mode (WDSP ANR) | ±9999 Hz, every offered mode | −31 to 0 dB, step 1 (the step attenuator, negated) |
| Every other backend | not offered | not offered | not offered | not offered | not offered |

ANAN RIT is a WDSP shift of the detector's zero by the offset: the DDC and
panadapter stay on the slice frequency. ±9999 Hz keeps the widest passband
edge (12 kHz in every mode, CW's 6 kHz cut plus its BFO cap included) inside
the WDSP channel's ±24 kHz at its 48 kHz DSP rate. ANAN noise reduction is
WDSP's ANR, run on the receiver's own channel, so it works with no desktop
audio chain. RTL's `receiveDspControl` keeps an `unknown` authority, so it stays a
GUI gating record and offers none of these verbs.

ANAN mode and filter follow the HL2's contract because both demodulate in the
same host WDSP: Protocol 2 carries no mode, so the radio adds none. ANAN
publishes its own mode menu (`AnanBackend::publishedModeStrings()`), and the
declaration is exactly that list. Aliases (CWU, NFM, WFM, RTTY) are stored
under their offered spelling, and a mode the DSP cannot demodulate is
refused rather than run as USB. The filter ranges are the HL2's, with the CW
pitch clamped to 100–6000 Hz at the backend seam, as the HL2's is.

Flex and ordinary TX-enabled HL2 still lack the normalized idle readback needed
by this daemon path, so their declarations do **not** make those methods live.
aetherd constructs the HL2 TX-disabled (no HL2 TX grant exists), so in the
daemon it qualifies. Icom is not offered by the daemon's
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
