# AetherModem AX.25 Notes

This file captures the AX.25 modem bring-up notes for the AetherSDR AetherModem
window. It covers the original 300 baud HF profile and the 1200 baud VHF (Bell
202 / 2m APRS) profile added afterward.

The working test packet has been:

```text
KI6BCJ-1>APDW18:!3644.00N\11947.00W-KI6BCJ HF APRS test via Direwolf 300 baud
```

## Test Setup

| Item | Value |
| --- | --- |
| Transmit source | Dire Wolf AX.25 APRS packet audio |
| Dire Wolf modem | AFSK 1600/1800 Hz, A+, 300 baud |
| Receiver mode | DIGU |
| Receiver frequency | 14.241.000 MHz during these tests |
| Receiver filter | 3 kHz |
| Decoder polarity | Normal decoded, Reverse produced 0 accepted frames in recorded tests |
| Decoder sample rate | 24 kHz post-demod receive audio |

## Captures

| Label | File | Notes |
| --- | --- | --- |
| Capture A | `/Users/patj/Library/Preferences/AetherSDR/ax25-rx-capture-20260517-020519Z-float32.wav` | Earlier 3 minute recording. Current best replay is 20 accepted frames. |
| Capture B | `/Users/patj/Library/Preferences/AetherSDR/ax25-rx-capture-20260517-023828Z-float32.wav` | Latest 3 minute recording. User observed 13 of 21 live with the older 5-lane build. Current best replay is 18 of 21. |
| Capture C | `/Users/patj/Library/Preferences/AetherSDR/ax25-rx-capture-20260517-033512Z-float32.wav` | Latest live test with the 21-lane build. Window reached 19 accepted frames; replay of the saved capture produced 18 accepted frames because the 19th live decode happened after the capture file had already been saved. |
| Short captures | `ax25-rx-capture-20260516-190036Z-float32.wav`, `ax25-rx-capture-20260516-192810Z-float32.wav` | 30 second captures. Replayed 2 accepted frames each after phase-diversity work, likely matching the number of packet bursts in those files. |

Capture B contains 21 transmit bursts. Each burst was about 2.4 seconds long and arrived at a similar level, with burst RMS around -18 dBFS and peaks around -11 dBFS. The missing frames were therefore not simply low-level audio events.

## Change Rounds

| Round | Change | Live Count | Recorded Replay Count | Improvement | Learning |
| --- | --- | ---: | ---: | --- | --- |
| 1 | Initial native RX path, single timing lane | Unknown | Capture A: 7 accepted, Normal. Reverse: 0. | Baseline | The packet path worked, but fixed timing missed many valid bursts. |
| 2 | Added modem/window diagnostics, tone meters, receive gate status, bad-FCS summaries | Not a decode-count change | Not a decode-count change | Visibility | Logs showed many AX.25-looking candidates with bad FCS, which pointed toward symbol timing/bit errors rather than GUI display or polarity. |
| 3 | Mark/space calibration tone testing | Not a packet test | Mark and space tones detected over 10 second Dire Wolf calibration transmissions | Confidence in audio path | The audio tap and tone measurement path could see the expected tones. Packet misses were downstream of tone presence. |
| 4 | 5-lane HF timing phase bank `{1,17,33,49,65}` | Capture B live: 13 of 21 | Capture A: 20 accepted. Capture B: 13 accepted. | Big gain on Capture A, no gain on Capture B live | Multi-phase fixed timing helped a lot, but some packet bursts needed decision phases between the 16-sample spacing. |
| 5 | 10-lane HF timing phase bank `{1,9,17,25,33,41,49,57,65,73}` | Not live tested | Capture A: 20 accepted. Capture B: 15 of 21. | Capture B +2 over 5 lanes | Denser timing coverage recovered additional bursts without changing polarity or levels. |
| 6 | 20-lane bank, original alignment `{1,5,9,...,77}` | Not live tested | Capture A: 20 accepted. Capture B: 17 of 21. | Capture B +4 over 5 lanes | Remaining misses were still partly timing-phase sensitive. |
| 7 | Tiny HF Gardner PLL alpha `0.0005` with 20 lanes | Not live tested | Capture B: 8 of 21. | Regression | The simple PLL experiment destabilized this capture. Fixed timing plus phase diversity is better until we implement a packet-synchronous timing loop. |
| 8 | Preserve demodulator state when the receive gate opens, clearing only HDLC frame state | Not live tested yet | Capture B stayed 17 of 21 with the 20-lane bank under test | Startup behavior fix, not a replay-count gain | This matches the observed phenomenon where decodes appeared only after the 3rd or 4th transmission. Resetting the full demodulator at gate-open likely made early packets pay the filter/AGC/timing warmup cost. |
| 9 | 40-lane diagnostic scan `{1,3,5,...,79}` | Not live tested | Capture B: 18 of 21 | Capture B +1 over original 20-lane bank | More timing coverage can recover one more burst, but 40 lanes replayed slower than real time and is too heavy to ship as the normal live path. |
| 10 | Alternate 20-lane bank `{3,7,11,...,79}` | Not live tested | Capture A: 19 accepted. Capture B: 18 of 21. | Better Capture B, slight Capture A regression | The latest capture preferred the alternate 4-sample alignment, but Capture A needed phase 1 for one recovered burst. |
| 11 | Current evidence-based 21-lane bank `{1,3,7,11,...,79}` | Capture C live: 19 of about 21 full bursts | Capture A: 20 accepted. Capture B: 18 of 21. Capture C: 18 accepted from the saved file. | Best balanced result so far | Retains the latest-capture gain while restoring the older-capture result. The saved Capture C file ended before the last live decode, explaining the live/replay difference. |

## Current Decoder Behavior

Current HF 300 configuration:

```text
sample rate: 24000 Hz
baud:        300
mark:        1600 Hz
space:       1800 Hz
polarity:    Normal for these Dire Wolf A+ DIGU tests
lanes:       21 timing phase lanes
```

Reverse polarity produced no valid frames in the recorded replay tests. Keep Normal for the current Dire Wolf A+ / DIGU setup. Reverse remains useful for future USB/LSB tone-sense inversion cases.

The current build logs:

- decode lane count
- HDLC starts
- HDLC frame candidates
- AX.25-like candidates
- accepted frames
- rejects by too-short, bad-FCS, and malformed class
- last reject preview and FCS details
- per-frame decode phase offset

The packet activity graph uses AX.25-like candidate deltas rather than raw HDLC candidate deltas, because raw HDLC counts are lane-summed and noisy.

## VHF 1200 baud (Bell 202 / 2m APRS)

The `Vhf1200` profile decodes standard 2m FM APRS (e.g. 144.390 MHz in North
America), including via a transverter where the radio sits on an HF IF in FM.
Select it with the **1200 baud VHF** profile button in the AetherModem window.

```text
sample rate:       24000 Hz
baud:              1200
mark:              1200 Hz
space:             2200 Hz
samples/symbol:    20
polarity:          Normal for upright FM-demodulated AFSK
lanes:             18 (10 free-running phase + 4 tracked phases x 2 PLL alphas)
```

Unlike HF, 1200 baud uses a **hybrid decode bank** (tuned by `kVhf1200*` in
`src/core/tnc/AetherAx25LibmodemShim.cpp`) that favors aggressive capture:

- **Free-running phase lanes** (PLL alpha 0, spread across the 20-sample symbol)
  decode clean / short / strong bursts the instant they arrive — at least one
  lane samples the eye center, the same proven mechanism as the HF bank. This is
  what makes the synthetic and chunked 1200 loopback tests decode; a single
  Gardner-only lane (the original stub) never locked reliably.
- **Gardner-tracked lanes** (PLL alpha 0.010 and 0.025) chase the symbol clock
  for TX/RX drift and fading on longer or weaker bursts.

Duplicate suppression collapses the same frame seen by several lanes into one
emission, so the 18-lane bank still reports each packet once.

Polarity is **Normal** for upright FM-demodulated AFSK; use Reverse only as a
tone-sense check if a mode/sideband change kills all decodes. Both receive and
transmit are available on the 1200 profile (see the Experimental TX Path
section) — the AetherModem text field keys an AX.25 UI frame at whichever baud
profile is selected.

### Iterating on 1200 baud

- Enable the **AetherModem** (`aether.ax25`) log category. On every profile
  switch the modem logs a one-line config summary
  (`modem configured: 1200 baud VHF ... lanes=18`), and each decode logs
  `decoded AX.25 frame baud=1200 conf=... SRC=... payload=...`.
- Click the packet-activity graph to toggle the per-second diagnostics summary
  (tones, gate, symbols, confidence, HDLC/AX.25 candidates, FCS reject classes)
  — identical instrumentation to the HF path, now tagged with `baud=`.
- Replay a saved capture against every profile/polarity in one pass:
  `ax25_libmodem_shim_test --replay-capture <mono-float32.wav>`. Use the
  window's **Capture 3m** button to record a real 2m session first.
- If marginal bursts are missed, raise `kVhf1200FreeRunPhaseCount` for denser
  fixed-phase coverage, or adjust `kVhf1200PllAlphas` for the tracked lanes.

## Radio and Level Learnings

The successful captures were not close to clipping:

| Capture | Overall RMS | Peak | Clip |
| --- | ---: | ---: | ---: |
| Capture A | about -21.5 dBFS | about -10.1 dBFS | 0.00% |
| Capture B | about -21.7 dBFS | about -10.2 dBFS | 0.00% |
| Capture C | about -21.5 dBFS | about -9.9 dBFS | 0.00% |

For current testing, keep receive audio in this rough range:

- Avoid clipping. Peaks around -10 dBFS were fine.
- Do not chase every decode miss with AF gain once tones are comfortably visible.
- Normal polarity is correct for the tested Dire Wolf A+ / DIGU path.
- If decodes stop entirely after a sideband or mode change, try Reverse polarity as a tone-sense check.

## Experimental TX Path

AX.25 UI-frame transmit works on **both** the 300 baud HF and 1200 baud VHF
profiles. The transmit field keys whichever profile is currently selected, so
the same text field that sends an HF packet on 14 MHz sends a 2m APRS packet
(via a transverter) when 1200 baud VHF is active.

- AX.25 UI frames at the selected baud profile (HF 300 → 1600/1800 Hz;
  VHF 1200 → 1200/2200 Hz Bell 202).
- The transmit field accepts raw payload text or full `SRC>DST,path:payload`
  monitor syntax.
- Raw text defaults to `<radio callsign> > APRS` with no digipeater path.
- AetherModem generates 24 kHz stereo float AFSK, pads it to the VITA packet
  boundary, and paces it through the app-owned DAX TX stream.
- The window sets DAX TX routing, keys PTT with a short settle/lead time,
  feeds the generated audio in 20 ms chunks, then unkeys and restores the
  previous DAX state.
- **TXDELAY (preamble) is profile-aware.** HF 300 keeps its long preamble
  (`kTxPreambleFlags`, ~2.1 s); VHF 1200 uses a shorter one
  (`kVhf1200TxPreambleFlags` = 64 flags, ~0.43 s) since each flag is 4x quicker
  at 1200 baud. Raise the VHF value if a transverter's T/R switching clips the
  start of the burst.

TX diagnostics in the `aether.ax25` category include packet source/destination,
path, payload bytes, AX.25 frame bytes, bit count, waveform duration, RMS/peak,
baud, mark/space, polarity, preamble/postamble flag counts, DAX TX stream id,
PTT lead/tail timing, and paced chunk progress when debug is enabled.

## Open Work

The remaining missed packets are mostly AX.25-looking candidates that fail FCS. That means the decoder is often finding packet structure but still has symbol/bit errors before CRC.

Next work should focus on:

- packet-synchronous HDLC gating
- better timing recovery for 300 baud HF AFSK
- reducing dependence on many parallel fixed phase lanes
- using bad-FCS AX.25-like candidates to diagnose where bit errors cluster
- possibly adapting lane activation only while the receive gate is open, if CPU becomes a concern
- validating over-the-air AetherModem TX level, timing, and FCS decode with a
  second receiver

Out of scope remains KISS, APRS-IS, maps, digipeating, and connected-mode AX.25.
