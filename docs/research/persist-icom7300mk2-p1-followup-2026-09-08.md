# IC-7300MK2 Persist P1 follow-up

This follow-up to merged #5500 addresses the diagnostic and test-process P1
items from the [expanded run](persist-icom7300mk2-expanded-run-2026-09-08.md).
The four earlier persistence repairs belong to #5514. This change does not
claim to repair every intermittent radio symptom or complete the entire Icom
control matrix.

## Changes and observed outcomes

| Area | Repair / evidence | Remaining boundary |
|---|---|---|
| State freshness | Accepted frequency, mode/data/filter tuple, SQL, AGC, RF-power and PTT publications carry confirmation age and session/context identity. Pending writes and startup defaults cannot imply readiness. Unchanged valid replies refresh age. | Six tracked fields only. Filter width/PBT, unselected VFO, meters and other controls are not included in the readiness flag. Readiness is diagnostic, not permission to transmit. |
| Transaction history | Monotonic event IDs survive scheduler reset; a backend UUID separates different instances. The bounded export includes retained endpoints for collector gap detection. | Only 128 retained events. Deduplicate by backend UUID/event ID; initial history is not newly observed traffic. CI-V has no transaction IDs, so delayed unsolicited replies cannot prove physical-intent correlation. |
| TX safety/reporting | The first-sample deadline starts before the key command. Prior-burst samples cannot qualify. Fresh zero-carrier CW gaps may omit the SWR ratio only after a valid ratio in the same burst, with both power and SWR telemetry under 500 ms. Icom unkey requires a fresh accepted PTT-off reply as well as model flags. | The 0.9-second initial deadline, 500 ms safety freshness and measured-watt limit are unchanged. Missing replies, positive power without SWR, and bursts that never establish SWR still stop. |
| Waveform labeling | Icom `setTune()` feeds a single sine; Flex `tune_mode` has no Icom route. The bridge now refuses `txtest twotone` on Icom before keying. Earlier reports are corrected. | Actual Icom two-tone/IMD generation remains unsupported. Ordinary TUNE remains a single-tone path. |
| Meter provenance | Undefined or never-fed PA temperature is null, not zero. Low-rate vitals have status, unit and age. The TX harness reports native ALC units rather than substituting legacy `swAlc` dBFS. | Native radio meters and widget observations are not independent RF instrument measurements. |

No poll cadence, queue priority, retry count, audio pipeline or default radio
setting was changed. A mode/frequency publication change and an outgoing VFO
select/exchange invalidate tracked context; an unobserved physical VFO change
with identical reported values cannot be detected by this mechanism.

## Live evidence

The operator authorized an IC-7300MK2 on a dummy load at ANT1, 7.200 MHz, all
modes, maximum 10 W. The radio lock was acquired before connecting. Test clients
used an isolated profile, session-only credentials, a three-second TX watchdog
and a five-percent TX control ceiling. No ATU cycle or VOX enable was performed.
Percentages below are setpoints; watts are the radio's unsmoothed calibrated
forward-power readings.

| Trial | Outcome |
|---|---|
| CW text, 2% RF | Repeat established 2.797 W and SWR 1.0. One earlier follow-up stopped during a zero-carrier character gap: power and SWR telemetry were fresh, but the model intentionally hid the ratio at zero power. The corrected gap handling completed the repeat. |
| AM TUNE, 2% | Radio returned fresh zero forward-power data and no usable SWR; the guard stopped. This establishes a meter-reported zero, not an independent measurement of zero RF or receiver sensitivity. |
| AM TUNE, 5% | Fresh positive output: 1.748 W in the development repeat and 1.399 W in the final repeat, SWR 1.0. |
| DIGU TUNE, 5% | Two first-TX-after-process-start trials established 6.993 W and SWR 1.0. The older intermittent initial DIGU failure was not reproduced. |
| Unkey | Explicit unkey succeeded after each burst; actual forward-power gauges read zero immediately and 0.7 seconds later. Final CW/AM repeats also used the stricter accepted-PTT-off confirmation. |
| Actual meter surfaces | Forward power, SWR and ALC gauges were sampled. Visible supply voltage was 14.894737 V, displayed +14.89 V, matching its native observation (263 ms age). PA-temperature and fan widgets were hidden; temperature was null/unsupported. |
| Process persistence | Manual SQL 27 and AGC Fast agreed in model and actual RX controls after normal Quit/new-process startup. Tracked readiness arrived at 2.513 seconds initially and 2.319 seconds after restart. |
| Final RX reconnect | The final backend-UUID build reached readiness at 2.418 and 2.357 seconds across same-process reconnect. Session generation changed from 2 to 5; the same backend UUID remained, and retained event IDs advanced from 1–66 to 202–267. A socket-free test separately verifies distinct backend-instance UUIDs. |

These repeats establish the exercised paths, not a deterministic cause or fix
for the original missing-CW-power report or the original first-DIGU stop. Those
historical observations remain open. The unsupported two-tone label is a
confirmed client defect; the zero-carrier-gap stop is a confirmed harness defect.
No IC-705 or IC-9700 hardware was exercised.

## Freshness baseline

A no-action 12-second window retained 53 snapshots, all with the six tracked
fields ready. It collected 481 distinct transaction events: 128 initial history
entries plus 353 subsequent events, with no collector gaps.

| Accepted field age (ms) | Median | p95 | Maximum |
|---|---:|---:|---:|
| AGC | 1471 | 2976 | 3217 |
| Frequency | 1050 | 2233 | 2618 |
| Mode/data/filter tuple | 983 | 2166 | 2484 |
| PTT | 132 | 234 | 250 |
| RF power | 1567 | 2983 | 3352 |
| SQL | 1418 | 2983 | 3448 |

These are ages observed by the collector, not wire round-trip latency or
physical front-panel-to-display latency. They provide a baseline before any
scheduler optimization. Five seconds is the diagnostic readiness budget;
TX safety continues to require the tighter 500 ms meter/PTT evidence.

## Restoration and test-process lessons

The final comparison matched the settled original snapshot: 18 slice fields
and 12 transmit fields, including 7.224540 MHz LSB, Manual SQL 14 and AGC Med.
RF 100% and Tune 10% were restored only in a verified TX-disabled client.
ANT1 remained selected, tuner bypassed and VOX off. PTT-off was freshly
confirmed, the app quit normally, and the radio lock was released.

An early startup snapshot showed fallback filter edges -3300…-300 Hz, whereas
the settled original was -3000…0 Hz. No filter recall was used to overwrite that
settled state. Six-field readiness must not be presented as full filter-width
readiness. During cleanup, setting AGC before a mode change was superseded by
the radio's mode-specific recall; restoration was corrected after each mode
confirmation (AM Slow, DIGU Med, LSB Med). Intermediate mismatches were retained.
The earlier expanded run's uncaptured hidden filter-bank definitions remain a
restoration limitation; this follow-up does not recover those missing originals.

## Build and evidence attribution

Live TX evidence used local base `7e6c4805481c73ae814210c83c15edd4c8721e8e`
with the P1 changes, executable SHA-256
`d25c117c4716d013ba35bbe4c2da4532e5dd888a04345ae6dae008259d8361b3`.
The subsequent UUID addition was checked in RX-only reconnects using executable
SHA-256 `542df16d0fd6e1e2e724af31e5e114bd9df6fbe94eebc2cbaca14cfc79596176`.
The new PR is based on refreshed main after #5500 merged; integration build and
local test evidence are distinct: base `ac92489b`, modified tree, full macOS
build passed with `cmake --build build -j22`; eight selected headless CTests
passed in 1.97 seconds, including 17 TX safety cases. Integration executable
SHA-256 is `b1cdf9d88fbe6e46560a544ada6d324afaea6ec53484f4cb62e8b4f6674046bd`.
Engine-boundary, test-registration, bridge-doc, touchpoint-manifest and frozen
CI-gate checks passed locally. No post-transplant TX
or full Icom matrix is claimed. Publication review additionally tightened
aggregate sample qualification to the same post-key/500 ms rule; this has
socket-free coverage and was not a further live TX trial.

Eight focused headless CTest selections passed before the transplant. Mutation
checks demonstrated failures when event IDs were made constant, context
invalidation was disabled, and the Icom two-tone refusal was removed; guards
were restored before the passing run. The new bridge regression injects an
inert backend and invokes the real dispatcher without sockets or firmware peers.

Local evidence sets are `persist-p1-live`, `persist-p1-final` and
`persist-p1-identity`. Ordered observations, source scripts, snapshots, widget
samples, raw replies and hashes remain local. Credentials, network/session
identifiers, profiles and raw artifacts are not included in this report.
