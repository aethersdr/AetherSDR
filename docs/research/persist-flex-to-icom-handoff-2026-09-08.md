# Persist handoff: Flex findings and IC-7300MK2 measurement plan

This is the handoff for PR #5500, not a new radio certification. The Flex
squelch repair remains in #5508. The initial IC-7300MK2 round is now recorded in the
[Icom run report](persist-icom7300mk2-first-run-2026-09-08.md), including
confirmation ages, successful retention trials, identity failures, bounded TX
and restoration. It used separate explicit Icom authorization. The subsequent [expanded Icom matrix and combined findings](persist-icom7300mk2-expanded-run-2026-09-08.md)
records four process retention restarts, six reconnect trials, mode/band/filter/
VFO/display/antenna/TX-control coverage and the remaining gaps. Its filter-bank
restoration limitation is explicit: factory preset recalls changed hidden
definitions whose pre-recall values were not all captured. The plan below
remains a checklist; the two reports state what actually ran.

## Evidence to carry forward

- [Flex two-slice and TX report](persist-flex-multislice-tx-2026-09-08.md):
  18 final RX checkpoints established on FLEX-8400M firmware 4.2.18.41174;
  actual SQL Off/Manual transitions and levels 26/39 were checked independently.
  Selection, USB/LSB mode changes, normal Quit/relaunch, and removal/reopening
  of only the created slice preserved isolation. Audio gating was not measured.
- [AGC isolation](persist-flex-agc-fm-boundary-2026-09-08.md): non-FM modes
  retained 43/17; crossing FM returned threshold/off-level to 50/50 in direct
  radio status while the app was suspended. Model and actual slider agreed
  after resume, including after a fresh subscription two seconds later.
  This isolates the observed reset to the radio publication, but does not
  establish whether Flex intends it. Do not add automatic client replay.
- TX evidence covers the authorized Flex ANT2/14.200 MHz/10 W conditions only.
  Peak unsmoothed forward power was 7.326 W. Independent radio status confirmed
  ANT2 during 15 later bursts, beyond UI selection; physical jack/relay routing
  was not independently measured. SWR above 1.0 alone did not disprove routing.
  All 24 recorded windows ended unkeyed with zero visible post-TX power.
  AM PTT at about 0.184 W remains inconclusive. TUNE/two-tone results do not
  certify microphone, DAX, CW keyer, VOX or physical DSP effects.
- Preserve first failures: the initial pan-center cleanup mismatch, corrected
  liveness field, operator pause, AM stop and initially omitted receive AGC
  cleanup remain in the reports. Later full restoration does not erase them.
- Raw evidence and source snapshots remain locally under
  `build/validation-multislice-20260908/`; the linked sanitized reports carry
  build identity and evidence limits. The live app combined #5500 and #5508.
  Those hardware results cannot be attributed to standalone #5500.

## Newly implemented coverage, awaiting a live run

`tools/radiocert_persist_multislice.py` now seeds distinct threshold/off-level
pairs **43/17 and 61/29**. It tracks `flexAgcOffLevel` during selection,
mode changes, restart, removal/reopening and restoration. It selects AGC Off
before and after restart to prove the actual RX slider displays the off level,
then returns to enabled AGC and checks the threshold. The accessible slider
name remains `AGC threshold` in both modes; its semantic meaning changes.

After the normal restart, each slice separately enters FM and returns to its
original USB/LSB mode. Entry snapshots and widget trees are retained. The
return's retention result is recorded **before** cleanup. Only stable changes
to that slice's AGC and filter fields may become cleanup expectations; missing
or mistyped values, changes in the peer slice, unrelated changes or further
movement stop the run. An immediate recheck precedes explicit restoration.
This is a bounded diagnostic cleanup policy, not proof that every change was
caused by firmware; a concurrent operator change to an allowed field cannot
be distinguished without independent provenance. Run with exclusive ownership.

These additions passed 45 socket-free Python policy cases, plus the existing
12 TX safety checks. Deliberately weakening the cleanup guard failed its test.
No new native app build or hardware run was performed for this extension.
The historical 18-checkpoint result applies to the earlier runner, not this
expanded matrix. FM changes outside the allowed fields deliberately interrupt
cleanup for inspection rather than restoring blindly.

## IC-7300MK2 run sequence

1. Confirm the exact connected model, firmware, identity and transport, check
   the radio lock and existing clients, then acquire exclusive use. Use a
   dedicated Icom settings profile and record the app commit/binary hash and
   runner sources. Use `IcomCredentials` with Keychain or operator-authorized session-only
   credentials; never put passwords or credential-bearing profiles in evidence
   or a PR.
2. Start receive-only with TX permission disabled. Inventory live capabilities,
   model fields, actual widgets, CI-V polling cadence and raw replies before
   changing anything. Use the model-specific official CI-V guide as authority.
   The current Persist mutation runners reject non-Flex families: build and
   validate an Icom-specific plan before trying to run them against this radio.
3. Record SQL Off, Manual and Auto separately: UI intent, saved client Auto
   intent, radio raw threshold, model level/manual cache and actual slider.
   Exercise Off/Manual both ways, distinctive manual levels, and explicit Auto
   entry/exit. Auto is client behavior; a nonzero CI-V threshold does not prove
   the radio selected Auto. Trace any unexpected Auto activation back to its
   writer and connect-time ordering before deciding on a fix.
4. Reproduce the operator's intermittent report with separate same-process
   disconnect/reconnect and full normal Quit/relaunch trials. Start trials in
   Off, Manual and Auto, including changes on the front panel while the client
   is disconnected. Preserve the first publications before any setter, every
   outbound SQL command, subsequent polls and rendered state. Repeat trials;
   one successful reconnect cannot rule out an intermittent ordering defect.
5. Check each advertised mode and filter context, including FM. Query AGC mode
   before and after mode changes and after reconnect; compare fresh CI-V replies
   to model and widget. Transceive alone is not a complete subscription: allow
   at least two applicable poll periods and require fresh command-specific
   readback. Record missing, stale, rejected and unsupported separately.
6. Check external/front-panel changes and mode/filter/VFO context ownership.
   Two VFO memories do not imply two simultaneous receivers. Treat Flex-style
   multislice tests as unsupported unless the actual capability exposes them.
7. Compare all captured slice/pan/TX/EQ state at cleanup, including unexercised
   sentinels and mode-change side effects. Restore only known test-owned state;
   stop for newer/conflicting values. Confirm TX flags false, quit normally,
   reset diagnostic categories and release the radio lock.

## Family-specific boundaries

| Surface | Flex evidence or behavior | Icom expectation to verify |
|---|---|---|
| AGC | Independent threshold and off-level, one mode-dependent slider | Current backend declares `hasAgcThreshold=false` and modes slow/med/fast; no Flex AGC-T/off-level claim. AGC time-constant editing is a different contract. |
| SQL | Radio SQL plus client Manual/Auto intent and cache | Current backend maps Off to threshold zero and On to a nonzero level; verify raw CI-V readback and client Auto intent independently. |
| Percent levels | Do not transfer Flex dB/0–100 semantics | Check `ceil(pct*255/100)` writes and `floor(raw*100/255)` reads where the specific Icom control uses the 0–255 level contract. |
| Refresh | Persistent slice subscription; independent fresh subscription isolated AGC | Poll/query the specific CI-V command in the correct mode/VFO context; never infer complete refresh from a frequency Transceive update. |
| Persist ownership | Empty client operating-state domains | Inspect current capabilities; do not replay radio-owned state or infer ownership from family name alone. |
| Transmit | Previous authorization applied to the Flex setup only | Obtain exact Icom radio, dummy-load port, frequency, modes and physical-watt limit before keying. Verify fresh model-specific calibrated meters and every drive source. |

Source pointers for the next investigation:
[RxApplet](../../src/gui/RxApplet.cpp),
[MainWindow wiring](../../src/gui/MainWindow_Wiring.cpp),
[SliceModel](../../src/models/SliceModel.cpp),
[Icom capability/setter/decoder/polling paths](../../src/core/backends/icom/IcomCivBackend.cpp),
[canonical live-test method](../automation/TX_TEST_PROMPT.md).

Remaining Flex work includes running the expanded FM/AGC matrix, confirming
firmware intent with SmartSDR/vendor evidence, AM PTT investigation, MultiFlex,
multiple pans, automatic SQL/audio gating, and the untested TX paths above.
None is silently transferred into an Icom success claim.

## Freshness and scheduler measurement proposal

Radio authority includes **when and in which context the radio last confirmed
this value**. A recently repainted widget, a successful setter ACK, or an
optimistic model update is not fresh value readback. Conversely, an unchanged
value in a new valid reply is fresh confirmation and must advance its age.

Existing building blocks are already present: `civ scheduler` exposes queue
and response aggregates; `civ incident` contains bounded transaction rows with
semantic key, generation, priority, queue wait, response duration and completion
classification; `civ trace all` includes meter traffic; meter snapshots expose
per-producer ages and reliability. Reuse these before adding instrumentation.
The incident holds only the last 32 transactions, so occasional snapshots
cannot yield a complete latency distribution under sustained traffic. Record
coverage/drops and use a bounded event export if that proves to be the gap.
Do not reconstruct unique events from ages alone or double-count overlapping
incident snapshots.

Proposed Persist evidence should join these stages with session, radio,
receiver/VFO, mode/context generation, semantic field and request generation:

| Measurement | Start → finish | What it tells us |
|---|---|---|
| Queue wait | Request enqueued → dispatched | Scheduling delay, coalescing and priority effects |
| Readback response | Query dispatched → accepted matching value reply | Command-plane response; ACK-only completion is separate |
| Client publication | Accepted reply → model update → widget observation | Decode/routing/UI delay; a widget snapshot only bounds render time |
| Write convergence | User intent → confirming value reply → matching widget | Both directions are connected and the request actually took effect |
| External-change discovery | Marked front-panel change → first accepted value reply → widget | Poll coverage and user-visible delay; mark uncertainty if the action time is manual |
| Field confirmation age | Now → last accepted value reply for this exact context | How long the current value has gone unconfirmed, even if numerically unchanged |
| Reconnect convergence | Connection/context established → each field freshly confirmed | Stale startup displays and the slowest fields in the initial inventory |
| Meter presentation | Raw reply → calibrated model → smoothed gauge | Correct units/scaling and intentional display lag, separately from acquisition freshness |

Use a monotonic clock within each process and a documented correlation boundary
between processes; never subtract unrelated clock origins. CI-V usually gives
client receive timing, not a timestamp for when the radio sampled the register:
label confirmation age accurately, without claiming exact physical sample age.
If model or widget timestamps are missing, report the polling interval as a
measurement bound and mark those stages uninstrumented. Do not invent precision.

For every field report support, authority/source, current value, confirmation
age, pending intent and outcome: never observed, awaiting confirmation, fresh,
stale, timed out, context-invalid or unsupported. A stale/late reply rejected
by the existing generation checks must not refresh a value's age. A mode/VFO
change or disconnect invalidates context-specific freshness until a valid new
publication arrives. An Icom frequency Transceive update cannot refresh AGC,
squelch or meters by association. Keep optimistic requested state separate
from confirmed radio state in the evidence.

Measure idle receive, rapid control movement, front-panel edits, mode/VFO
transitions and reconnect first. Later, with separate Icom TX authorization,
measure keyed/unkeyed transitions and active-meter load. Report sample count,
median, p95, maximum, timeout rate, queue depth and time over a declared
per-field freshness budget. Tail percentiles need enough observations; never
present a handful of samples as a reliable p99. Include readback coverage,
unit/calibration correctness and context validity alongside latency: fast but
wrong data is not an improvement.

Use the baseline to propose priority changes, one at a time: preserve emergency
unkey and TX-state safety, promptly confirm recent operator changes, refresh
context-dependent controls after a mode/VFO change, service active visible
meters and RX controls, then reconcile slower configuration. The current
scheduler already has priority aging, a meter queue budget and a background
starvation ceiling; extend measured behavior rather than replacing those
mechanisms or simply polling everything faster. Hidden controls still need
bounded external-change reconciliation. Compare identical scenarios before
and after, including oldest background-field age and timeouts, so a faster
meter cannot conceal starved settings.

This is a proposal for the Icom round, not implemented per-setting freshness
telemetry or a scheduler optimization in this PR. First audit which stages
are already observable; add the smallest backend-neutral diagnostic seam for
missing stages, with injected-clock/transport tests for stale replies, context
changes and non-events. Then use live Persist for positive convergence and
measured distributions. No current refresh constants are changed here.

The first live round confirms the measurement gap: periodic reads reuse request
generations, so generation/key deduplication does not identify unique completed
transactions. Use a stable event sequence before computing a full scheduler
latency distribution. The [run report](persist-icom7300mk2-first-run-2026-09-08.md)
records this collector correction and the observed startup-default interval.
