# RadioCert Persist: expanded FLEX-8400M run

Implemented 40 applet setting contracts and repaired the FFT model readback gap. The final RX-only run completed with a reproducible manual-squelch synchronization concern. All 40 applet setpoints matched after a normal client restart; a subsequent band revisit reproduced the SQL disagreement.

## Run and validation

- FLEX-8400M, firmware 4.2.18.41174; exact serial selected, one owned client/slice/pan. 20m/40m and ANT1/ANT2 receive switching only. Separate settings profile, stable GUIClientID across restart.
- Build passed at `ab03be0629a498603ff02ef68a7dbfbd0de91d10`, working tree modified. 25/25 selected local CTests passed, including 30 socket-free runner-policy cases. No CI run at the time of this hardware experiment.
- The pan-model regression test rejected an isolated mutant that omitted the request updates: it read the old 50/25 rather than the requested 17/15. Production source and app were untouched by the mutation check.
- Engine-boundary, test-registration, bridge documentation-generation and diff-whitespace checks passed.
- App SHA-256: `9e0b0b7447ee48a354e5a8cd97778550ff5ec2eb8b877327d6e536e6131ad8e3`. Exact runner source files and SHA-256 values are saved with the run.
- Evidence: 901 snapshots, 275 journaled action intents, 5418 captured wire-log events; 0 detected log gaps. These are bounded observations, not a whole-radio verdict.

## Results by setting

The table reports model convergence. Visible-widget observations are separate in `appletCoverage.widgetTransitions`; hidden CW controls and the inactive EQ page are not falsely marked as visually checked. Every later setter also checks earlier seeded values. A concern in a combined checkpoint is attributed to its differing field, not to every control named in that checkpoint.

| Setting | Seed | Band revisit after restart | Full restart | Restore |
|---|---|---|---|---|
| `transmit.rfPower` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `transmit.tunePower` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `slice.audioGain` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `slice.audioPan` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `slice.audioMute` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `slice.manualSquelchLevel` | ESTABLISHED | CONCERN | ESTABLISHED | ESTABLISHED |
| `slice.agcMode` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `slice.agcThreshold` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `transmit.micLevel` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `transmit.speechProc` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `transmit.speechProcLevel` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `transmit.monitor` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `transmit.monGainSb` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `transmit.amCarrierLevel` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `transmit.voxLevel` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `transmit.voxDelay` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `transmit.dexp` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `transmit.dexpLevel` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `equalizer.rx.63` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `equalizer.rx.125` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `equalizer.rx.250` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `equalizer.rx.500` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `equalizer.rx.1k` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `equalizer.rx.2k` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `equalizer.rx.4k` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `equalizer.rx.8k` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `equalizer.tx.63` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `equalizer.tx.125` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `equalizer.tx.250` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `equalizer.tx.500` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `equalizer.tx.1k` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `equalizer.tx.2k` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `equalizer.tx.4k` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `equalizer.tx.8k` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `equalizer.rxEnabled` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `equalizer.txEnabled` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `transmit.cwDelay` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `transmit.cwSpeed` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `transmit.monGainCw` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| `transmit.monPanCw` | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |

Additional processor, expander, phone-monitor and RX/TX EQ enable round trips retained the seeded levels and both curves. Grid/palette, mode/filter, RX antenna and FFT checks ran alongside the applet scenarios.

## FFT gap resolved

FlexLib updates local Average/FPS on dispatch because firmware can acknowledge a setter without publishing a matching status back to the setting client. AetherSDR previously updated the widget but left PanadapterModel stale. The model now follows successfully dispatched requests, keeps the last radio-published values separate, and always accepts subsequent radio status. No stored client value or timer replays the request.

`averageIsRequest` / `fpsIsRequest` distinguish intent from `radioReportedAverage` / `radioReportedFps`. Seed convergence is not mislabeled as an independent radio read. Band revisits, restart and cleanup require the radio-published FFT values as well as model/presentation agreement. All those final-run FFT checks established their bounded observations.

## Manual SQL concern: radio 26, cache and visible control 20

The test set manual squelch to 26. After 20m → 40m → 20m, firmware published squelch enabled at level 26, while the manual-value cache and actual applet slider showed 20. This reproduced in two expanded runs, including the final run after its client restart. A later CW → USB mode round trip reconciled the value; that later agreement does not erase the earlier concern.

Observed mechanism in `RxApplet.cpp` around `applySquelchState`: an incoming off-state changes the UI mode to Off. On the following radio-restored on/26 status, the handler checks the old UI mode before adopting the manual cache; it then enters Manual and its visual refresh uses the stale cache. `SliceModel::applyDelta` also respects that old manual-echo gate. The radio-owned `squelchLevel` is already 26. This is an ordering defect in adoption/presentation, not evidence that the radio forgot the setting.

The captured discrepancy window contains no outgoing squelch-level write between the restored radio publication and the stale UI observation. A later wrong resend remains a risk to test, not a reproduced claim. No SQL production fix is included in this change. A focused follow-up should adopt the restored manual value on the Off→Manual transition while retaining Auto-mode and external-receive isolation; pin that UI/model transition with a socket-free regression test.

Compact relevant wire records follow (the complete ordered capture is retained):

| PID / sequence | Time | Direction | SQL fields |
|---|---|---|---|
| 40762 / 1733 | 22:13:28.406 | client → radio | squelch=1 |
| 40762 / 1735 | 22:13:29.565 | client → radio | squelch_level=26 |
| 40762 / 2007 | 22:18:45.526 | radio → client | RF_frequency=7.175000, mode=LSB, squelch=0, squelch_level=20 |
| 40762 / 2185 | 22:18:49.781 | radio → client | RF_frequency=14.100000, mode=CW, squelch=1, squelch_level=20 |
| 40762 / 2234 | 22:18:49.782 | radio → client | mode=USB, squelch=0, squelch_level=20 |
| 40762 / 2270 | 22:18:49.782 | radio → client | RF_frequency=14.100000, mode=USB, squelch=1, squelch_level=26 |
| 40762 / 2374 | 22:18:59.737 | radio → client | mode=CW, squelch=1, squelch_level=20 |
| 40762 / 2464 | 22:19:42.357 | radio → client | mode=USB, squelch=1, squelch_level=26 |
| 41733 / 125 | 22:20:36.821 | radio → client | RF_frequency=14.100000, mode=CW, squelch=1, squelch_level=20 |
| 41733 / 126 | 22:20:36.821 | radio → client | mode=USB, squelch=0, squelch_level=20 |
| 41733 / 139 | 22:20:36.822 | radio → client | RF_frequency=14.100000, mode=USB, squelch=1, squelch_level=26 |
| 41733 / 920 | 22:20:59.130 | radio → client | RF_frequency=7.175000, mode=LSB, squelch=0, squelch_level=20 |
| 41733 / 1097 | 22:21:14.673 | radio → client | RF_frequency=14.100000, mode=CW, squelch=1, squelch_level=20 |
| 41733 / 1146 | 22:21:14.673 | radio → client | mode=USB, squelch=0, squelch_level=20 |
| 41733 / 1182 | 22:21:14.674 | radio → client | RF_frequency=14.100000, mode=USB, squelch=1, squelch_level=26 |
| 41733 / 1286 | 22:21:37.680 | radio → client | mode=CW, squelch=1, squelch_level=20 |
| 41733 / 1376 | 22:22:03.324 | radio → client | mode=USB, squelch=1, squelch_level=26 |
| 41733 / 1534 | 22:24:42.883 | client → radio | squelch_level=20 |
| 41733 / 1557 | 22:25:07.014 | client → radio | squelch_level=10 |
| 41733 / 1558 | 22:25:07.036 | client → radio | squelch_level=67 |
| 41733 / 1562 | 22:25:09.201 | client → radio | squelch=0 |
| 41733 / 1563 | 22:25:09.201 | client → radio | squelch_level=20 |
| 41733 / 1674 | 22:25:19.855 | radio → client | RF_frequency=7.175000, mode=LSB, squelch=0, squelch_level=20 |
| 41733 / 1885 | 22:25:42.605 | radio → client | RF_frequency=14.100000, mode=CW, squelch=1, squelch_level=20 |
| 41733 / 1934 | 22:25:42.605 | radio → client | mode=USB, squelch=0, squelch_level=20 |
| 41733 / 1970 | 22:25:42.606 | radio → client | RF_frequency=14.100000, mode=USB, squelch=0, squelch_level=20 |
| 41733 / 2125 | 22:25:47.552 | radio → client | RF_frequency=7.175000, mode=LSB, squelch=0, squelch_level=20 |
| 41733 / 2300 | 22:26:05.777 | radio → client | RF_frequency=14.100000, mode=CW, squelch=1, squelch_level=20 |
| 41733 / 2349 | 22:26:05.778 | radio → client | mode=USB, squelch=0, squelch_level=20 |
| 41733 / 2385 | 22:26:05.779 | radio → client | RF_frequency=14.100000, mode=USB, squelch=0, squelch_level=20 |
| 41733 / 2555 | 22:26:33.245 | radio → client | RF_frequency=7.175000, mode=LSB, squelch=0, squelch_level=20 |
| 41733 / 2727 | 22:26:38.376 | radio → client | RF_frequency=14.100000, mode=CW, squelch=1, squelch_level=20 |
| 41733 / 2776 | 22:26:38.376 | radio → client | mode=USB, squelch=0, squelch_level=20 |
| 41733 / 2812 | 22:26:38.376 | radio → client | RF_frequency=14.100000, mode=USB, squelch=0, squelch_level=20 |

## Restoration and limits

- Applet restoration: {'ESTABLISHED': 40}. Both band/display cleanup checks established fresh radio readback.
- Final context: 14.1 MHz USB, RX ANT1, one slice/pan. The owned test client remains open for inspection.
- No transmit-keying action was requested. TX permission remained disabled, and all 901 sampled states had transmitting false and VOX disabled (0 unsafe/unknown samples by this check). No physical RF-output or processing-response claim is made.
- Initial mute settling is recorded separately. Only a leading pre-action value is classified as settling; unrelated changes, a third value, or a reversion after the first match remain concerns.
- Remaining scenarios: AGC OFF and automatic SQL semantics; radio-setup controls/profile loads; client DSP/CHAIN; audio devices/routing; additional RX DSP/FM controls; slice creation/deletion; MultiFlex external authority; Icom and other-family mutation plans; power-cycle/crash recovery. Snapshot availability is not certification coverage.

## Independent final verification

A fresh bridge read checked original applet values again after the complete
cleanup sequence. CW delay initially appeared different only because the audit
compared the CW baseline (5) with the final USB context (41). Both values were
already present in the corresponding pre-test contexts. A receive-only CW→USB
verification confirmed 5 in CW and 41 in USB again, with no CW-delay write.
All 40 original values match when compared in their proper context. This is a
restoration-oracle lesson, not a second radio defect. The final client remains
on 20m USB / ANT1, transmitting false, with VOX and bridge TX permission off.
App and runner SHA-256 values also match the exact sources executed in the run.

## Evidence retention

Run ID: `persist-final-20260907-221202`. The operator retains the full
`persist.json`, `wire.json`, `sql-state-transitions.json`, runner-source copies,
build/test logs and independent context-verification journals under
`build/persist-evidence/` in the test worktree. These raw local artifacts are
not bundled in this PR. The tables above provide the publishable observations
and ordered SQL excerpts; the app hash identifies the binary that produced them.
