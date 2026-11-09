# First experimental TS-890S RX PR

This bounded delivery supersedes exhaustive reference parity as the first-PR gate. Historical research stays preserved locally. Status below reflects executed evidence, not planned work.

| Function group | Scope | Qualification / remaining gate |
|---|---|---|
| KNS identification, login and scoped credentials | Included | Software coverage and earlier owned live login; final-candidate reconnect and teardown pending |
| Receive audio and waterfall | Included | Audible RX confirmed by operator on Debug build; deployed checkpoint rendered with Metal and captured nonzero raw/post/final PCM; final-candidate recovery pending |
| Frequency entry, click/drag/wheel/keyboard, client STEP | Included | Software coverage retained; final native gesture/effect/restoration checks pending |
| Native FC/UD relative tuning | Included | Three real-widget cases passed and restored: FC Up×1, UD Up single, UD Down3. Other combinations software-qualified, not exhaustive hardware coverage |
| Native span/zoom | Included | All eight spans 5–500 kHz observed through real zoom buttons in CW, original 10 kHz restored; firmware 1.01 only |
| Center/Fixed/Auto-scroll | Included | Scope-row visibility regression fixed and tested; live final-candidate row/gesture/stationary-window proof pending |
| VFO/sidebar meters | Included | Checkpoint screenshot shows agreement at 16 dots; final moving-value/liveness check pending. No calibrated S/dBm conversion |
| Mode selection | Included | All 14 exposed modes converged through real RX applet, original CW restored. No modulation-source/decoder-quality claim |
| CW filter and APF | Included | 250/500 Hz filter, APF Off/On, 80/160/320 Hz bandwidth, shift and gain readbacks/effects captured; listed RX fields restored; persistent DATA SEND routing restoration unverified after earlier PC Audio toggles. Ambient-noise response only, not calibrated transfer accuracy |
| Native filter-bank settings/resets and network audio levels | Included experimentally | Software coverage; per-control live qualification remains open. APF/filter effect checks do not establish bank-reset or routing/level coverage |
| Native AGC and receiver options | Included experimentally | Software coverage; live per-control qualification remains open unless explicitly accepted |
| Auto-mode tables, scan/preferences, operating display/beeps, CW/PSK decoder settings, detailed EQ/routing | Included experimentally | Operator explicitly accepted software-tested / hardware-unverified limitation; no hardware-pass claim |
| Existing TCI/CAT/rigctl RX and host recording | Included experimentally | Selected integration software coverage; final exact-bundle route or explicit hardware limitation still required |
| Native transverter XV/XO and full RF projection | Deferred | Writes unavailable; ordinary coordinates withheld until confirmed Off; admitted On retires session without changing the radio. Software safety tests passed |
| Native memories and memory scans | Unsupported | Operator exclusion; visible reasons and backend refusals retained |
| TX, native transmit-input/power-limit configuration, mixed RX/TX VFO actions, CW auto-zero start/stop | Deferred | Fences retained; no RF tests or TX acceptance |
| Exhaustive reference/accessory parity | Follow-up | Historical inventory is not a first-PR gate |

## Executed software and package checks

- Selected regression: 44 passed, one skipped (`kenwood_network_test`), zero failed in 319.75 s. This checkpoint precedes the later Scope-row and TCI-relay changes; their focused regressions subsequently passed.
- Four selected ASan/UBSan suites passed in 69.85 s, leak detection disabled. No full-suite or Windows/Intel validation claim.
- APF shared-level and early FC ordering defects have failing-before/passing-after regressions. Scope-row regression also failed before its visibility fix.
- Three focused TCI suites pass after the later confirmed-only notification fix (18.94 s). Its legacy-seed regression failed before the fix; independent source review verified it.
- Final M5 app and aetherd compile. Candidate `delivery-pr-rx-offline-20261106-121503` contains 81 arm64 Mach-O files, valid bundled dependencies and verified deep/strict ad-hoc signatures. Its maximum observed deployment minimum is macOS 27.2.
- An exported proposal configured and built app/aetherd independently, without optional DFNR and keychain packages. The deployed candidate includes those locally prepared optional dependencies; supporting input hashes are recorded separately.
- User docs build and current engine-boundary, command-plane, capability, touchpoint, generated-reference and whitespace checks pass. ARM build graph has no RNNoise x86 sources.

The earlier deployed checkpoint `delivery-pr-rx-20261106-085700` supplied the recorded live functional evidence. The newest offline candidate includes the Scope-row and TCI relay fixes. It has not been launched while the operator is offline and has not demonstrated real RX. An intermediate candidate launched with a responsive authenticated bridge but was idle. Do not transfer the older checkpoint's runtime proof to it.

## Closed review findings

- Both APF shared-level surfaces display `--` when the backend cannot provide a shared percentage; transitions retain that declaration.
- Native FC accepts eligible early frequency observations while preserving invalid-input/safety retirement and coherent final survey requirements.
- Deferred transverter state cannot publish unqualified ordinary coordinates or accept ordinary tuning writes.
- Newly added TCI state relays are restricted to confirmed receive policy, preserving existing optimistic-receiver notifications.
- Backend-provided native scope labels expose the existing Scope row; no Kenwood family branch was added. Icom and unlabeled-family visibility is regression-covered.

## Remaining delivery gates

Disposition of unrecorded DATA SEND routing restoration, final-candidate native scope/tuning gestures, moving meters, remaining included control/integration qualification, 12-minute endurance with two owned reconnects and verified restoration/teardown, final source/commit-set review and signing handoff remain open.

On November 6 the operator withdrew the live session to lock the Mac and turn off the radio. Continue offline preparation only; do not reconnect or power on the radio. Hardware gates require a newly available owned session. They are incomplete, not waived or failed.

## Evidence

The `live-functional/` summaries record representative relative tuning, mode selections, span changes, APF/filter measurements and restoration. Failed/partial attempts remain in local evidence and are excluded from pass claims. Raw PCM, private logs, full widget snapshots and historical inventories are not intended for the proposed commit set.

The operator accepted only the advanced-page limitation listed above. That acceptance does not waive core live checks, authorize persistent writes, or expand TX scope.

## Screenshot status

No screenshot files are proposed. New native pages may make Radio Setup navigation/search captures stale; these were not refreshed while the display is unavailable and must be identified in the handoff. Generic Flex Display-panel visibility is unchanged. Local TS-890S screenshots remain unproposed until station-data redaction is reviewed.

## PC Audio procedure correction

PC Audio is not a host-only test toggle on this backend: an explicit On request can write persistent DATA SEND LAN selection (`MS103`). Earlier APF procedures toggled it and restored the host flag to Off, but did not record the original `MS1` route. The available snapshots/logs do not establish that route's original value or restoration. APF effect evidence and listed RX-field restoration remain valid; full radio-setting restoration is not claimed. No blind route correction will be sent. Obtain an explicit operator disposition in the next available session, and do not toggle PC Audio during endurance to manufacture a passing final-stage capture.
