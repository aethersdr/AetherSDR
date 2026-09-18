# RadioCert Persist: first live FLEX-8400M run

September 7, 2026 Pacific. FLEX-8400M (exact serial selected),
firmware 4.2.18.41174. Build ab03be0629a4 with working-tree changes.
One client, one pan, one slice; dedicated test settings profile and stable
GUIClientID. Authorized RX ports: ANT1 (live antenna), ANT2 (no antenna).
**Transmit was not authorized and the automation TX gate remained disabled.**

## Findings

| Area | Live observation | Interpretation |
|---|---|---|
| FFT AVG/FPS immediate writes | Display adopted 19/18 on 20m and 43/30 on 40m; the pan model kept its previous values. The wire shows successful command responses. | A same-session readback gap. Model inequality is not evidence that the radio failed to save the setting. |
| Band persistence | Subsequent radio status explicitly published 19/18 for 20m and 43/30 for 40m; model and presentation then agreed. | Radio retention observed for both tested band contexts. |
| Mode/filter | 20m USB 300..2650 Hz and 40m LSB -2600..-200 Hz matched after band revisits; the 20m CW→USB round trip recovered the intended USB filter. | Tested tuples retained; no claim about other modes or filter slots. |
| RX antennas | 20m ANT1 and 40m ANT2 returned correctly; 20m ANT1→ANT2→ANT1 retained the tested mode/filter/display fields. | RX antenna context and unaffected-field agreement observed. Quiet ANT2 was expected. |
| Client preferences | Grid on and palette index 2 survived mode/band/antenna changes and normal restart. | Client presentation retention observed in the isolated profile. |
| Full restart | PID 38890 exited normally with code 0; PID 38923 used the same profile and GUIClientID. The first restored 20m status and subsequent 40m revisit matched the test values. | A real normal-quit/relaunch, not a reconnect masquerading as restart. |
| Cleanup | Original 20m USB 100..2800 Hz, ANT1, AVG 50/FPS 25, Grid on/palette 0 restored; original 40m LSB -2800..-100 Hz, ANT1, AVG 50/FPS 25 verified. | Fresh band-status observations establish restoration of the fields we changed. Final state: 14.100 MHz USB ANT1, one slice/pan, transmitting=false. |

The raw runner verdicts intentionally remain conservative: seed discrepancies
produce CONCERN, and subsequent agreements are INCONCLUSIVE as complete retention
verdicts until the seed discrepancy is understood. This assessment additionally
uses the command/status timeline, rather than silently relabeling those verdicts.

## Read → write → read race inspection

Normal-restart process 38923, ordered application log sequence numbers:

| Time | Sequence | Event |
|---|---:|---|
| 21:17:04.974 | 67 | Initial pan status: AVG 0 / FPS 25, 50×20 pixels. |
| 21:17:04.992 | 80–81 | Client sends WNB off / level 50. These values match the initial status; this occurs before the later restored pan state. |
| 21:17:05.071 | 137, 148 | Radio publishes restored AVG 19 / FPS 18. |
| 21:17:05.184 | 199 | Client sends FPS 18, matching the restored radio value. |
| 21:17:05.259 | 245 | Subsequent radio status still reports AVG 19 / FPS 18. |
| 21:17:15.373 | 946 | Revisited 40m status reports AVG 43 / FPS 30 and RX ANT2. |

**The incorrect-value AVG/FPS replay was not reproduced in this captured run.**
The initial default-like status followed by a later restored status is real,
so a final snapshot alone would miss the ordering hazard. WNB startup writes
before restored state remain a concrete next scenario: seed distinct radio-owned
WNB values, then inspect whether the early client write replaces them. No WNB
control was deliberately changed in this run, so that failure is not established.

The immediate-write discrepancy has a specific wire example: sequence 826 sends
`C73|display pan set 0x40000000 average=19`, and 827 receives `R73|0|19`.
The widget shows 19 while the pan model still shows 17. The next 20m band recall
publishes 19 from the radio at sequence 1236. This explains why both seed and
immediate cleanup comparisons were misleading; fresh radio status resolves them.

## Evidence and tool gaps

The operator retained local journals for `radiocert-persist-flex8400-20260907-211107`
and `radiocert-persist-flex8400-continuation-20260907-211531`, including the merged
ordered wire capture and verified cleanup. Raw local artifacts are not bundled
in this PR; the compact evidence above is self-contained.

Startup originally exceeded the runner's 500-event tail request. The still-live
ring was recovered separately, yielding contiguous recorded sequences 1..1930
for process 38890 and 1..1708 for process 38923. The runner now requests 5000
records and reports detected gaps rather than implying a complete trace.
The first attempt's client did not remain open after its supervisor completed;
owned app processes now use their own process session. The subsequent normal
restart has explicit successful process-exit evidence.

Next improvements: field-specific observation provenance (ACK, fresh status,
model, widget); resolve seed concerns per field instead of downgrading unrelated
fields; validate cleanup through fresh authority observations; retain ordered
startup traces continuously; then add the distinct-value WNB race and separately
prepared MultiFlex external changes. Multiple slices/pans, Icom, radio power
cycles and deliberately varied startup timing were not tested here.

These are live radio-status and client observations, not physical RF measurement
or an exhaustive certification. No backend behavior was changed during this run.
