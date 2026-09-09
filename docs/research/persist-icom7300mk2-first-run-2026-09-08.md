# IC-7300MK2: first Persist, freshness and guarded TX observations

The first Icom round established control readback and selected reconnect/restart
retention, and exposed connection-readiness and telemetry gaps. It did **not**
reproduce the reported unexpected Auto squelch activation. This is diagnostic
evidence for Persist PR #5500, not a whole-radio certification or a production
fix. The Flex squelch repair remains separate in #5508.

## Build, scope and authority

- Live IC-7300MK2 over its RS-BA1 network transport; directed CI-V address B6
  confirmed the exact model. Firmware version was not exposed by the snapshot.
- Existing integration app commit `9a8ea1b59eea5226dd6e1788608c5f9a22abc650`,
  combining original Persist #5500 (`93bbff39`) and squelch #5508 (`05c68846`).
  Executable SHA-256:
  `a785be3ed2f4904c0ac9528334024ee61ec1075bd8ddcae4166bc40b8c2dfa43`.
  No new native build was made for this run/report. These results cannot be
  attributed to standalone #5500 or to a new Icom implementation.
- Dedicated settings profile, exclusive radio lock, session-only credentials.
  The operator explicitly authorized ANT1 dummy load, 7.200 MHz, all modes,
  maximum physical 10 W. Earlier Flex authorization was not reused.
- Receive/control/restart observations started with TX permission disabled.
  A separate TX process had a three-second native watchdog and five-percent
  control ceiling, plus a reactive 10 W calibrated-meter cutoff. Percentage
  is not watts. Requested bursts were bounded to 1.6 seconds with a 0.9-second
  initial telemetry deadline; all exit paths explicitly unkeyed.
- The radio advertised one slice and empty client operating-state domains.
  Multiple simultaneous slices are unsupported here. SQL threshold and AGC
  mode are radio-authoritative; Auto SQL intent is client behavior. Icom AGC
  threshold/off-level editing is unsupported in this backend. Generic snapshot
  defaults such as AGC-T 65, off-level 10 or PA temperature 0 are not readback.
- A quiet panadapter on the dummy load is expected. Signal amplitude was not
  used as a stream-liveness test, and this round did not measure audio gating.

Protocol interpretation used the official **IC-7300MK2 CI-V reference guide**,
the local Icom oracle, and the current codec/backend/scheduler/meter paths.
No scheduler priorities, polling constants or radio commands were changed.

## Control and retention evidence

Commands below omit framing/address bytes. A matching value reply is distinct
from an ACK and from optimistic model presentation. Percent-level writes use
`ceil(percent * 255 / 100)`; readback uses `floor(raw * 100 / 255)`.

| Control and actual UI action | Wire value and matching reply | Same-session model/widget | External change | Normal restart | Final restoration |
|---|---|---|---|---|---|
| Manual SQL 26 → 39 → 14 | `14 03 00 67`, `14 03 01 00`, `14 03 00 36` | All matched twice after fresh reply | Not established | Seed 39 adopted in model and slider | Manual 14, fresh reply and widget |
| SQL Off / Manual / Auto buttons | Off threshold 0; Manual 26; Auto produces a changing threshold | Corrected reconnect trials retained Off, Manual and explicit Auto | Not established | Only Manual 39 full-process trial established | Manual restored |
| AGC Fast → Slow → Med | `16 12 01`, `16 12 03`, `16 12 02` | All matched twice after fresh reply | Not established | Seed Fast adopted in model and combo | Med, fresh reply and widget |
| AF gain 23 → 0 | `14 01 00 59`, `14 01 00 00` | Both matched twice | Not tested | Baseline 0 matched; distinct 23 not restart-tested | 0 |
| Microphone gain 37 → 80 | `14 0b 00 95`, `14 0b 02 04` | Both matched twice | Not tested | Baseline 80 matched; distinct 37 not restart-tested | 80 |
| RF power 2%, restored 100% | `14 0a 00 06`, `14 0a 02 55` | Fresh radio reply and actual slider | Not tested | Low setpoint retained into later client | 100%, with TX permission disabled |
| Tune power 2%, 5%, restored 10% | Client test-audio drive; keyed power measured separately | Model/widget and post-TX restoration observed | Not applicable to a radio register | Not independently certified | 10% |

For the initial ten SQL/AGC/AF/microphone actions, inferred intent-to-value-reply
times were approximately **70–98 ms**. Model observations were about 4–21 ms
and widget observations about 39–64 ms: presentation commonly preceded radio
confirmation. These are observation bounds from monotonic polling and trace
ages, not exact render times or radio sample timestamps. Later cleanup actions
had longer confirmation times; the initial range is not a run-wide maximum.

The valid same-process reconnect series used a freshly confirmed seed and exact
identified-model readiness. Each trial observed about eight seconds:

- Off stayed Off with threshold 0.
- Manual stayed Manual at 26.
- Explicit Auto stayed Auto. Its visible margin was 10 while its radio
  threshold ended at 30; these are different quantities, not a mismatch.

The distinct Manual 39 / AGC Fast seed also survived normal Quit/relaunch with
the same isolated profile. Relative to the new connect request, identity was
observed at **1.230 s**, SQL confirmation and widget adoption at **2.548 s**,
and AGC confirmation and widget adoption at **3.077 s**. After identification,
model/widget initially displayed SQL 20 / AGC Med until the respective replies
arrived. Eventual agreement must not erase that startup interval. This one
successful restart does not exclude an intermittent Auto/lifecycle defect.

An attempted post-restart raw-write probe was refused by the RX-only bridge's
`civ send` TX gate before dispatch. Cleanup confirmed SQL 14 and AGC Med again.
No physical front-panel change or external-change latency is claimed.

## Idle confirmation freshness

A 30.078-second receive baseline captured 112 observations with no UI actions
or raw-command injection. The scheduler counted 937 replies, zero timeouts
and zero stale replies during that window. The following estimates discard
the first four seconds (96 remaining observations per field) to avoid the
scratch collector's initialization clamp when reconstructing old trace times.

| Field | Median confirmation age | p95 | Maximum |
|---|---:|---:|---:|
| SQL threshold | 1,491 ms | 2,928 ms | 3,271 ms |
| AGC mode | 1,460 ms | 2,933 ms | 3,240 ms |
| Frequency | 1,027 ms | 2,171 ms | 2,451 ms |
| Mode/data flag | 995 ms | 2,109 ms | 2,389 ms |
| RF-power setpoint | 1,501 ms | 2,904 ms | 3,329 ms |
| S-meter | 55 ms | 143 ms | 177 ms |

These are client receive-confirmation ages inferred from a bounded decoded
CI-V trace. They are correlated samples from one short run, not independent
latency trials or exact physical data age. An unchanged reply still refreshes
confirmation. Frequency traffic does not refresh SQL or AGC by association.

The first collector incorrectly labeled 41 deduplicated transaction rows as
unique transactions. Periodic reads reuse `(key, generation, completion)`, so
that key is not an event identifier. Those rows retain only the last observed
event per key; their queue/response percentiles are **not a complete latency
distribution**. Sampled rows included queue waits up to 1,933 ms and response
durations up to 11 ms, suggesting scheduling deserves investigation, not
proving its contribution across all 937 replies. The original data and a
separate correction are retained. Do not optimize from the biased percentiles.

## Connection readiness failures retained

Several authenticated network sessions did not complete CI-V identity. Some
broadcast discovery attempts received `FA` and then timed out; a fresh directed
B6 attempt also exhausted its five identity attempts without identification.
This is broader than an Auto-address-only hypothesis. A later explicit
disconnect, three-second pause and reconnect succeeded; that single retry is
not proof that a longer delay fixes the defect.

The first reconnect scratch helper accepted family/connected state while the
model remained **Unknown Icom**. Its apparent Manual/Auto successes and a later
Off→Manual observation are inconclusive, not confirmed persistence results.
The run stopped, readiness was hardened to exact model plus fresh seed reply,
and the corrected trials above ran separately. No TX was attempted while
identity was unknown. RS-BA1 login/connected status alone is insufficient
readiness for either Persist or transmit testing.

## Guarded TX results

Every burst verified ANT1 in the actual TX antenna widgets, frequency/mode,
offsets, VOX off, tuner bypass, power controls and connected identity. Captured
radio replies included antenna `12 00 00` and TX-frequency
`1c 03 00 00 20 07 00` (7.200 MHz). This is protocol evidence, not independent
measurement of the physical jack or relay. Forward power used the Icom meter
calibration curve; reported peaks below are unsmoothed radio telemetry, not an
external wattmeter measurement. No observed sample exceeded 10 W; sampling
cannot exclude an unobserved transient.

| Radio mode | Requested path / Tune control | Peak observed W | Outcome |
|---|---|---:|---|
| LSB | TUNE / 2% | 2.448 | Fresh forward/SWR, unkey confirmed |
| USB | TUNE / 2% | 2.448 | Fresh forward/SWR, unkey confirmed |
| AM | TUNE / 2% | 0 | Stopped: no valid fresh SWR before deadline |
| AM | TUNE / 5% | 1.748 | Fresh forward/SWR, unkey confirmed |
| FM | TUNE / 5% | 7.692 | Fresh forward/SWR, unkey confirmed |
| FM-DATA (DFM) | TUNE / 5% | 7.692 | Fresh forward/SWR, unkey confirmed |
| USB-DATA (DIGU) | First TUNE / 5% | 0 | Stopped: no valid fresh SWR before deadline |
| USB-DATA (DIGU) | Two-tone / 5% | 5.944 | Fresh forward/SWR, unkey confirmed |
| LSB-DATA (DIGL) | TUNE / 5% | 6.294 | Fresh forward/SWR, unkey confirmed |
| RTTY | TUNE / 5% | 7.692 | Raw mode 04 confirmed; neutral client mode is DIGL |
| CW | CW text, RF control 2% | Unknown | Stopped: no fresh calibrated forward power before deadline |
| USB | Two-tone / 2% | 2.448 | Keyed actual power gauge additionally sampled |

All positive-output windows reported SWR 1.0. All twelve attempted windows
confirmed tuning/MOX/model/radio transmitting false and actual forward gauge
zero immediately after unkey and again 0.7 seconds later. The final USB run
also observed the actual gauge rise from zero through approximately 1.05–2.27 W
while keyed. Earlier windows captured producer/model meters plus pre/post
widgets; they did not sample the actual widget throughout keying.

The Tune waveform selection can persist after two-tone, so later DIGL/RTTY
TUNE button results must not be labeled proven single-tone tests. They show
the requested TUNE path produced reported RF in those radio modes.

Both DATA and non-DATA modulation sources already replied LAN (`1a 05 00 85 05`
and `1a 05 00 84 05`); LAN modulation level was raw 130. No input setting was
changed. Wrong DATA input therefore does not explain the captured DIGU zero
attempt. AM at 2% and the first DIGU attempt remain intermediate observations,
not erased by successful later bursts.

CW text was submitted with break-in enabled (radio `16 47 01`, semi break-in)
and 21 WPM. A keyed-state reply appeared, but the meter deadline expired before
fresh calibrated forward power was available. Explicit CW abort and unkey ran.
This is not evidence of zero RF. CW reverse (CWL, raw mode 07) was checked only
for receive mode/readback afterward; further CW-family TX was not attempted.
Microphone speech, external audio/DAX, VOX, ATU tuning and all physical DSP
effects remain untested. NFM is a codec alias of FM, not a separate RF proof.

Early mode preflights injected raw read queries through `civ send`; later
ones used production mode confirmation and periodic frequency polls. The
diagnostic read path can affect scheduler ACK/timeout accounting. Timeouts and
late/unmatched replies seen during those probes are retained separately from
the clean no-action freshness window. Do not assign them all to ordinary polls.

## Restoration and harness corrections

Final slice, pan and EQ comparisons against the original captured state were
empty. Receive state returned to **7.224540 MHz LSB, Manual SQL 14, AGC Med**;
AF 0, microphone 80, RF control 100% and Tune control 10% matched their original
values. The power setpoints were restored in a separately verified TX-disabled
client, after the TX session ended. These are restored setpoints, not emitted
power. The only remaining TX-state differences are tuner enabled/status:
**the tuner remains bypassed**. No separately authorized ATU cycle was run.
This comparison does not certify every internal radio band-stack/VFO memory.

The first restoration launch set the TX environment variable to `0`, but the
bridge checks presence, so its TX-disabled assertion failed while the client
was still disconnected. It was closed normally without connecting or keying;
the corrected launch removed the variable and verified `txAllowed=false`.
Earlier pre-key scratch metadata errors and the refused external-write probe
are also retained. These are harness findings, not successful radio tests.

Diagnostic logging was reset, the final client exited normally, and the radio
lock was released. Final state: **one slice, transmitting false**.

## Concrete next work

1. Add a stable transaction event sequence and bounded export/coverage counters
   so queue/response distributions can be measured without duplicate loss.
   Reuse current scheduler diagnostics; do not interpret request generation as
   an event identity.
2. Track each supported field's accepted confirmation time and session/VFO/mode
   context separately from pending intent. Preserve unsupported, never seen,
   stale and rejected states. Unchanged valid replies must advance freshness;
   rejected late replies and unrelated Transceive traffic must not.
3. Expose inventory readiness distinct from transport connection, and distinguish
   startup defaults from confirmed radio state. Investigate identity exhaustion
   with RS-BA1/CI-V ordering evidence before changing retries or timers.
4. Measure marked physical front-panel changes and mode/VFO transitions, then
   compare one scheduler adjustment at a time. Preserve urgent unkey/TX-state
   and meter service, operator-write confirmation, context refresh and bounded
   background reconciliation. The present data warrants measurement, not a
   blanket increase in polling rate.
5. Diagnose CW keyer/TX-state-to-meter scheduling with injected transport tests
   before another bounded live CW trial. Separately reproduce AM/DIGU startup
   output with an explicit waveform selection and input/readback capture.
6. Repeat the intermittent Auto SQL investigation with physical front-panel
   changes while disconnected, radio switching and multiple full-process trials.
   The successful directed reconnect series is a baseline, not closure.

Per-field production freshness telemetry, scheduler optimization, connection
repair and CW fixes are **not implemented in this PR**. The public Flex mutation
runners still reject Icom; this run used explicit, locally retained Icom lab
scripts. Generalizing those scripts requires capability-shaped contracts and
socket-free policy/transport validation, not a permissive family-check removal.

## Evidence retention

Run ID `validation-icom-20260908`. The operator retains the ordered
`observations.jsonl`, initial/final snapshots and widget trees, scripts,
control/reconnect/restart results, all twelve TX windows, CI-V traces,
freshness baseline and correction, restoration comparison and session logs.
Raw artifacts contain private radio/session metadata and are not bundled in
the PR. Credentials were session-only and are excluded from this report.
