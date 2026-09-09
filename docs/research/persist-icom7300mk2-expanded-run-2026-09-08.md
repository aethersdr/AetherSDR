# IC-7300MK2 expanded Persist run and combined findings

The expanded Icom lab matrix completed mode/band/control, same-process reconnect,
normal process restart, display, receive antenna, selected/unselected VFO,
custom-filter, FM tone and TX-filter observations. It found four process-restart concerns, two reproduced at all four
retention restarts. This is a bounded diagnostic run, not a whole-radio pass or a
production fix. The [first Icom round](persist-icom7300mk2-first-run-2026-09-08.md)
and [Flex handoff](persist-flex-to-icom-handoff-2026-09-08.md) remain part of the
combined evidence; later success does not erase their failures.

**Restoration limitation:** the 30 FIL1/FIL2/FIL3 button/semantic preset recalls
reset factory width/PBT definitions. The experiment did not capture every
hidden per-mode/slot definition before those recalls. It restored the known
original active LSB FIL1 and the captured post-recall USB FIL3 baseline, but
cannot restore or certify the unknown prior filter-bank customizations. This
is a test design and documentation gap, not evidence that the radio forgot
those values. Capture each slot with a slot-only operation before future sweeps.

## Build and method

- IC-7300MK2, directed CI-V address B6, RS-BA1 network connection. Firmware
  version was not exposed. Exact connected model and one owned slice were
  required before each action; an available radio lock was acquired first.
- Existing integration app commit `9a8ea1b59eea5226dd6e1788608c5f9a22abc650`,
  combining original Persist #5500 (`93bbff39`) and squelch #5508 (`05c68846`).
  Executable SHA-256 `a785be3ed2f4904c0ac9528334024ee61ec1075bd8ddcae4166bc40b8c2dfa43`
  was reverified. **No new native build or native test run** occurred this round.
  Results cannot be attributed to standalone #5500 or an Icom repair.
- Dedicated profile and session-only credentials; private identity and transport
  artifacts remain local. Existing public Persist mutation runners still reject
  Icom. This used explicit local lab scripts, not a newly shipped Icom runner.
- Readback evidence combines actual widget actions, snapshots and fresh matching
  CI-V replies. Each stable observation required two matching samples. Full
  intermediate mismatches and failed attempts remain in the local journal.
  Snapshot timing bounds observation latency, not exact radio sampling time.
- Four normal Quit/new-process retention transitions, seeded in Manual, Off,
  Auto and Manual/TX-filter contexts. Six separate same-process reconnect
  trials used Off/Manual/Auto twice. A final RX-only cleanup restart is excluded
  from that retention count. Every Quit first verified transmit inactive.
- TX remained separately authorized for ANT1 dummy load, 7.200 MHz, all modes,
  maximum physical 10 W. The TX-enabled app had a three-second watchdog and
  five-percent control ceiling. No VOX enable or ATU cycle was requested.

## Executed matrix

| Surface / transition | Evidence and result |
|---|---|
| Ten mode routes | LSB, USB, AM, FM, DFM, DIGU, DIGL, RTTY, CW and CWL obtained matching CI-V mode replies and model/UI observations. RTTY has normalized model DIGL but displayed RTTY; CWL displays CW. Initial harness alias mismatches were corrected and retained. |
| Filter presets | Thirty factory recalls, FIL1/2/3 in each mode, obtained matching slot readback. Mode changes carry the current slot into the new mode; distinct per-mode selections were not independently retained. Coincidental later matches are not ten separate persistence passes. |
| Primary control vector | SQL 26, AGC Fast, AF 17, RF 3%, microphone 37, NR/NB/ANF on, processor off, monitor on/31, RF gain 67, CW speed 24 and break-in off were established. The 14 values agreed after the mode sweep and at five band checkpoints. Applicable actual controls were compared, including after restart. |
| Band round trips | 20 m USB → 80 m LSB → 40 m LSB → 20 m → 40 m retained the seeded primary vector. These were receive/control operations; RF was emitted only at the authorized 7.200 MHz. |
| SQL lifecycle | All six valid same-process reconnects retained their requested Off, Manual 26 or explicit Auto intent. Normal restart retained active Manual 26 and Off/zero, but lost the previous manual cache while Off and explicit Auto intent; details below. Unexpected Auto activation was not reproduced. |
| Additional radio controls | NR/NB levels, preamp 0/1/2/0, attenuation 0/20 dB/0, RIT/XIT on/off/zero and 10 Hz wheel changes, shared offset 120 with both disabled, CW pitch 605 and VOX gain 34 with VOX off were exercised. NR request 23% was normalized by the radio to raw 56/model 21; this is not a retention failure. |
| Passive model/UI adoption | Safe raw writes to SQL 39, AGC Slow, AF 29, microphone 43 and RF gain 73 bypassed their widget setter and reached matching model/widgets before and after restart. This tests the inbound path, but is not a physical front-panel latency experiment. |
| Custom filter | USB FIL3 request 0…2300 Hz became radio width 2300, PBT registers 111/111 and normalized model edges 42…2342. A corrected slot-only revisit after normal restart retained all of those values. The earlier observer clicked FIL3 and reset the definition; that trial is inconclusive. |
| Receive antenna | RX-ANT was selected and independently read as `12 00 01`. After all four process retention restarts the radio still returned RX-ANT, while model and selector said ANT1. TX antenna remained ANT1; RX selection and TX routing are separate. |
| Display | Waterfall color gain 37 and auto-black offset 43 retained. Rate 63 returned to 100 after all four process retention restarts. Scope Zoom in did not establish a change from 0.2 to 0.1 MHz, including a trial without an injected read; distinct-span retention remains unproved. |
| Unselected VFO | Direct unselected VFO writes/readback established 14.175 MHz USB FIL3, with selected VFO unchanged, and retained it through restarts. `07 B0` exchange returned explicit `FA`; no successful A/B exchange is claimed. The backend does not expose the unselected VFO as another slice. |
| TX passband | SSB WIDE edges 200/2800 and DATA edges 300/2900 agreed with their packed CI-V registers and actual controls, then retained through the fourth process restart. All four WIDE/MID/NAR/DATA registers were captured and compared; only WIDE and DATA were changed. |
| FM tone | CTCSS TX at 100.0 Hz obtained mode/value readback, survived USB→FM and a normal restart, and was restored to off/67.0. This is setpoint/enable retention, not an RF tone-deviation measurement. |

## Combined actionable list

The following keeps confirmed discrepancies separate from incomplete coverage
and firmware-policy questions. The first four are new, separately observed
process-restart problems; they should not be collapsed into one squelch bug.

| Priority / item | Established evidence | Concrete next work |
|---|---|---|
| 1. Icom RX antenna adoption | Four restarts: radio `12 00 01`, app ANT1. The backend profile currently marks antenna readback unavailable although this model answered the read. | Trace profile capability, startup query and reply-to-model routing; inject `12` replies in a socket-free regression, then prove RX-ANT/ANT1 on hardware across restart. Do not infer physical TX routing from this receive selector. |
| 2. Icom manual SQL cache while Off | Manual 26 → Off → normal restart → Manual sends/adopts 20. Active Manual 26 and same-process Off/Manual reconnect retain correctly. | Define the scoped owner of the last manual value while radio threshold is zero; prevent unrelated startup defaults from becoming remembered operator intent. Test client cache and raw threshold separately. No blanket replay of radio-owned SQL. |
| 3. Icom explicit Auto intent | Explicit Auto before Quit returns as Manual after new-process startup; same-process Auto reconnect retains Auto in both trials. | Specify client Auto-intent retention, then test applet attach/startup ordering and automatic threshold writes. This does not explain the reported unexpected Auto activation; keep that intermittent report open. |
| 4. Icom waterfall rate | Seed 63 → new process 100, four times, while color 37/offset 43 retained. | Trace client display persistence and startup wiring that sets waterfall duration; test stored value, model and rendered consumer separately. |
| 5. Icom freshness/readiness | First round showed startup SQL 20/AGC Med before fresh adoption at 2.548/3.077 s, background confirmation ages up to about 3.3 s, and several identity failures. Current valid reconnect/restart trials do not prove an identity fix. | Expose per-field/context confirmation age and unknown/pending state. Add stable transaction event IDs before comparing scheduler latency distributions; prioritize critical confirmations without starving background fields. |
| 6. Filter operation semantics / observer effects | Current `setSliceFilterPreset` deliberately recalls factory width and centered PBT, while mode change carries the current slot. The test initially treated recall as selection, and reset hidden originals. | Correct docs and make future Persist capture all slots with raw slot-only mode/filter selection before any recall. Separate “select stored slot” from “recall factory preset” expectations; review desired per-mode UX explicitly. |
| 7. Scope and VFO gaps | Scope narrowing was not established. VFO exchange returned `FA`; direct unselected read/write retained correctly. | Reduce each operation against the MK2 guide and actual operating context. Do not change memory/VFO mode blindly or claim multi-receiver coverage. |
| 8. Icom TX telemetry paths | First round CW text lacked fresh forward-power telemetry; initial AM/DIGU zero-output attempts stopped. Later AM/two-tone successes do not resolve those paths. | Investigate CW keying/meter scheduling and TUNE/drive semantics separately, keeping initial freshness/SWR gates and measured-watt limits. |
| 9. Flex FM/AGC behavior | Independent direct radio status changed threshold/off-level to 50/50 across FM with the app suspended; model/UI subsequently agreed. | Determine intended firmware/SmartSDR behavior before a client change. The newly expanded two-slice FM/AGC OFF runner still needs its own live run. |
| 10. Flex squelch repair | Two-slice Off/Manual and level isolation were established in the earlier combined-build run. Repair remains #5508; Persist tooling/reporting remains #5500. | Preserve that separation. Audio gating, MultiFlex and multiple-pan evidence remain separate work. |

Relevant source paths are `IcomCivBackend::setSliceFilterPreset`,
`filterPresetRecallPlan`, `IcomCivBackend::setSliceMode`, model control publication,
`RxApplet` SQL mode/cache handling and waterfall initialization in
`MainWindow_Wiring.cpp`. Source explanations are investigation leads unless a
controlled regression and live before/after prove causality.

## Guarded transmit and restoration

After the fourth retention restart, one 1.6-second USB two-tone window used
7.200 MHz, ANT1, RF 3% and Tune 2%. It obtained six fresh positive forward-power
samples, peak unsmoothed calibrated **2.098 W** and SWR **1.0**, with no stop
reason. The actual forward gauge was sampled while keyed; explicit unkey
succeeded and both immediate/later gauge observations were zero. This is radio
meter and protocol evidence, not an independent wattmeter or RF-jack measurement.
Across the two Icom rounds, ten of thirteen windows established positive RF;
three first-round stops remain unresolved. Highest peak remains the earlier
7.692 W. Do not interpret percentages as physical watts.

Final slice, pan, transmit, EQ and display model comparisons matched the captured
baseline with no differences. Active state returned to 7.224540 MHz LSB FIL1,
Manual SQL 14, AGC Med, AF 0, RF gain 100, microphone 80, NR/NB/ANF off,
monitor off/50, processor on, CW speed 21/pitch 600/semi break-in, VOX off/gain 50,
and RIT/XIT off/zero. Exact hardware compressor level 221 was restored after
the processor setter wrote its coarse level. Raw replies also confirmed RX
ANT1, the original unselected 0.580000 MHz AM FIL1 and all four original TX
bandwidth registers. RF 100% and Tune 10% were restored only in the final
TX-disabled process. Waterfall color/offset/rate returned to 50/50/100. The tuner
was bypassed at this run's baseline and remains bypassed; no ATU cycle occurred.

The full model match does **not** cover the uncaptured hidden filter definitions
described above. No complete filter-bank restoration is claimed. Final TX flags
were false, the app quit normally, diagnostic logging was reset and the radio
lock was released.

## Measurement and harness corrections retained

- Raw diagnostic reads use an operator-command scheduling path. An immediate
  read of the same semantic key can coalesce/replace a pending raw write, and
  a first arriving reply can belong to an older in-flight read. Corrected
  probes separated write dispatch and matching value confirmation before
  another read. Do not use the initial overlapping attempts as latency proof.
- VFO exchange rejection did not abort one scratch sequence; its next tune
  briefly changed the selected VFO to 14.185 MHz. That was receive-only,
  journaled and restored immediately. The corrected unselected-VFO trial
  used direct addressed writes with exact readback and no exchange.
- RIT/XIT `setValue` on a ScrollableLabel was refused. Existing wheel actions
  then established +10 Hz and zero. No new bridge verb is needed for that gap.
- Initial alias, NR quantization and exact filter-edge expectations were too
  strict. Their first mismatches remain recorded beside corrected observations.
- One restoration UI lookup used the inactive DSP button name while its actual
  accessible name included “(DSP active)”; it stopped before that action. The
  observed name was used to resume cleanup.
- A repeated no-op AF restoration did not yield the expected fresh reply within
  that observation window; the final actual control/model baseline matched zero.
  Setter no-op and missing confirmation remain distinct from a value mismatch.
- Numeric checkpoint totals would include repetitions, corrected expectations
  and cleanup observations. They are not independent test cases or a pass rate.

## Evidence retention and remaining scope

Run ID `persist-icom-matrix-20260908`. The operator retains `observations.jsonl`,
all startup/reconnect snapshots, raw register originals/seeds/restoration,
widget trees, TX samples, exact scratch sources, client logs and hashes under
that ignored build directory. Public reports exclude credentials, radio IP,
login, socket names and session/profile identifiers.

Physical front-panel changes while disconnected, RF/audio gating, independently
measured jack routing, memory/profile recall, crash/power-cycle recovery,
physical microphone/DAX/VOX/ATU effects and all hidden filter definitions remain
unverified. IC-7300MK2 advertises one slice; Flex multislice assumptions do not
apply. Per-setting freshness telemetry, scheduler tuning and production repairs
are proposals for follow-up, not changes made by this report.

Local report validation: 45 socket-free Python Persist policy cases and all 12
TX safety checks passed in the publication worktree; bridge-doc generation and
whitespace validation passed. An initial `unittest` module invocation lacked the
tools import path; direct documented script entry points completed successfully.
These are tooling checks, not new native-build or hardware-fix evidence.
