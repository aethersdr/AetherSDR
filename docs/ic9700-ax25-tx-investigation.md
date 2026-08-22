# IC-9700 AX.25 Transmit over RS-BA1 — Signal Paths and Investigation Record

**Status:** Investigation record — two confirmed defects fixed/in-flight, one
radio-side anomaly still open, deciding experiment specified
**Author:** Nigel Fenton (G0JKN) with Claude, PTT-timing diagnosis by @jensenpat
**Date:** 2026-08-22 (consolidates bench work from 2026-08-12 onward)
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

**Era caveat on the last row:** the off-air captures (08-12) and the client
tap controls (08-20) are eight days and several Icom fixes apart. "The seam
displaced the tone" and "the client transmitted a wrong tone on 08-12 and has
since been fixed" cannot be separated from these files.

## 5. Findings

1. **PTT/audio synchronization defect — confirmed, distinct, real.**
   Optimistic keyed-state opens the audio gate before the radio keys; PTT-ON
   is paced while only PTT-OFF bypasses; the radio-confirmed keying signal
   exists and is unawaited; `kTxLeadMs` is a guess. Diagnosis @jensenpat,
   fix in progress by him. Predicts first-frame loss with later frames
   decodable — which is exactly why it **under-explains** this fault's 0/N.
2. **TxPacketizer silent overflow — confirmed, fixed as diagnosis.** #5162
   adds counters; the drop rule is unchanged. On the recorded bench keyings
   the counter did not advance, so production drops are not established as a
   cause here.
3. **Deviation pinning / spectral displacement — open, radio-side, and the
   leading candidate for THIS fault.** Level-invariance, full-length corrupt
   bursts, and a displaced space tone all point past the UDP datagrams into
   the RS-BA1 audio path. AetherSDR's last observable point (the wire tap)
   is clean in CONTENT — with the honest limit that the tap's frame-count
   residual (#5060) is unexplained, so "clean" here rests on what decodes,
   per Finding 4's own rule about instruments.
4. The wire-tap defects cost a week of chasing a corruption that was in the
   instrument, and #5060's frame-count residual remains open. Recorded so
   the next investigator distrusts instruments first — including these.

## 6. The deciding experiment

One keying on the bench, after @jensenpat's timing fix lands, captured at
three points **simultaneously**:

1. post-resample tap (client, last point AE controls),
2. wire tap (datagram payload),
3. RTL-SDR IQ off-air (independent, no receiver audio path).

Same day, same build, same burst. If (1)(2) decode and show 1200/2200 while
(3) shows a displaced tone or fails to decode: the fault is radio-side in the
RS-BA1 audio path, and #5011 gets retitled to say exactly that. If all three
decode: the timing fix was the whole story for this path too, and #5011
closes. Either way the taps (#5058) are the instrument, re-scoped from
"find the audio fault" to "prove where the boundary is".
