# Icom restart fixes — IC-7300MK2 proof, 2026-09-08

This is a focused repair and verification of the four discrepancies recorded by
[RadioCert Persist #5500](https://github.com/aethersdr/AetherSDR/pull/5500).
It is not a new full-radio certification or a TX test. The intermittent report
of unexpected Auto activation remains unreproduced.

## Findings and repairs

| Finding from Persist | Root cause and final behavior | Final live proof |
|---|---|---|
| RX-ANT remained selected in the radio but the app showed ANT1 after all four earlier restarts | MK2 readback was disabled after `12 00` returned FB. Bare `12` returns `12 00 00/01`. Startup, periodic and post-write reads now publish radio truth to the model and both RX antenna controls. The scheduler matches the reply's `00` subcommand although the query has none. | RX-ANT survived both normal restarts and both reconnects; fresh `12 00 01`, model and both actual controls agreed. Restoration produced `12 00 00` and ANT1 in both controls. |
| Manual 26 → Off → restart → Manual returned 20 | Icom Off sets its threshold register to zero. The previous manual choice existed only in the destroyed slice. Scoped client `SquelchIntent.manualLevel` preserves that choice for a later operator action without replaying it during startup. | Off remained Off/zero after restart. Clicking Manual sent/read `14 03 00 67`, and the actual threshold slider showed 26. The same sequence passed on reconnect. |
| Explicit Auto became Manual after process restart | The client algorithm's Auto intent was transient. Scoped `SquelchIntent.autoEnabled` resumes it only after fresh enabled radio SQL readback; a radio reporting Off cancels old Auto intent. | AUTO rendered after normal restart and reconnect. Returning to Manual recovered 26 in both cases. No attach-time default replay is permitted by the injected-state tests. |
| Waterfall rate 63 became 100 after all four earlier restarts | Icom's waterfall cadence is shaped by the client, but was following the non-persisted radio-display path. Scoped `ClientDisplay.waterfallRates` now stores explicit user changes by pan slot and seeds the local rate shaper on wiring. | Display state and the actual Waterfall rate slider remained 63 after both restarts and reconnects. Restored to the original 100. |

The waterfall slider was already wired to local display speed; this repair adds
persistence. It does not set a native Icom waterfall speed over CI-V. Radio-owned
operating values remain radio-authoritative. Flex does not read or write these
Icom squelch documents or persist its radio-owned cadence through this helper.

Both client documents use exact per-radio rows, atomic checked writes, versioned
schemas and range checks. They preserve unrelated fields and refuse unreadable
or newer documents. Passive manual readback remains authoritative. Auto algorithm
threshold updates do not replace the remembered manual choice.

## Family coverage

| Repair | IC-7300MK2 | IC-705 | IC-9700 |
|---|---|---|---|
| RX-ANT readback | Enabled by its model profile; live proven | Not enabled; profile guard tested | Not enabled; profile guard tested |
| Previous manual SQL choice | Shared Icom path; live proven | Shared Icom path; hardware untested here | Shared Icom path; hardware untested here |
| Explicit Auto intent | Shared Icom path; live proven | Shared Icom path; hardware untested here | Shared Icom path; hardware untested here |
| Client waterfall rate persistence | Local shaping path; live proven | Same path; hardware untested here | Same path; hardware untested here |

These shared-path conclusions are source-level applicability, not separate
IC-705/IC-9700 hardware certification. This change does not add secondary-receiver
lifecycle coverage to the current Icom backend.

## Final validation

- Source: main base `9c40094480e5ea4d8fc9abc2c9236215cbbb561d` plus this PR's changes.
- macOS app SHA-256: `2e241a900b353a6d3c54d694165bcd52cbf37e320944098a7526e0b6f59a301d`.
- Built using `cmake --build build -j22`. The first full build needed host access
  for macOS icon generation; that sandbox failure was not a source failure.
- Ten selected headless CTests passed on the final changes: `icom_civ_test`,
  `icom_civ_scheduler_test`, `icom_control_profile_test`, `icom_meters_test`,
  `icom_family_test`, `slice_model_squelch_memory_test`,
  `rx_applet_squelch_reconciliation_test`, `client_display_settings_test`,
  `waterfall_rate_test`, and `display_status_gate_test`.
- Mutation checks: disabling antenna adoption, SQL-intent saves, and waterfall
  saves caused all three targeted regressions to fail. Both Off/manual-memory
  and explicit-Auto cases failed. A further test failed on the uncorrected bare
  query/reply matcher, then passed with the final response shape.
- Tests inject state/frames into real code with unstarted sessions. No new radio
  peer, socket listener, or live-hardware dependency enters CTest.
- Strict engine-boundary and test-registration checks passed; no diff whitespace
  errors. CI is separate from these local results.

The final hardware run used an isolated settings profile, session-only credentials,
one IC-7300MK2 at CI-V B6, one slice, and three separate app processes. TX permission
was disabled in the bridge throughout. Sixteen bounded observations established the
requested state across two normal Quit/relaunch cycles, two same-process reconnects,
manual/Off/Auto transitions, and restoration. Each radio-backed verification required
a new expected CI-V reply and agreement in model and actual controls. Intermediate
snapshots were retained; the criterion required two consecutive agreeing samples.

Captured `rx.antenna` transaction samples completed as `reply` in 3–26 ms. Every
sampled scheduler snapshot reported zero timeouts. The script's control convergence
durations include waiting for a subsequent periodic read and are not measurements
of initial connection freshness. They must not be substituted for a future full
RadioCert freshness distribution or a scheduler priority benchmark.

The first live iteration proved the visible state but final review exposed the
query/reply matcher discrepancy. The final app was rebuilt and the entire focused
sequence repeated; only the final binary above supports the results here.

Original Manual 14, ANT1 and waterfall 100 were restored. Original and restored
slice, pan and transmit snapshots had no differing fields. Fresh restoration
readbacks were `14 03 00 36` and `12 00 00`. MOX, tuning and transmitting were false;
the app quit normally and the exclusive lab lock was released. No transmission
was performed, and filter preset/PBT behavior was not changed.

Raw final evidence is retained locally under
`build/icom-restart-proof-final/` (journal, snapshots, transaction samples and
captures). Detailed session-derived captures and observations remain local;
the PR reports validation results without public evidence attachments.
