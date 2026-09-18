# FLEX-8400M persistence and TX observations — 2026-09-08

Native app: integration commit `9a8ea1b59eea5226dd6e1788608c5f9a22abc650`, SHA-256 `a785be3ed2f4904c0ac9528334024ee61ec1075bd8ddcae4166bc40b8c2dfa43`. The native binary combines the squelch fix from #5508 (`05c68846`) with the original #5500 diagnostics (`93bbff39`). The Python tooling extensions in this PR were exercised against that binary; no new native build was required. Four selected local CTests passed against those working changes: `radiocert_persist_policy`, `tx_meter_safety`, `rx_applet_squelch_reconciliation_test` and `slice_model_squelch_memory_test`. The applet reconciliation test and squelch fix belong to #5508 and are not included in this PR. CI for the updated PR head is separate from this live evidence.

Authorization: FLEX-8400M, dummy load confirmed on ANT2, 14.200 MHz, USB/LSB/AM/FM/CW/DIGU, measured ceiling 10 W. RF/Tune controls were limited to 8, SWR cutoff 2.0, app watchdog 4 s. ATU remained bypassed; no ATU RF tune or microphone/DAX audio injection.

## Antenna evidence

An independent non-GUI TCP session subscribed to radio slice status. The radio reported `RF_frequency=14.200000 txant=ANT2 tx=1` for the transmitting slice. Before and during each resumed AM/FM/CW/DIGU burst, a new independent subscription rechecked the radio-selected TX slice, antenna, mode and frequency. USB/LSB predate this added independent check; their app wire captures and model/widget snapshots are retained. This establishes radio-command/state agreement. Physical jack/relay routing was not independently measured with an external wattmeter.

Flex documents that its SWR reading can be above 1.0 on a resistive dummy load: [FlexRadio explanation](https://helpdesk.flexradio.com/hc/en-us/articles/41457432050971-Why-Does-the-Radio-s-VSWR-Not-Match-other-SWR-meters). The meter reading alone cannot identify the output port.

## RF windows

Power is radio-reported unsmoothed forward power, not an external calibration. Tune control settings are shown separately. AetherSDR’s SWR conversion matches FlexLib Meter.cs: raw SWR divided by 128.

| Mode | Trigger | Tune control | Peak W | Peak SWR | Independent radio ANT2 before/during | Unkey confirmed | Result |
|---|---|---:|---:|---:|---|---|---|
| USB | carrier | 2 | 1.628 | 1.211 | not sampled independently | True | stopped: harness used wrong liveness field (repaired) |
| USB | carrier | 2 | 1.649 | 1.211 | not sampled independently | True | positive RF observed |
| USB | carrier | 5 | 4.278 | 1.180 | not sampled independently | True | positive RF observed |
| USB | carrier | 8 | 6.891 | 1.141 | not sampled independently | True | positive RF observed |
| USB | two-tone | 2 | 1.480 | 1.344 | not sampled independently | True | positive RF observed |
| LSB | carrier | 2 | 1.652 | 1.211 | not sampled independently | True | positive RF observed |
| LSB | carrier | 5 | 4.278 | 1.148 | not sampled independently | True | positive RF observed |
| LSB | carrier | 8 | 6.953 | 1.133 | not sampled independently | True | positive RF observed |
| LSB | two-tone | 2 | 1.470 | 1.234 | not sampled independently | True | positive RF observed |
| AM | carrier | 2 | 1.652 | 1.211 | yes | True | positive RF observed |
| AM | carrier | 5 | 4.278 | 1.195 | yes | True | positive RF observed |
| AM | carrier | 8 | 6.879 | 1.148 | yes | True | positive RF observed |
| AM | ptt | 2 | 0.184 | 1.000 | yes | True | inconclusive: below 0.3 W reporting threshold |
| FM | carrier | 2 | 1.652 | 1.211 | yes | True | positive RF observed |
| FM | carrier | 5 | 4.301 | 1.148 | yes | True | positive RF observed |
| FM | carrier | 8 | 6.891 | 1.125 | yes | True | positive RF observed |
| FM | ptt | 2 | 4.332 | 1.148 | yes | True | positive RF observed |
| CW | carrier | 2 | 1.737 | 1.219 | yes | True | positive RF observed |
| CW | carrier | 5 | 4.556 | 1.164 | yes | True | positive RF observed |
| CW | carrier | 8 | 7.326 | 1.148 | yes | True | positive RF observed |
| DIGU | carrier | 2 | 1.652 | 1.211 | yes | True | positive RF observed |
| DIGU | carrier | 5 | 4.332 | 1.195 | yes | True | positive RF observed |
| DIGU | carrier | 8 | 6.966 | 1.125 | yes | True | positive RF observed |
| DIGU | two-tone | 2 | 1.499 | 1.219 | yes | True | positive RF observed |

AM PTT produced about 0.18 W and stopped at the harness positive-carrier criterion. Its TUNE windows succeeded; AM PTT remains inconclusive. All interrupted attempts remain in the journal: a pre-key scratch-script event argument error, an incorrect harness liveness field (fixed to the bridge’s `connected` field), the operator-requested dummy-load pause, and the AM low-carrier stop.

## TX control evidence

Each set/restore row samples the model and actual widget three times. Protocol columns list exact captured commands and correlate replies by client PID plus command sequence. A successful command reply is not an independent measurement of microphone DSP behavior.

| Control | Set path | Captured protocol values | Radio command replies | Actual widget | External change | Normal process restart | Restoration |
|---|---|---|---|---|---|---|---|
| EQ 125 | `setValue` | `eq TXsc 125Hz=0`<br>`eq TXsc 125Hz=2` | {'0': 2} | ESTABLISHED | not run | ESTABLISHED | ESTABLISHED |
| EQ 1k | `setValue` | `eq TXsc 1000Hz=0`<br>`eq TXsc 1000Hz=5` | {'0': 2} | ESTABLISHED | not run | ESTABLISHED | ESTABLISHED |
| EQ 250 | `setValue` | `eq TXsc 250Hz=0`<br>`eq TXsc 250Hz=3` | {'0': 2} | ESTABLISHED | not run | ESTABLISHED | ESTABLISHED |
| EQ 2k | `setValue` | `eq TXsc 2000Hz=0`<br>`eq TXsc 2000Hz=1` | {'0': 2} | ESTABLISHED | not run | ESTABLISHED | ESTABLISHED |
| EQ 4k | `setValue` | `eq TXsc 4000Hz=0`<br>`eq TXsc 4000Hz=2` | {'0': 2} | ESTABLISHED | not run | ESTABLISHED | ESTABLISHED |
| EQ 500 | `setValue` | `eq TXsc 500Hz=0`<br>`eq TXsc 500Hz=4` | {'0': 2} | ESTABLISHED | not run | ESTABLISHED | ESTABLISHED |
| EQ 63 | `setValue` | `eq TXsc 63Hz=0`<br>`eq TXsc 63Hz=1` | {'0': 2} | ESTABLISHED | not run | ESTABLISHED | ESTABLISHED |
| EQ 8k | `setValue` | `eq TXsc 8000Hz=0`<br>`eq TXsc 8000Hz=3` | {'0': 2} | ESTABLISHED | not run | ESTABLISHED | ESTABLISHED |
| Equalizer enable | `setChecked` | `eq TXsc mode=False`<br>`eq TXsc mode=True` | {'0': 2} | ESTABLISHED | not run | ESTABLISHED | ESTABLISHED |
| AM carrier level | `setValue` | `transmit set am_carrier=41`<br>`transmit set am_carrier=48` | {'0': 2} | ESTABLISHED | not run | not run | ESTABLISHED |
| CW delay | `setValue` | `cw break_in_delay 170`<br>`cw break_in_delay 5` | {'0': 2} | ESTABLISHED | not run | not run | ESTABLISHED |
| CW speed | `setValue` | `cw wpm 23`<br>`cw wpm 30` | {'0': 2} | ESTABLISHED | not run | not run | ESTABLISHED |
| Downward expander | `setChecked` | `transmit set compander=0`<br>`transmit set compander=1` | {'0': 2} | ESTABLISHED | not run | ESTABLISHED | ESTABLISHED |
| DEXP threshold | `setValue` | `transmit set compander_level=0`<br>`transmit set compander_level=33` | {'0': 2} | ESTABLISHED | not run | ESTABLISHED | ESTABLISHED |
| Microphone gain | `setValue` | `transmit set miclevel=37`<br>`transmit set miclevel=82` | {'0': 2} | ESTABLISHED | not run | ESTABLISHED | ESTABLISHED |
| Sidetone volume | `setValue` | `transmit set mon_gain_cw=19`<br>`transmit set mon_gain_cw=80` | {'0': 2} | ESTABLISHED | not run | not run | ESTABLISHED |
| Monitor volume | `setValue` | `transmit set mon_gain_sb=29`<br>`transmit set mon_gain_sb=55` | {'0': 2} | ESTABLISHED | not run | ESTABLISHED | ESTABLISHED |
| CW audio pan | `setValue` | `transmit set mon_pan_cw=37`<br>`transmit set mon_pan_cw=50` | {'0': 2} | ESTABLISHED | not run | not run | ESTABLISHED |
| TX monitor | `setChecked` | `transmit set mon=0`<br>`transmit set mon=1` | {'0': 2} | ESTABLISHED | not run | ESTABLISHED | ESTABLISHED |
| RF power | `setValue` | `transmit set rfpower=100`<br>`transmit set rfpower=5` | {'0': 6} | ESTABLISHED | ESTABLISHED | ESTABLISHED | ESTABLISHED |
| Speech processor | `setChecked` | `transmit set speech_processor_enable=0`<br>`transmit set speech_processor_enable=1` | {'0': 2} | ESTABLISHED | not run | ESTABLISHED | ESTABLISHED |
| Processor level | `setValue` | `transmit set speech_processor_level=1`<br>`transmit set speech_processor_level=2` | {'0': 2} | ESTABLISHED | not run | ESTABLISHED | ESTABLISHED |
| Tune power | `setValue` | `transmit set tunepower=10`<br>`transmit set tunepower=2`<br>`transmit set tunepower=5`<br>`transmit set tunepower=8` | {'0': 22} | ESTABLISHED | not run | ESTABLISHED | ESTABLISHED |
| VOX delay | `setValue` | `transmit set vox_delay=29`<br>`transmit set vox_delay=45` | {'0': 2} | ESTABLISHED | not run | ESTABLISHED | ESTABLISHED |
| VOX level | `setValue` | `transmit set vox_level=34`<br>`transmit set vox_level=50` | {'0': 2} | ESTABLISHED | not run | ESTABLISHED | ESTABLISHED |

External-change proof was exercised for RF power only, before and after normal restart. CW and AM controls were exercised in their respective modes; their independent external-change and restart cases were not run. VOX enable, CW break-in/keyer output, microphone boost/bias, accessory delays and profile loads were not exercised. TUNE/two-tone does not establish speech-processor, expander, microphone or TX-EQ audio effects.

## Meter limits

All recorded burst exits confirmed unkeyed, and the visible power gauge read zero immediately after unkey and again after the late-reply interval. FWDPWR and SWR were sampled with freshness checks. The safety cutoff used `fwdPowerInstant`, not the smoothed display. PACURRENT is flagged unreliable for this Flex model and is not a calibrated-current claim. ALC under internal TUNE is not microphone-chain verification. The maximum observed FWDPWR sample age was 68 ms. Raw JSON preserves meter units/ages and visible forward-power gauge observations before, during, immediately after and 0.7 s after unkey.

## Restoration

The full final comparison found receive AGC threshold/off-level at 50/50 versus the original 65/10 after the mode sweep. A separate guarded RX-only cleanup restored both values through the actual controls; the original mismatch snapshot is retained in `tx/final-before-agc-cleanup.json`. The subsequent [receive-only isolation](persist-flex-agc-fm-boundary-2026-09-08.md) reproduced a radio-side FM transition reset with the app suspended and verified a fresh subscription. Whether it is intended firmware behavior or a Flex defect remains unconfirmed.

TX journal status: `completed-with-recorded-AGC-cleanup-extension`.

Final: one slice, 14.1 MHz USB, TX ANT1, RF control 100, Tune control 10, ATU manual_bypass. Original power and antenna settings were restored only after relaunch without TX permission. Tuning=False, MOX=False, model transmitting=False, radio transmitting=False.

## Two-slice RX persistence

Final runner status: `completed`. Outcomes: {'ESTABLISHED': 18}.

| Scenario | Outcome |
|---|---|
| seed A | ESTABLISHED |
| seed B | ESTABLISHED |
| active selection A | ESTABLISHED |
| active selection B | ESTABLISHED |
| active selection A | ESTABLISHED |
| second slice SQL off, first unchanged | ESTABLISHED |
| first slice SQL remains manual | ESTABLISHED |
| second slice SQL manual restored | ESTABLISHED |
| mode round trip A | ESTABLISHED |
| mode round trip B | ESTABLISHED |
| two slices after normal restart | ESTABLISHED |
| post-restart selected B | ESTABLISHED |
| post-restart selected A | ESTABLISHED |
| restore slice B | ESTABLISHED |
| original survives removal | ESTABLISHED |
| original survives reopen; new slice inventoried | ESTABLISHED |
| restore slice A | ESTABLISHED |
| restore original pan center | ESTABLISHED |

The operator retains raw journals for run `validation-multislice-20260908` (`tx`, `rx`, `rx-final`) and the exact runner-source snapshots. Raw identifying radio data is excluded from this PR. This report is the publishable evidence summary. The first two-slice run preserved a pan-center cleanup mismatch; the final repeat exercises the corrected guarded cleanup end to end. Multiple panadapters, MultiFlex, radio power cycle, Icom and physical receive-audio squelch gating remain untested.
