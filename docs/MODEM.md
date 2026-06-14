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
demodulator:       Direwolf-derived Profile A+ (9 amplitude slicers)
```

Unlike the HF 300-baud path (which uses libmodem's timing-phase bank described
above), VHF 1200 baud uses a **Direwolf-derived AFSK demodulator** —
`AetherAFSKDemod` in `third_party/direwolf_afsk/`, a C++20 rewrite of Dire
Wolf's profile-A `demod_afsk.c` (GPL-2.0-or-later). It runs the **A+**
configuration: a single IQ-mix → RRC-lowpass → envelope/AGC front end feeding
**9 amplitude slicers** (`kVhf1200SpaceGains` in
`src/core/tnc/AetherAx25LibmodemShim.cpp` — the exact Dire Wolf A+ space-gain
series), each with its own DPLL chasing the symbol clock. This replaced the
older libmodem free-running / Gardner-tracked lane bank on the VHF path (the HF
300 path is unchanged). On a 1,875-packet overnight live run it out-copied both
Dire Wolf and Graywolf (#3527).

Duplicate suppression collapses the same frame seen by several slicers into one
emission, so the 9-slicer bank still reports each packet once.

Polarity is **Normal** for upright FM-demodulated AFSK; use Reverse only as a
tone-sense check if a mode/sideband change kills all decodes. Both receive and
transmit are available on the 1200 profile (see the Experimental TX Path
section) — the AetherModem text field keys an AX.25 UI frame at whichever baud
profile is selected.

### Iterating on 1200 baud

- Enable the **AetherModem** (`aether.ax25`) log category. On every profile
  switch the modem logs a one-line config summary
  (`modem configured: 1200 baud VHF ... lanes=9`), and each decode logs
  `decoded AX.25 frame baud=1200 conf=... SRC=... payload=...`.
- Click the packet-activity graph to toggle the per-second diagnostics summary
  (tones, gate, symbols, confidence, HDLC/AX.25 candidates, FCS reject classes)
  — identical instrumentation to the HF path, now tagged with `baud=`.
- Replay a saved capture against every profile/polarity in one pass:
  `ax25_libmodem_shim_test --replay-capture <mono-float32.wav>`. Use the
  window's **Capture 3m** button to record a real 2m session first.
- The Profile A+ slicer set is the fixed Dire Wolf `kVhf1200SpaceGains` series;
  the vendored demodulator and its parameters are documented in
  `third_party/direwolf_afsk/` (`AETHERSDR-PATCHES.md` + the source headers).

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

## KISS TNC (TCP)

The **KISS TNC** tab next to AX.25 turns AetherModem into a KISS-over-TCP TNC so
any host packet/APRS application drives the modem:

- **Enable TNC** starts/stops a `QTcpServer` (cross-platform) on the configured
  **TCP port** (default **8001**, all interfaces). **Start TNC on Startup**
  persists the choice; when set, the app constructs the AetherModem window
  hidden at launch so the server runs headless and survives the window closing.
- Multiple clients are supported. Each gets an independent, resync-safe KISS
  decoder. Dead/stuck clients are reaped via TCP keepalive, a slow-consumer
  write-backlog cap, and an idle sweep; a client cap bounds resource use.
- **RX → clients:** every decoded frame is forwarded to all clients as a KISS
  data frame. The exact on-air bytes (address..info, no FCS) are captured at
  decode (`Ax25DecodedFrame::ax25FrameNoFcs`) so hosts get a byte-faithful copy.
- **clients → TX:** a client's KISS data frame is queued and keyed onto the air
  with the baud profile selected on the AX.25 tab. The modem computes/->appends
  the FCS (`buildTransmitAudioFromFrame`). Queued frames serialize through the
  one-at-a-time TX path; the queue drains as each transmit completes.
- Enabling the TNC also enables the modem (RX tap) for you; a slice must be
  attached and the radio ready for traffic to flow.

KISS framing lives in `src/core/tnc/KissFraming.{h,cpp}` (pure, unit-tested);
the server is `src/core/tnc/KissTncServer.{h,cpp}`. All lifecycle and per-frame
activity logs on the `aether.ax25` (AetherModem) category prefixed `KISS`, so a
problem can be triaged as client-side (connect/parse/backlog) vs RF-side
(decode/level/gate). The TNC STATUS panel shows listening port, client count,
and RX/TX frame counters.

## Personal Mailbox System (PMS)

The **Mailbox** tab turns AetherModem into a compact, Kantronics-KPC-3-style
Personal Mailbox System (PBBS). A single remote caller can connect over
**1200-baud AX.25 connected mode** and read, list, and send messages, see who
has been heard, then disconnect.

**Connected-mode data link.** Unlike the KISS/UI paths (connectionless), the PMS
needs AX.25 v2.0 connected mode (LAPB, mod-8). Two reusable, RF-agnostic,
unit-tested layers provide it:

- `src/core/tnc/Ax25.{h,cpp}` — frame primitives: callsign/SSID `Address`
  encode/decode and `Frame` parse/build for I, RR/RNR/REJ, SABM, DISC, DM, UA,
  FRMR, and UI frames (address..info, no FCS — the same convention as
  `buildTransmitAudioFromFrame`).
- `src/core/tnc/Ax25Connection.{h,cpp}` — a single-connection state machine:
  accepts an inbound SABM (→ UA), tracks V(S)/V(R)/V(A), acknowledges with RR,
  segments outbound data into I-frames (≤ paclen), and retransmits unacked
  I-frames on the T1 timeout up to N2 tries before declaring link failure.
  Defaults (T1 6 s, N2 8, paclen 128, window 4) are sized for 1200-baud FM with
  PTT overhead, so a lossy link recovers via REJ/T1 rather than dropping.

**The mailbox service** is `src/core/pms/PmsMailbox.{h,cpp}` (one file pair). It
owns the `Ax25Connection`, greets a caller by callsign with the AetherMailbox SID
and version, runs a line-oriented command interpreter, and persists state as JSON
under `~/.config/AetherSDR/pms/` (`messages.json`, `callers.json`,
`heard.json`). Decoded frames are fed in via `onAirFrame()`; everything it emits
on `transmitFrame()` is keyed through the existing one-at-a-time TX queue. The
**heard list** is updated for *all* received frames (not just mailbox traffic) so
callers can discover other PMS/BBS stations nearby, and an optional **hourly UI
beacon** announces the mailbox is online and how to connect.

**Commands** (Kantronics subset; first letter or full word):
`H(elp)`, `B(ye)`, `I(nfo)`, `J(heard)`, `L(ist)`, `LM` (list mine),
`R(ead) n`, `K(ill) n`, `S(end)`/`SP call`, `SB cat`, `U(sers)`. A message is
entered after `SUBJECT:` and terminated with `/EX` or Ctrl-Z on its own line.
Use `ALL` as the recipient for a public message.

The **Mailbox** config tab exposes Enable PMS, the **listen callsign** the
mailbox answers on (full `CALL-SSID`, e.g. `KI6BCJ-10`), an optional **vanity
alias** it also answers on (e.g. `AETBBS` — AX.25 limits a callsign to 6
characters plus an optional `-SSID`), a welcome/PTEXT line, the
hourly-beacon toggle and text, plus a stats row with **Statistics** on the left
and the **last callers** on the right. When a caller dials the alias, the whole
session (UA, greeting, every reply) uses the alias address. All settings persist
in `AppSettings` (`AetherModemPms*` keys) across restarts; enabling the PMS turns
the modem on. The bottom of the window is a slim status bar showing modem state,
gain, and a compact packet-activity strip.

These layers are intentionally split so the planned APRS/AX.25 **digipeater** can
reuse `Ax25`/`Ax25Connection` and the heard list directly.

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

KISS-over-TCP, connected-mode AX.25, a Personal Mailbox System, and an **APRS
client** (live station map, GPS beacon, two-way messaging on the AetherModem
tab) are now implemented — see the sections above and the PSK Reporter map's
shared Qt mapping engine, which the APRS map reuses. Out of scope remains
**APRS-IS** (internet gateway) and a future **APRS/AX.25 digipeater**, which can
reuse the `Ax25`/`Ax25Connection` primitives.

PMS follow-ups worth tracking:

- On-air validation of connected-mode RX/TX at 1200 baud with a second TNC
  (the protocol layer is unit-tested; RF round-trip needs a real radio).
- Multi-connect (the current PMS answers one caller at a time, by design).
- A local operator terminal tab to use/test the mailbox without a radio.
