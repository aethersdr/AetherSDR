# IC-9700 AX.25 Transmit over RS-BA1 — Signal Paths and Investigation Record

**Status:** Investigation record — timing defect fixed and re-tested (does
NOT resolve the fault); TWO open leads now (radio-side deviation/spectral,
AND a new client-side post-resample-vs-wire timing/framing gap); deciding
experiment 2/3 legs run, not yet simultaneous
**Author:** Nigel Fenton (G0JKN) with Claude, PTT-timing diagnosis by @jensenpat
**Date:** 2026-08-22, updated 2026-08-24 (consolidates bench work from
2026-08-12 onward)
**Scope:** Why AX.25 transmitted through AetherSDR to an IC-9700 over RS-BA1
LAN audio keys the radio, sounds like packet on a receiver, and decodes as
nothing — with every signal path involved described end to end, every
measurement recorded, and what each one rules out.

Related: #5011 (the fault), #5058 (transmit-audio taps), #5060 (wire-tap
over-record), #5162 (packetiser drop counters), PR #4994 (DFM/DATA flag),
PR #5006 (CI-V scheduler).

---

## 1. The fault

An AX.25 connect burst sent from the AX.25 dialog through an IC-9700 over the
RS-BA1 LAN transport:

- keys the transmitter, full-length, every time;
- sounds like packet on any nearby receiver;
- decodes as **zero frames** — off-air, at any receive level, on multiple
  independent receive chains;
- while the **same radio decodes and transmits packet perfectly through its
  USB sound interface driven by Direwolf directly** (no AetherSDR involved),
  which proves the radio, RF path, and antenna are all capable. The failing
  ingredient is the client-to-radio path.

## 2. Signal paths

### 2.1 The transmit AUDIO path

```
Ax25HfPacketDecodeDialog          modulator: libmodem shim, 1200 Bd AFSK
        │  float mono 24 kHz, 20 ms chunks (kTxChunkMs), paced by wall clock
        ▼
AudioEngine tap → RadioModel::submitTxAudio           [txwave records HERE]
        │  int16 stereo interleave, engine rate
        ▼
IcomCivBackend::submitTxAudio
        │  GATE: `if (!m_keyed && !m_tuning) return;`   ← opens on OPTIMISTIC
        │  keyed state, not radio-confirmed (see §2.2)
        │  downmix stereo→mono
        ▼
Resampler 24k → 48k  (m_txResampler)      [post-resample tap emits HERE — #5058]
        ▼
TxPacketizer (IcomAudio.cpp)
        │  cap kMaxPendingBytes = 24000 bytes = 250 ms @ 48 kHz s16 mono
        │  overflow drops the OLDEST bytes — correct for voice, destructive
        │  for a burst whose oldest bytes are preamble + opening flag.
        │  Silent until #5162 added drop counters.
        ▼
IcomSession::sendAudio → LPCM 16-bit mono 48 kHz → UDP datagrams
        │                                          [wire tap decodes datagrams
        │                                           back to float — #5058]
        ▼
RS-BA1 LAN transport → radio's network-audio modulation input
        ▼
IC-9700 internal processing: its own MOD level, its own deviation control
        ▼
RF
```

Stage-by-stage integrity has been measured (§4). Everything down to the UDP
datagrams is byte-clean and decodes; everything past the RS-BA1 seam is the
radio's own audio processing, which AetherSDR can neither observe nor tap.

On unkey, `setKeying(false)` calls `IcomSession::flushTxAudio()` — queued
audio belongs to the transmission that ended. Correct in intent, but it means
**an unkey that races the packetiser drain discards the burst tail**. The
AX.25 dialog waits `m_txTailMs` (default 150 ms; the recorded bench sessions
ran it at 200) after the last chunk, which bounds but does not eliminate the
race when pacing has fallen behind.

### 2.2 The transmit KEYING path — and why it is not synchronized with audio

```
Ax25HfPacketDecodeDialog
        │  after kTxDaxSettleMs = 150 ms
        ▼
TransmitModel::requestPttOn → setMox(true)
        │  "Optimistic MOX edge gating" — m_transmitting set TRUE
        │  synchronously, BEFORE any command is emitted. The dialog's
        │  `isTransmitting()` guard therefore cannot fail for timing reasons.
        ▼
commandReady("xmit 1") → command plane → IcomCivBackend::setKeying
        │  ENQUEUES the CI-V PTT frame (below), then sets m_keyed=true and
        │  emits mox=true in the same synchronous call — this opens the §2.1
        │  audio gate before the frame has been dispatched, let alone answered:
        ▼
IcomCivScheduler  (PR #5006)
        │  one dispatch slot per kSlotMs = 25 ms
        │  one reply-bearing transaction in flight; a queued write waits for
        │    an outstanding read to retire — up to kReadTimeoutMs = 350 ms
        │    on a lost reply (queueWrite sets expectsReply = true)
        │  PTT-OFF is Priority::Emergency and bypasses all pacing (correct:
        │    an unkey must never queue). PTT-ON is Priority::Operator and is
        │    fully paced — THE KEYING EDGE IS THE ONE THAT WAITS. NB for
        │    anyone reaching for "just promote PTT-ON": sendUserCommand
        │    derives Emergency from the frame's payload byte (data.front()
        │    == 0), so Emergency is structurally reserved for unkey — the
        │    fix shape lives at the audio/keying synchronization, not here.
        ▼
CI-V transport (serial or RS-BA1 LAN) → radio keys
        ▼
onCivFrame: reconciles m_pendingPttIntent against a real 1C 00 readback
             ← radio-confirmed keying EXISTS here, and NOTHING WAITS FOR IT
```

The dialog starts audio a fixed `kTxLeadMs = 200 ms` after requesting PTT.
That number is a guess against an unbounded queue delay, not a measured
margin. When the queue is busy, audio is admitted (gate already open) and
paced out before the radio is keyed — the front of the burst transmits into
an unkeyed radio and is lost. This is the truncation @jensenpat diagnosed on
2026-08-22 (Icom VHF, Daybreak model): *"it's not related to audio stack at
all, it's a timer issue because CI-V is serial (and slow)."* Confirmed in
code at every step above.

### 2.3 Reference and witness paths

| Path | Role | Status |
|---|---|---|
| Direwolf → IC-9700 USB audio + RTS PTT | known-good control: same radio, no AE | working TNC, decodes both ways |
| AE → AIOC → Baofeng VOX → Direwolf | independent TX path for A/B | built, pending the A/B session |
| FT-847 + SignaLink, FM demod → WAV | off-air witness for 2026-08-12..16 captures | valid, with the caveat that receiver audio filtering can hide sub-20 ms dropouts |
| RTL-SDR (Pi 5) IQ capture → offline demod | off-air witness, bypasses any receiver audio path | validated 2026-08-22: decoded CW and FT8 from the same station perfectly |
| `txwave save` → `atest -B 1200` | standing arbiter for any client-side audio question | in routine use |

## 3. Instruments

- **`txwave`** — records `submitTxAudio` input samples (not levels; levels
  cannot show shape).
- **Post-resample tap** (#5058) — the exact mono float handed to
  `IcomSession::sendAudio`; a WAV from it feeds `atest` directly.
- **Wire tap** (#5058) — decodes the audio payload of each outbound UDP
  datagram back to float. NB: two of the tap's defects (double-recording
  retransmits, keyed-flag truncation) are fixed on the #5058 branch, and its
  first output produced a false "wire is corrupt" reading, recorded as #5060.
  **#5060 is still open**: after both fixes the capture still holds ~52 frames
  of audio more than was sent, cause unestablished. Frame-COUNT claims from
  this tap are therefore not trustworthy yet; frame-CONTENT claims (what
  decodes, at which tones) are, because a decoder validates them
  independently.
- **TxPacketizer drop counters** (#5162) — cumulative dropped bytes and
  overflow events; before them a transmission that lost 63% of itself was
  indistinguishable from one that lost nothing.
- **`icom_tx_resample_ax25_test`** (#5058 branch) — resampler, LPCM codec and
  packetiser exercised standalone with 1200 Bd AFSK; no radio, no sockets.
- Bridge polling note: `get("transmit")` carries both `mox` (the MOX latch
  only — stays false through DAX/TCI/CWX-keyed transmissions) and
  `transmitting` (interlock truth). Poll `transmitting`.

## 4. Results ledger

| Date | Experiment | Result | Rules out / establishes |
|---|---|---|---|
| 08-12/13 | Off-air captures (FT-847 witness), connect bursts | 0/N decode, every burst | the fault is real and repeatable off-air |
| 08-15 | `submitTxAudio` capture → `atest` | 3/3 and 2/2 frames, mark/space clean | client entry point is clean; **stop re-testing it** |
| 08-16 | Amplitude sweep of a known-good capture: 1.00→0.010 (−40 dB) | 2 packets at every level | level/amplitude is not the mechanism |
| 08-17 | Radio-side MOD level 8% vs 80%, client +8.2 dB | identical on-air deviation | deviation is PINNED radio-side; RS-BA1 audio-path anomaly (#5011, open) |
| 08-18 | Wire tap first output | ~2.5× audio over-recorded | the TAP was the liar, not the wire (#5060); tap fixed |
| 08-20 | Post-resample + wire taps (defects partially fixed, #5060 residual open) → `atest` | frames decode clean at both points, at the right tones | resampler, LPCM encode and datagram CONTENT clean; datagram COUNT still suspect (#5060) |
| 08-20 | `icom_tx_resample_ax25_test` | ratio 1.0000, LPCM worst error < 1 quantum, oversized submit drops 40960/64960 bytes silently (pre-#5162) | packetiser overflow is reachable and was invisible |
| 08-22 | @jensenpat: PTT timing truncates preamble (Icom VHF) | confirmed in code (§2.2), every step | a real defect; the fix belongs at the keying/audio synchronization |
| 08-22 | Off-air burst structural analysis (08-12 captures) | bursts full-length (0.60–0.67 s ×3 retries), modulation present start to end | **front-truncation is NOT what these captures show** — PTT timing does not explain this fault instance |
| 08-22 | Spectral comparison, same analysis on control (bench artifacts: `ax25bench-wire.wav` / `ax25bench-post.wav` 08-20 controls; `ax25-connect-burst.wav` 08-12 off-air; 64k-FFT band-peak method, reproducible from the WAVs) | client wire tap (08-20): peaks 1176 / 2224 Hz — mark/space where they belong. Off-air (08-12): 1175 / **1775** Hz — mark in place, space displaced ~425 Hz | corruption is SPECTRAL, not temporal; consistent with radio-side audio-path anomaly; level-invariant, audible, undecodable |
| 08-24 | Live re-test with @jensenpat's PTT/CI-V timing fix in place (radio-confirmed keying now awaited, §2.2) | **decode still fails — no improvement over pre-fix behavior** | **the timing fix is EXCLUDED as the cause of this fault instance.** Confirms the 08-22 spectral finding rather than merely being under-explained by it; deviation-pinning / RS-BA1 audio-path anomaly is the sole remaining leading candidate |
| 08-24 | First half of the deciding experiment (§6): #5058 branch, AetherModem Terminal Connect + APRS Msg send, dual tap armed, live on the bench | **0 samples recorded, 3 separate real-RF trigger attempts** (2× Connect, 1× APRS Msg — all confirmed keyed via the gate log) | **red herring, root-caused, not a client-audio finding:** `RadioModel::setTxPostResampleTapEnabled`/`setTxWireTapEnabled` only arm when `qobject_cast<IcomCivBackend*>` succeeds (RadioModel.cpp ~8232-8262). AE was connected via **Aether-gate**, which presents as a `FLEX-6700` — the active backend was `FlexBackend`, so the cast failed and both taps silently no-op'd regardless of trigger. **Lesson for future taps/txwave use: confirm `get radio` reports the native Icom model/serial (`icom:<ip>`), not a gate's Flex costume, before trusting a "nothing recorded" result as a client-audio finding.** |
| 08-24 | Reconnected AE directly to the 9700 (Connect by IP, native `IcomCivBackend`, gate stopped to free the RS-BA1 session) — dual tap re-armed, one AX.25 Connect burst, drive 4% | **post-resample: 2/2 decode** (`G0JKN>KB2SKP-12:`, clean). **wire: 0/2, fails to decode.** Peak amplitude IDENTICAL between the two taps (12895/32768, 39.4%, exact match). Mark/space tones near-identical too (1176/2225 Hz post-resample vs 1174/2224 Hz wire) — both in the CORRECT band, not displaced like the 08-22 off-air spectral finding. | **A genuinely new localization, distinct from the radio-side hypothesis**: the corruption sits between the post-resample tap and the wire tap — i.e. inside AetherSDR's own LPCM-encode/packetizer/socket-write stage, upstream of anything the radio touches. Level and tone content are provably NOT the mechanism this time (both taps agree on both); something in TIMING/FRAMING between those two points is the new leading suspect. This is the first client-side leg of the §6 deciding experiment to actually run — the RTL off-air leg (same evening, different connection) is in the ledger's next row. A single simultaneous three-point capture (client can't hold the gate AND a native connection at once with current tooling) is still outstanding. |
| 08-24 | RTL-SDR V4 off-air, 2 separate 20 s captures via Aether-gate Connect-button bursts (before the backend-mismatch root cause was found; drive 4%, antenna repositioned after saturation tuning at 40%/5%/3%/1%) | **Clean, unclipped, well-shaped bursts both times** — capture 1: 3 bursts ~1 s each at 195-211× floor; capture 2: 4 bursts matching the gate's key log almost exactly. No decode attempted yet (not demodulated to audio). | Off-air RF is present, correctly timed to the keying log, and NOT saturated/garbled at the RF envelope level — consistent with (but does not by itself prove) the corruption being downstream of the radio's actual transmission, matching the new packetizer/socket lead above. Raw IQ saved: `ax25-triple-capture-2026-08-24/real_capture.iq` + `real_capture2.iq`, `native_post_resample.wav` + `native_wire.wav`. |

**Era caveat on the 08-22 spectral row:** the off-air captures (08-12) and the
client tap controls (08-20) are eight days and several Icom fixes apart. "The
seam displaced the tone" and "the client transmitted a wrong tone on 08-12
and has since been fixed" cannot be separated from these files. The 08-24
row is a live re-test on current code and does not carry this caveat.

## 5. Findings

1. **PTT/audio synchronization defect — confirmed, distinct, real, and now
   EXCLUDED as the cause of this fault instance.**
   Optimistic keyed-state opens the audio gate before the radio keys; PTT-ON
   is paced while only PTT-OFF bypasses; the radio-confirmed keying signal
   exists and is unawaited; `kTxLeadMs` is a guess. Diagnosis @jensenpat,
   fix landed; predicts first-frame loss with later frames decodable. Live
   re-test 08-24 with the fix in place: **decode still fails**, so this
   defect — real as it is — is not what blocks decode here. The fix is
   presumably still worth keeping for the synchronization gap it closes on
   its own terms.
2. **TxPacketizer silent overflow — confirmed, fixed as diagnosis.** #5162
   adds counters; the drop rule is unchanged. On the recorded bench keyings
   the counter did not advance, so production drops are not established as a
   cause here.
3. **Deviation pinning / spectral displacement — open, radio-side, and A
   leading candidate for THIS fault, but no longer sole.** Level-invariance,
   full-length corrupt bursts, and a displaced space tone all point past the
   UDP datagrams into the RS-BA1 audio path. **SUPERSEDED CLAIM, corrected
   08-24:** this finding previously said the wire tap is "clean in CONTENT".
   It is not, on a fresh 08-24 run: post-resample decodes 2/2, the wire tap
   from the SAME transmission decodes 0/2, with peak amplitude and mark/space
   tone frequency essentially IDENTICAL between the two. So content is not
   trivially clean at the wire either — see Finding 5.
5. **NEW 08-24 — a client-side timing/framing lead, distinct from Finding 3.**
   Post-resample audio decodes; the wire-tap capture of the SAME burst does
   not, despite matching level (12895/32768 both) and matching tone
   frequency (within 1-2 Hz). Ruling out amplitude and tone content as the
   wire-tap mechanism points at something in TIMING or FRAMING between the
   post-resample point and the socket write — the LPCM encode, the
   packetizer, or the datagram framing itself. This does not contradict
   Finding 3's radio-side evidence (different bursts, different sessions,
   both could be real); it means the fault may have TWO distinct
   contributors, or the wire tap itself may still be lying in a way #5060
   never fully closed (see Finding 6). Needs the RTL-off-air leg captured
   from THIS SAME burst to disambiguate — not yet done (§6).
6. The wire-tap defects cost a week of chasing a corruption that was in the
   instrument, and #5060's frame-count residual remains open. Recorded so
   the next investigator distrusts instruments first — including these. The
   08-24 wire-tap failure (Finding 5) is exactly the kind of result #5060
   warns about: it MUST be re-examined for an instrument-side explanation
   before being read as a new client-side defect.
7. **NEW 08-24 — tooling gotcha, not a physics finding, but costly if
   repeated.** `RadioModel::setTxPostResampleTapEnabled`/
   `setTxWireTapEnabled` only arm behind
   `qobject_cast<icom::IcomCivBackend*>(m_backend.get())` (RadioModel.cpp
   ~8232-8262). Connected via Aether-gate (which presents as a `FLEX-6700`
   to AE), the cast fails and both taps silently no-op — three separate
   real-RF trigger attempts (2× Terminal Connect, 1× APRS Msg send, all
   confirmed keyed via the gate's own log) produced zero recorded samples
   with no error. Always confirm `get radio` reports the native Icom
   model/serial (`icom:<ip>`), not a gate connection, before trusting a
   `txwave save` "nothing recorded" response as a client-audio finding.

## 6. The deciding experiment

**Precondition met 2026-08-24:** @jensenpat's timing fix has landed and been
re-tested live (§4, 08-24 row) — decode still fails with it in place, so the
timing variable is now settled rather than open. The experiment below no
longer needs to distinguish "timing fix helps" from "timing fix doesn't
help"; it is now purely about localizing the remaining fault to the client
or the radio.

One keying on the bench, captured at three points **simultaneously**:

1. post-resample tap (client, last point AE controls),
2. wire tap (datagram payload),
3. RTL-SDR IQ off-air (independent, no receiver audio path).

Same day, same build, same burst. If (1)(2) decode and show 1200/2200 while
(3) shows a displaced tone or fails to decode: the fault is confirmed
radio-side in the RS-BA1 audio path, and #5011 gets retitled to say exactly
that. If (1) or (2) also fails to decode: the fault is client-side after
all, upstream of where prior taps looked.

**Partial run, 2026-08-24 — two legs done, NOT simultaneous, inconclusive
by design.** Leg (1)+(2) ran together from one burst (client-side, native
`IcomCivBackend` connection): post-resample 2/2, wire 0/2 (Finding 5). Leg
(3) ran twice, cleanly, from *different* bursts on a *different* connection
(Aether-gate, before the backend-mismatch tooling gap in Finding 7 was
found) — two off-air captures, both showing clean unclipped multi-burst
RF matched to the gate's key log, not yet demodulated/decoded.

**Why not simultaneous, and what it would take:** Aether-gate holds the
single permitted RS-BA1 session while it runs; the client-side taps only
arm behind a native `IcomCivBackend` connection (Finding 7), which needs
the gate stopped to free that session. With current tooling the two legs
are mutually exclusive on one AE instance — the gate can feed an
independent RTL-SDR-style witness while native AE runs the taps only if
the RTL capture is driven from something OTHER than the gate's own
RS-BA1 session (it already is — the RTL listens over the air, not via
RS-BA1 — so the actual blocker was sequencing, not architecture: the gate
was stopped for the native-connection taps run, so no off-air witness
was live during THAT specific burst). **Still not run as a single
matched burst.** Next attempt: keep the gate stopped, AE on the native
connection with taps armed, RTL-SDR recording throughout (independent of
the gate — it only needs the frequency, not the gate's session) — one
Connect click keys all three simultaneously.

Either way the taps (#5058) are the instrument, re-scoped from "find the
audio fault" to "prove where the boundary is" — and Finding 5 has already
moved that boundary closer to the client than Finding 3 alone suggested.
