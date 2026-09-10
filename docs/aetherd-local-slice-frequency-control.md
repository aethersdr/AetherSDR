# AetherD v1 — local slice frequency control

This Stage 3 slice of #3849 adds `slice.setFrequency` for an **existing owned
slice**. It does not create slices, connect radios, migrate the desktop client,
implement other receive setters, or expose transmit. The daemon still defaults
to observe-only. The existing `--allow-local-control` flag explicitly grants
observe and non-TX control to every client admitted by its current-user local
endpoint; it is not per-application consent. No remote transport, credentials,
new grant, implicit discovery or automatic retry is introduced.

## Request and response

After `hello`, use the negotiated connection's `sessionId`:

```json
{"v":1,"id":"tune-1","sessionId":"<negotiated session>","method":"slice.setFrequency","params":{"radioSession":"radio-1","slice":"0","expectedRevision":42,"hz":14230000}}
```

All four parameters are required; unknown fields are rejected. `slice` is a
canonical non-negative decimal string fitting a signed 32-bit integer (no
sign, whitespace or leading zeros). `expectedRevision` is the slice resource's
revision, not the radio-session revision. It and `hz` must be positive exact
JSON integers at most 2^53-1. Hz are never silently rounded or clamped.

That is the general wire-integer limit, not the frequency-observation domain.
Coverage maxima and observations share the conservative whole-MHz ceiling
9,007,199,254 MHz (9,007,199,254,000,000 Hz), below the wire ceiling by 740,991 Hz.
This avoids advertising a command domain the MHz model cannot report as known.
A backend maximum above this ceiling disables its frequency-control capability;
an observation above it is unknown. Current backend maxima are much smaller.

Success is `{"v":1,"id":"tune-1","result":{"accepted":true}}`.
It means exactly one typed backend intent was dispatched, **not** that the
radio acknowledged it, the requested frequency took effect, or DSP settled.
No raw command, endpoint, family override, force option or TX flag is accepted.
The target deliberately bypasses the optimistic desktop/CAT setters.

Authorization is checked before parameter validation and resource lookup. The
service then checks the exact session/slice resource and revision. Immediately
before dispatch, on the engine owning thread without yielding, the model target
checks connection readiness, current slice ownership, lock state, fresh frequency
observation, backend coverage and transmit safety. The trusted target binds once
before dispatch to an otherwise idle engine; it does not attach to a running
desktop session. Destroyed dependencies or a wrong-thread call fail closed.

| Error | Meaning |
|---|---|
| `auth.grant_denied` | No control grant; observing does not authorize tuning |
| `request.invalid_params` | Missing, unknown, non-integral or noncanonical parameters |
| `resource.not_found` | Unknown session, absent slice, or slice not owned by this engine |
| `request.conflict` | Stale revision, connection transition, lock, or active/unconfirmed TX |
| `capability.unavailable` | No target, unknown coverage/authority, or no valid observation |
| `request.out_of_range` | Outside the backend's positive bounded range or declared band union |

Existing envelope, session, revocation and terminal rate-limit errors still
apply. This method shares the existing per-client 100/s, burst-200 budget;
it adds no queue, reservation or independent retry budget.

## Observations and concurrency

`slice.value.frequencyHz` retains its existing effective model semantics,
including optimistic desktop changes. The additive `frequencyObservation`
object separates the last backend publication:

```json
{"known":true,"hz":14230000,"authority":"radio"}
```

`known:false` carries `hz:null`, never a default masquerading as a report.
`authority` is `radio`, `engine` or `unknown`, declared by the backend.
`radio` denotes normalized radio readback; `engine` denotes backend-owned
receive configuration, **not a hardware acknowledgement or DSP-completion
signal**. In-process backends may publish before asynchronous hardware/DSP work
settles. Invalid/non-positive reports clear knowledge. Disconnect/reconnect
invalidates the old observation, even if a SliceModel is reused. A new same-value
report establishes knowledge again. An echo matching an optimistic desktop
value still updates the separate observation. Identical snapshots are deduplicated.

Observe this field through existing `resource.get` / atomic
`resource.subscribe` snapshots and events. A differing backend value wins;
neither timeout nor acceptance fabricates the requested value. A backend can
publish synchronously, so its event need not follow the response. An accepted
same-value command need not produce an event at all. There is no per-command
completion event, error callback, convergence timeout or exactly-once guarantee.

`expectedRevision` is a **freshness check, not compare-and-swap or exclusive
ownership**. Two controllers may submit different intents against the same
observed revision before a backend publication; both can be accepted in engine
dispatch order. No pending intent changes the revision by itself. A publication,
including a non-frequency slice change, invalidates old selections. Removed and
recreated resources get new revisions, including across reconnect. Refresh and
make a new decision on conflict or an uncertain response; never automatically
retry the old intent. Losing a grant blocks subsequent dispatch but does not
undo an already accepted command. No TX lease is created or implied.

## Capability and safety gates

`radioSession.value.capabilities.sliceFrequencyControl` declares `authority`,
`minimumHz` and `maximumHz`. Zero bounds or unknown authority disable the method;
the older UI `tuningRangeHz` convention (zero means unconstrained) is not reused.
A nonempty `declaredBands` list further restricts requests to its inclusive union.
Malformed bounds fail closed. The simulator declares a 1 Hz–1 THz API domain,
not physical RF coverage. All six backends explicitly declare this metadata;
see the [capability map](architecture/radio-capabilities-map.md#local-slice-frequency-control).

`capabilities.get` advertises `slice.setFrequency` only to controllers and only
while at least one owned unlocked slice is eligible. Every request rechecks the
selected slice. Refresh capabilities after connection, capability or slice
changes; advertisement is not a reservation or a guarantee of later acceptance.

For a TX-capable backend, initial/default false TX state is insufficient. The
target requires explicit idle readback and refuses while the radio or local
transmit/MOX/TUNE state is active. Active TX, backend replacement and connection
transitions invalidate idle knowledge. A falling command edge alone cannot
restore it. This is conservative receive admission, **not the Stage 4 transmit
arbiter**: asynchronous external PTT can always change after the last report.

The `txSlice` designation is not itself a refusal condition. An explicit receive
tune while confirmed idle may change the frequency that a subsequent transmission
would use; this method does not grant permission to key. Lease/inhibit policy for
that coupling belongs to the Stage 4 arbiter and must be reviewed before enabling
TX-capable daemon paths, not inferred from this receive-only method.

The production simulator is the fully exercised integration path for this
slice. RTL-SDR is eligible when built and connected but is not hardware-certified
by simulator tests. Ordinary TX-enabled HL2 lacks normalized idle readback and
remains unavailable; explicitly TX-disabled HL2 can qualify. Flex and ANAN
currently lack verified command coverage and remain unavailable. Icom's
profile-driven bounds/readback can satisfy the model seam, but the current daemon
catalogue intentionally does not offer Icom/manual connections. None of these
limitations changes existing desktop tuning behavior.

## Verification

`control_slice_frequency_test` exercises the production target/service/adapter
with an injected socket-free command recorder and normalized observations. It
does not open a listener, simulate firmware, discover hardware or key a radio.
It covers grants, schemas, revisions, competing controllers, delayed/differing
reports, optimistic desktop separation, reconnect, coverage, locks, unknown/active
TX, target lifetime, owning-thread dispatch and terminal budgets.
The manual daemon smoke uses our real local server and production simulator;
native Mac automation-bridge regression uses a separate no-TX simulator profile.
Neither substitutes for hardware convergence evidence.
