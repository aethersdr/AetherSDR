# First experimental TS-890S RX PR

This bounded delivery supersedes exhaustive reference parity as the first-PR gate. Historical research stays preserved locally. Status below reflects executed evidence, not planned work.

## Upstream integration — November 9

PR #7118 merges upstream `505b9172cf240db3ce017299005b4b2a9474db92` into
the feature branch. Conflict resolution retains upstream CW text control, ATU
availability and test additions alongside the TS-890S integration. Upstream TCI
volume/AGC units remain the default; Kenwood's existing client dialect stays
behind its explicit receive-client opt-in. Generated touchpoint and log-rule
locations were refreshed without weakening the architecture checks.

The merged source builds the native M5 app and daemon. Seven backend/TCI suites
passed in 49.49 s and both affected applet suites passed in 4.88 s. Documentation
and static checks pass. All four architecture gates pass on both current
upstream and merged source, with no added warning records after line-number
normalization. The proposal retains the same 220 changed paths against upstream.
Existing whitespace in upstream WSPR sources was preserved; the proposal's
whitespace check against upstream passes.

The earlier hardware results below apply to the pre-merge tested head. A new
owned RX candidate and bounded live rerun are being prepared; post-merge
hardware qualification is not yet claimed.

| Function group | Scope | Qualification / remaining gate |
|---|---|---|
| KNS identification, login and scoped credentials | Included | November 9 exact-candidate bounded endurance completed, with two owned reconnects and clean process teardown |
| Receive audio and waterfall | Included | Operator confirmed audible RX on November 9; span-fix candidate has 707 changing waterfall samples and five finite nonzero raw/post/final PCM captures across the 12-minute run and reconnects |
| Frequency entry, click/drag/wheel/keyboard, client STEP | Included | November 9 isolated candidate: typed, wheel, keyboard-handler and spectrum double-click tuned and restored with settled Fixed bounds held. Span-fix candidate: 12 typed tune/restore cycles passed. Drag remains software-qualified |
| Native FC/UD relative tuning | Included | Three real-widget cases passed and restored: FC Up×1, UD Up single, UD Down3. Other combinations software-qualified, not exhaustive hardware coverage |
| Native span/zoom | Included | November 9 span-fix candidate: 14 settled real-button checks covered all eight spans 5–500 kHz in CW and restored 20 kHz. Kenwood-only repeated-request guard prevents a settled zoom gesture reversing its span; firmware 1.01 only |
| Center/Fixed/Auto-scroll | Included | November 9 isolated candidate: all three modes selected and original Center restored; Fixed bounds held across typed/wheel/keyboard-handler/double-click tuning. Span-fix candidate: settled span ladder passed in Center. No claim of writing/restoring persistent Fixed edge ranges |
| VFO/sidebar meters | Included | Checkpoint screenshot shows agreement at 16 dots. November 9 completed rerun measured changing native LEVEL telemetry from 0 to 32 dots, maximum age 307 ms; operator accepted the meter topic as working. No calibrated S/dBm conversion or new visual needle-motion measurement |
| Mode selection | Included | All 14 exposed modes converged through real RX applet, original CW restored. No modulation-source/decoder-quality claim |
| CW filter and APF | Included | Earlier 250/500 Hz filter, APF bandwidth/shift/gain effects captured; span-fix candidate APF Off/On restored with finite nonzero PCM. Operator accepted DATA SEND LAN as intended; original route remains unknown. Ambient-noise response only, not calibrated transfer accuracy |
| Native filter-bank settings/resets and network audio levels | Included experimentally | Operator accepted software-tested / hardware-unverified limits for the first PR. APF/filter effect checks do not establish bank-reset or routing/level coverage |
| Native AGC and receiver options | Included experimentally | Operator accepted software-tested / hardware-unverified limits for the first PR |
| Auto-mode tables, scan/preferences, operating display/beeps, CW/PSK decoder settings, detailed EQ/routing | Included experimentally | Operator explicitly accepted software-tested / hardware-unverified limitation; no hardware-pass claim |
| Existing TCI/CAT/rigctl RX and host recording | Included experimentally | Selected integration software coverage; operator accepted software-tested / hardware-unverified limits for the first PR |
| Native transverter XV/XO and full RF projection | Deferred | Writes unavailable; ordinary coordinates withheld until confirmed Off; admitted On retires session without changing the radio. Software safety tests passed |
| Native memories and memory scans | Unsupported | Operator exclusion; visible reasons and backend refusals retained |
| TX, native transmit-input/power-limit configuration, mixed RX/TX VFO actions, CW auto-zero start/stop | Deferred | Fences retained; no RF tests or TX acceptance |
| Exhaustive reference/accessory parity | Follow-up | Historical inventory is not a first-PR gate |

## Isolation checkpoint — November 9

### Live follow-up — November 9

The operator made the radio available again and confirmed audible receive audio.
Read-only SEND/PTT and DATA SEND observations showed Microphone and LAN,
respectively. The operator explicitly accepted LAN as the intended DATA SEND
setting. That closes the disposition question without inventing an original
value or claiming the earlier route was restored. No route write or connected
PC Audio toggle was used in this session.

Live testing found a Kenwood span quantization defect: the shared zoom control's
settled repetition could reverse the first admitted native span change. The
correction is confined to the Kenwood backend and preserves the existing shared
gestures. A failing-before/passing-after regression covers repeated requests,
the full native span ladder and context retirement. Four focused suites passed
on the updated source (42.32 s); two ASan/UBSan suites passed (277.67 s, leak
detection disabled). Native M5 app/aetherd, docs and static checks pass.

The separately staged span-fix candidate has 81 arm64 Mach-O files, verified
bundled dependencies and a valid deep/strict local signature. Its app/daemon
UUIDs match the M5 build. Maximum observed dependency minimum remains macOS
27.2; no older-macOS, Intel or Windows runtime claim is made.

An initial timed attempt stopped at its first post-reconnect native-context
check after 360 samples and 12 successful frequency-entry/restore cycles. The
automation acknowledged only a deferred Read click; a later explicit native
read succeeded. Independent source review found no confirmed permanent binding
defect. The attempt also recorded one 1.816 s ping and 2.523 s meter age, so it
does not qualify endurance or the response-time gate. Meter values were all
zero; movement is unqualified. Failed and preflight attempts remain in local
evidence. A corrected bounded run waits for startup and records actual native
survey activity/completion. The rerun completed 720.87 seconds with 707 samples,
12 exact tune/restore cycles and two owned reconnects. Both reconnects recovered
native context, rendered scope and captured raw/post/final audio within seven
seconds, with the survey acknowledged on the first attempt each time. Every
sampled waterfall crop changed, with no unchanged-crop warnings or unexpected
disconnected samples. Bridge ping p95/max was 0.574/33.622 ms. Five three-second
captures had finite nonzero PCM at all three stages with no clipped samples;
this does not establish uninterrupted or drop-free audio for the entire run.

Final readbacks verified the owned baseline, A/B VFOs and split Off before normal
Quit; the process exited in 0.31 seconds. RSS samples ranged from 311040 to
524368 KiB, ending at 400608 KiB; this short run is not a leak-free claim. The
audio underrun counter resets across reconnects and is not used as evidence of
drop-free playback. The installed application was untouched and the radio was
left receiving; no transmit or power requests were made.

The operator subsequently accepted the meter topic with “Consider this topic
OK, it works.” The completed rerun also measured native LEVEL changes from 0 to
32 dots, with maximum sample age 307 ms. Earlier zero-only samples remain in the
failed-attempt record; no new visual needle-motion measurement is claimed.

The operator changed the tuning during an earlier preflight and confirmed doing
so. The new owned baseline is 7.019804 MHz, CW, 250 Hz filter, APF On and 20 kHz
scope span. The old frequency must not overwrite that operator choice.

The local isolation pass adds default-absent per-feature interaction/client and
presentation descriptors. Only Kenwood opts in. Existing radios retain their
navigation, visibility/colors/help, tuning paths and client behavior. Twenty-five
selected suites passed, followed by five focused passes after the last minor
adjustments; three ASan/UBSan suites passed with leak detection disabled. Two
isolation guards were mutation-checked. Native M5 app/aetherd and docs builds pass.
These local changes are not yet committed or pushed. The November 6 deployed
candidate below is historical, not a package of the isolated current source.
The live follow-up above supersedes this checkpoint's open hardware gates and
DATA SEND disposition.

## Independent isolation audit fixes — November 9

The final isolation audit's APF presentation, connection-ownership, saved-family
probe and APF-help findings are corrected. Native APF registrations explicitly
release their temporary presentation without changing existing registrations.
Local/SmartLink takeover retires native intent while preserving the saved form;
new native IP requests reserve cancellation ownership before queueing. Saved
family restoration precedes callback epoch capture. Lifetime/reentrancy guards
protect that reservation boundary.

Two independent reviewers found no remaining blocker in these GUI/lifecycle
fixes. On the updated source, 28 selected suites passed and one source-path suite
skipped because this Mac lacks an adapter with both IPv6 GUA and ULA addresses
(74.18 s total). Four ASan/UBSan suites passed (19.00 s; leak detection disabled).
The APF/help and Local/SmartLink regressions failed before their fixes. Two
connection guards were separately mutation-checked, restored byte-for-byte and
passed. Native M5 app/aetherd and user-doc builds pass; static checks pass with
no new accessibility warnings against the pre-fix snapshot.

The initial November 9 isolated candidate was staged separately and then tested
in the newly available owned RX session. The span-fix candidate described above
supersedes it. Neither was installed over `/Applications/AetherSDR.app`. Earlier
hardware evidence does not automatically qualify later source revisions.
These changes are uncommitted and unpushed. Historical offline handoff manifests
must not be used as the current commit allowlist.

## Earlier executed software and package checks

- Selected regression: 44 passed, one skipped (`kenwood_network_test`), zero failed in 319.75 s. This checkpoint precedes the later Scope-row and TCI-relay changes; their focused regressions subsequently passed.
- Four selected ASan/UBSan suites passed in 69.85 s, leak detection disabled. No full-suite or Windows/Intel validation claim.
- APF shared-level and early FC ordering defects have failing-before/passing-after regressions. Scope-row regression also failed before its visibility fix.
- Three focused TCI suites pass after the later confirmed-only notification fix (18.94 s). Its legacy-seed regression failed before the fix; independent source review verified it.
- Final M5 app and aetherd compile. Candidate `delivery-pr-rx-offline-20261106-121503` contains 81 arm64 Mach-O files, valid bundled dependencies and verified deep/strict ad-hoc signatures. Its maximum observed deployment minimum is macOS 27.2.
- An exported proposal configured and built app/aetherd independently, without optional DFNR and keychain packages. The deployed candidate includes those locally prepared optional dependencies; supporting input hashes are recorded separately.
- User docs build and current engine-boundary, command-plane, capability, touchpoint, generated-reference and whitespace checks pass. ARM build graph has no RNNoise x86 sources.

The earlier deployed checkpoint `delivery-pr-rx-20261106-085700` supplied the recorded live functional evidence. The November 6 offline candidate includes the Scope-row and TCI relay fixes but predates isolation. It was not launched while the operator was offline and did not demonstrate real RX. An intermediate candidate launched with a responsive authenticated bridge but was idle. Do not transfer those checkpoints' runtime proof to the current candidate.

## Closed review findings

- Both APF shared-level surfaces display `--` when the backend cannot provide a shared percentage; transitions retain that declaration.
- Native FC accepts eligible early frequency observations while preserving invalid-input/safety retirement and coherent final survey requirements.
- Deferred transverter state cannot publish unqualified ordinary coordinates or accept ordinary tuning writes.
- New TCI state relays require an explicit receive-client feature; existing Confirmed-policy backends also retain their established notifications.
- Backend-provided native scope labels expose the existing Scope row; no Kenwood family branch was added. Icom and unlabeled-family visibility is regression-covered.

## Remaining delivery gates

The operator accepted the remaining experimental limits for native filter-bank
resets, network audio levels, AGC/receiver options and TCI/CAT/rigctl/recording.
These join the earlier advanced-page limitation; none is a hardware-pass claim.
The bounded RX validation gates are closed for this experimental scope. Final
source/commit-set review retained the same 220 tracked proposal paths as the
independently audited allowlist, excluding untracked investigations, raw captures
and root package-lock material. Since the isolation checkpoint, the only changed
production files are the three Kenwood backend/span files; the other changes are
their regression and documentation. A separate temporary index records the
proposal without staging the normal index. Signed-commit/fork-push handoff remains
before PR creation. The corrected 12-minute run, both reconnects and listed RX
restoration/teardown completed. The operator accepted DATA SEND LAN and the
meter topic. The November 6 offline-only restriction ended when the operator
made the radio available for the November 9 owned RX tests; this session does not
authorize transmitting or power changes.

## Evidence

The `live-functional/` summaries record representative relative tuning, mode selections, span changes, APF/filter measurements and restoration. Failed/partial attempts remain in local evidence and are excluded from pass claims. Raw PCM, private logs, full widget snapshots and historical inventories are not intended for the proposed commit set.

The operator accepted the explicitly listed advanced-page and remaining experimental-group limitations. These acceptances do not authorize persistent writes or expand TX scope. Core bounded live checks completed separately.

## Screenshot status

No screenshot files are proposed. New native pages may make Radio Setup navigation/search captures stale; these were not refreshed and must be identified in the handoff. Generic Flex Display-panel visibility is unchanged. Local TS-890S screenshots remain unproposed until station-data redaction is reviewed.

## PC Audio procedure correction

PC Audio is not a host-only test toggle on this backend: an explicit On request can write persistent DATA SEND LAN selection (`MS103`). Earlier APF procedures toggled it and restored the host flag to Off, but did not record the original `MS1` route. The available snapshots/logs do not establish that route's original value or restoration. The operator explicitly accepted LAN after a November 9 read-only observation. This resolves its disposition without claiming restoration to an unknown original value. No blind route correction was sent, and PC Audio was not toggled while connected during the November 9 endurance attempts.
