# HF 300-Baud Modem — Reliability Research and Roadmap

Companion to [`MODEM.md`](MODEM.md). `MODEM.md` is the *decoder* logbook — the
capture-replay experiment history that produced the current 21-lane HF demod.
This file covers everything above and around it: the connected-mode data link,
channel access, and the sensitivity work that is still open.

Scope is the **300 baud HF AFSK profile** (1600/1800 Hz, 24 kHz sample rate) as
used by AetherModem's Terminal tab and the Personal Mailbox System. The VHF
1200 profile shares almost all of this code and is called out where it differs.

---

## 1. Why HF is not "VHF but slower"

Every timing constant in the connected-mode stack was originally sized for
1200-baud VHF FM. At 300 baud the same numbers are not merely conservative,
they are *physically impossible*. The airtime arithmetic:

| Component | Bits | Seconds @ 300 baud |
| --- | ---: | ---: |
| TX preamble, 80 flags (`kTxPreambleFlags`) | 640 | 2.13 |
| I-frame, paclen 128 (146 bytes on air) | 1191 | 3.97 |
| TX postamble, 8 flags | 64 | 0.21 |
| PTT lead + DAX settle + TX tail | — | 0.50 |
| **Our own keyed time for one I-frame** | | **6.81** |

The shipped T1 was **6000 ms**, started at the moment the frame was queued.
T1 therefore expired roughly 0.8 s *before we stopped transmitting* — before
the peer could have heard the frame, let alone answered it. Every I-frame
timed out, retransmitted, and the link died after N2 = 8 tries and about a
minute of continuous keying.

This is the root cause of "HF connected mode doesn't work". It is arithmetic,
not tuning: no amount of decoder sensitivity work could have fixed it.

**The general rule this file exists to enforce:** any constant expressed in
milliseconds that interacts with the air interface must be *derived from the
baud rate and the framing*, never hardcoded. See §3.

---

## 2. What the link layer looked like before this work

Findings from the review that motivated the current change set. Items marked
✅ are addressed on this branch; items marked ⏳ are on the roadmap in §6.

### Link timing
- ✅ **T1 shorter than one transmission** (above). Fixed by the airtime model.
- ✅ **paclen 128 on HF** — a 4.0 s frame on a QSB-prone path is a coin flip,
  and AX.25 has no partial-frame recovery: one hit destroys the whole frame.
  HF now defaults to 64.
- ⏳ **T1 is static.** AX.25 2.2's adaptive T1 (smoothed round-trip → T1V)
  would let the link find its own timing instead of trusting a model.

### Link liveness
- ✅ **No T3.** With no I-frames outstanding, T1 is stopped, so a link whose
  peer has vanished stays `Connected` forever. The terminal displays a false
  "Connected to X" indefinitely; the single-caller mailbox is locked out
  permanently, because a SABM from any other station is answered with DM.
- ✅ **Unbounded REJ recovery.** The REJ handler reset the retry counter on
  every REJ, so a peer stuck REJ-ing never drove us to N2 — we would
  retransmit forever.
- ✅ **Retry counter reset without progress.** `ackUpTo()` cleared the retry
  counter even when N(R) acknowledged nothing new, so a peer repeating a
  stale RR could hold the link in a permanent retransmit loop. The counter now
  clears only on real progress (V(A) advanced) or on a genuine F=1 response to
  our poll — AX.25 2.2's rule, and Karn-safe.
- ✅ **SABME silently ignored.** Only SABM (0x2F) was decoded; SABME (0x6F)
  fell through to `Unknown` and got no reply at all. Peers that try AX.25 v2.2
  first — Direwolf among them — burn their entire retry budget before falling
  back to v2.0. We are mod-8 only, so the correct answer is a prompt DM, which
  makes the peer fall back immediately.

### Mailbox
- ✅ **No session inactivity timeout** — see T3 above; the mailbox needs its
  own timeout as well, because a caller can hold a *live* link open forever
  without ever sending a command.
- ✅ **Unbounded inbound line buffer** — a peer that never sends CR grew
  `m_lineBuffer` without limit.

### Channel access
- ⏳ **No DCD / CSMA.** TX is gated only on *our own* radio transmitting.
  Nothing checks whether the channel is busy, so we key on top of a peer
  mid-burst — and because we then go deaf, we miss the very frame we are
  waiting for. This is the failure the REJ and T2 half-duplex comments in
  `Ax25Connection` fight downstream instead of upstream. The ingredients
  already exist (`receiveGateOpen`, `hdlcCodec.inFrame()`); what is missing is
  a fast accessor (diagnostics only publish once per second) and a randomized
  slot backoff.

### Instrumentation
- ✅ **No round-trip measurement.** There was no way to tell whether T1 was
  too short, too long, or right — the single most important number for tuning
  an HF link, and it was not recorded anywhere.

---

## 3. The airtime model

`src/core/tnc/Ax25LinkTiming.h` is now the single source of truth for "how
long does this take on the air". It is pure arithmetic, no Qt, unit-tested.

```
onAirBytes = 14 (addresses) + 7·digiHops + 1 (control)
           + (info ? 1 (PID) + infoBytes : 0) + 2 (FCS)
frameBits  = onAirBytes · 8 · 1.02        // bit-stuffing allowance
           + (preambleFlags + postambleFlags) · 8
airtimeMs  = frameBits · 1000 / baud
```

T1 must cover a full round trip: our frame out, the peer's turnaround, its
supervisory reply back, plus a margin.

```
ourFrameMs  = airtime(paclen)   + localTxOverheadMs
peerReplyMs = airtime(no info)  + peerTurnaroundMs
T1          = 1.5 · (ourFrameMs + peerReplyMs)
```

Worked values, with the preamble taken from the profile's actual
`kTxPreambleFlags` so the two can never drift apart:

| Profile | paclen | Our frame | Peer reply | Round trip | **T1** |
| --- | ---: | ---: | ---: | ---: | ---: |
| HF 300 | 64 | 5.08 s | 3.31 s | 8.39 s | **12.6 s** |
| HF 300 | 128 | 6.82 s | 3.31 s | 10.1 s | **15.2 s** |
| VHF 1200 | 128 | 1.97 s | 1.10 s | 3.07 s | **4.6 s** |

The VHF row is the model's validation: it reproduces, from first principles,
the empirically-tuned 6 s that has worked on 2 m for a year. That is the
reason to trust the 12.6 s it produces for HF.

**T3** (idle-link poll) is derived from T1 as `10 · T1`, floored at 60 s and
capped at 300 s — long enough not to be chatty on a shared channel, short
enough to notice a dead peer within a couple of minutes.

---

## 4. Instrumentation for the HF soak

New logging category **`aether.ax25.link`** (`lcAx25Link`), Info by default so
it lands in a support log captured without foreknowledge — same reasoning as
`aether.hl2`.

What it records:

- **On connect** — the whole timing contract in one line: baud, preamble
  flags, paclen, configured T1/T2/T3, modelled I-frame airtime, and modelled
  round trip. If the link then misbehaves, this line says whether the settings
  were even plausible.
- **Per-ack round-trip samples** — measured from first transmission of an
  I-frame to the ack that covers it. Retransmitted frames are excluded
  (Karn's algorithm) because their RTT is ambiguous.
- **On T1 expiry** — configured T1 against observed RTT, with an explicit
  `T1_TOO_SHORT` marker when the average measured round trip exceeds T1. That
  marker is the single grep that answers "is the timing wrong, or is the
  channel bad?"
- **On disconnect** — a session summary: duration, payload bytes each way,
  effective throughput, retransmit ratio, and RTT min/avg/max.

The soak this was built for: PMS answering on the FlexRadio, Terminal calling
from the Hermes-Lite 2, both running at once so each side of the link is
independently instrumented.

---

## 5. Sensitivity — where the decoder actually loses frames

From `MODEM.md`: the remaining misses are overwhelmingly *AX.25-looking
candidates that fail FCS*. The decoder finds frame structure and then has a
handful of bit errors. That is the cheapest possible place to buy sensitivity,
and two levers are currently untouched.

### 5.1 The demodulator's confidence output is discarded
`libmodem::demod_result::confidence` is computed per bit and then used only
for a smoothed display number (`lane.lastQuality`). Keeping the per-bit
confidences for the frame in flight enables:

- **Confidence-ranked FCS repair.** On FCS failure, flip the least-confident
  bit and re-CRC, then the two least-confident, and so on. Direwolf's
  `FIX_BITS` does this blind; ordering by confidence makes it dramatically
  cheaper and far more likely to hit within the first few candidates.
- **Cross-lane soft combining.** 21 lanes see the same burst at different
  phases. When several produce AX.25-like candidates with matching address
  headers but different FCS failures, a per-bit vote weighted by confidence
  recovers frames no single lane can.

`rejectBadFcs` already counts exactly the population this would rescue, so the
before/after metric exists on the current captures.

### 5.2 21 free-running phase lanes is the wrong shape of brute force
The HF lanes run with `pll_alpha = 0` — the Gardner TED is disabled outright
and timing is covered by 21 static phases. Round 7 in `MODEM.md` found that a
small PLL alpha made things *worse*, but that experiment ran the loop free, so
it wandered on noise and silence between bursts.

The fix `MODEM.md` already points at ("packet-synchronous HDLC gating") is to
**gate** the loop: update the TED only while the HDLC codec is in preamble or
in frame, and freeze it otherwise. HF 300 hands us a ~2.1 s flag preamble to
acquire on — an enormous, free training sequence. One gated lane should beat
21 free ones.

### 5.3 Spend the freed lanes on frequency, not phase
The I/Q mixers sit at exactly 1600/1800 Hz. Nothing in the chain detects or
corrects HF mistuning, and 50–100 Hz of offset degrades mark/space
discrimination asymmetrically. A 5-lane offset bank (−60/−30/0/+30/+60 Hz)
is a much better use of the same CPU than 21 phases.

The `TonePowerMeter` Goertzels already in the diagnostics could be extended to
a small comb around each tone to drive a **"you are 40 Hz low"** readout —
genuinely useful on HF, where the operator has no other cue that they are
mistuned.

### 5.4 Receive gate details
- `kReceiveGateMinimumDbfs = -32 dBFS` is an *absolute* floor, but the known
  good captures ran at −21 dBFS RMS with −18 dBFS bursts. A genuinely weak
  signal never opens the gate, so it never gets the burst-boundary decoder
  reset that round 8 showed matters. Should be relative, or much lower.
- The floor tracker's `alpha = 0.04` is applied **per audio block**, so its
  time constant silently changes with the tap's block size. It should be
  normalized to a time constant in seconds.

### 5.5 Duplicate suppression window
`kDuplicateSuppressSeconds = 2` exists to collapse the same frame seen by
multiple lanes — which needs milliseconds, not seconds. At 2 s it can also
swallow a legitimately repeated supervisory frame before `Ax25Connection` ever
sees it. It should scale with frame airtime, and probably exempt frames
addressed to our own callsign.

---

## 5.6 Measured baseline — first working HF session

2026-07-31, HL2 mailbox (KI6BCJ-10) ↔ FlexRadio terminal (KI6BCJ-12), 21.100
MHz, 5 W. 537 bytes delivered over 330 s. Both sides logging `aether.ax25.link`.

| | mailbox (TX side) | terminal (RX side) |
| --- | --- | --- |
| I-frames sent / resent | 9 / 20 | 1 / 0 |
| I-frames received | 1 | 9 |
| Duplicates re-acked | 0 | 3 |
| Sequence gaps dropped | 0 | 0 |
| T1 timeouts | 21 | 0 |
| Measured RTT | 6900 / 7424 / 7948 ms | — |

**Frame error rate = 1 − 12/29 = 59%.** The mailbox made 29 I-frame
transmissions; the terminal decoded 12 of them (9 new + 3 duplicates). Every
frame arrived *eventually* — nothing was permanently lost — but well over half
of all transmissions did not decode.

What this settles:

- **Timing is not the problem.** Measured RTT 6.9–7.9 s against T1 12.6 s is
  36–45% headroom, and the model's 8.4 s estimate is if anything pessimistic.
  The earlier 11.7 s sample that suggested T1 was too tight was an outlier
  (almost certainly a T2-deferred ack). The peer-turnaround allowance does
  **not** need raising.
- **Pacing is not the problem.** Stretch 0.97–0.99x on the HL2 (max chunk gap
  46–66 ms) and 0.96–0.97x on the Flex (21–22 ms, zero late chunks), both well
  inside the 120 ms lead cushion.
- **Decodes are clean or absent.** Confidence 0.89–1.03 on everything that got
  through, clustered near 1.0 — the all-or-nothing FCS signature. There is no
  partial-copy middle ground, which is what makes frame *length* so expensive.

### The airtime budget, at 80 flags / paclen 64

| Component | Data frame | Acknowledgement |
| --- | ---: | ---: |
| **Preamble (80 flags)** | **2.13 s (42%)** | **2.13 s (65%)** |
| Payload + header + FCS | 2.23 s | 0.46 s |
| Postamble | 0.21 s | 0.21 s |
| PTT lead + settle + tail | 0.50 s | 0.50 s |
| **Total** | **5.07 s** | **3.30 s** |

One clean exchange costs 8.4 s to move 64 bytes. Observed throughput was
1.6 B/s — about 4% of the 300-baud channel — with roughly 168 s of the 330 s
actually radiating.

### The TXDELAY sweep

The preamble is the largest single term and the only one that shortens **both**
directions, so it is the first thing to tune. `Ax25DemodConfig::txPreambleFlags`
(0 = profile default) is now an operator knob — Terminal tab "TXD flags", or
`modem preamble <flags|auto>` over the bridge — and both the modulator and the
airtime model read it through `ax25EffectiveTxPreambleFlags()`, so the timers
re-derive on their own. Verified:

| TXD flags | I-frame airtime | modelled RTT | derived T1 |
| ---: | ---: | ---: | ---: |
| 80 (default) | 4577 ms | 8386 ms | 12579 ms |
| 32 | 3297 ms | 5826 ms | 8739 ms |
| 24 | 3084 ms | 5400 ms | 8100 ms |

**Protocol.** Sweep 80 → 48 → 32 → 24, two sessions per setting, and score on
frame error rate — *not* throughput. FER is computed by pairing the two session
summaries:

```
FER = 1 − (peer.rxDecoded / self.txAttempts)
    = 1 − ((peer.iRcvd + peer.iDuplicate) / (self.iSent + self.iResent))
```

Both halves are in the `link status` verb's `quality` block. FER is the right
score because it counts transmissions against decodes and is therefore immune
to T1 behaviour — timing changes cannot skew it, whereas throughput conflates
everything. At ~29 transmissions per session a single estimate carries roughly
±10%, hence two sessions per point.

Expected: 32 flags takes an exchange from 8.4 s to ~5.4 s (−36%), and shorter
frames should lower FER as well, so the two gains compound. Too few flags and
the far end's AGC and PLL cannot settle and it copies nothing — that floor is
what the sweep is looking for.

**Change one thing at a time.** Adaptive T1 (§6.6) must wait until the sweep
is done: it tracks measured RTT, and RTT is dominated by frame airtime, so
tuning it first would be discarded the moment the preamble moves. It is also
the one change that could be mistaken for the thing being measured — a T1 that
runs short retransmits needlessly, which in the logs looks exactly like frame
loss.

---

## 6. Roadmap

Ordered by value per unit of risk. Items 1–2 are on this branch.

1. ✅ **Profile-derived T1 and paclen** (§3) — without this nothing else about
   HF connected mode is testable.
2. ✅ **Link liveness**: T3 idle poll, bounded REJ recovery, retry-on-progress,
   SABME → DM, mailbox inactivity timeout and line-buffer cap (§2).
3. ⏳ **Channel-busy TX inhibit** (DCD + slot backoff). Reuses machinery that
   already exists; needs a sub-second gate accessor and a randomized slot.
4. ⏳ **Confidence-ranked FCS repair** (§5.1). Highest sensitivity per line of
   code, and Captures A/B/C give an immediate A/B on `framesAccepted` vs
   `rejectBadFcs`.
5. ⏳ **Gated timing loop + frequency-offset lanes** (§5.2, §5.3). The real
   decoder fix; deserves its own capture-replay cycle in `MODEM.md`.
6. ⏳ **Adaptive T1** (AX.25 2.2 SRT/T1V), fed by the RTT samples that §4 now
   collects. The measurement had to come first.
7. ⏳ **Mistuning readout** (§5.3) — small, and immediately useful to an
   operator hunting a marginal HF path.
8. ⏳ **File transfer — YAPP** (§7). Works without anything below it, but only
   stops being painful once `MAXFRAME` can exceed 1.
9. ⏳ **Single-keyup multi-frame TX.** Build several I-frames into one audio
   burst so `MAXFRAME` can rise above 1. This is the throughput ceiling today:
   with a window of 1, every frame costs a full radio turnaround. Highest-value
   change for file transfer, more than the protocol choice.
10. ⏳ **FEC — IL2P or FX.25.** Reed-Solomon around AX.25 turns marginal frames
    into good ones instead of retransmitting them. On a long HF session — and
    on any file transfer, where hundreds of consecutive frames must all pass
    FCS — this is arguably a prerequisite rather than an enhancement. Same axis
    as §5.1, one layer down.
11. ⏳ **TX preamble review.** HF's 80 flags is 2.13 s of dead air per frame,
    the single largest term in the airtime budget. Real HF TNCs use far less.
    Shortening it would nearly halve frame airtime — but it protects the *far*
    end's AGC and PLL, so it cannot be changed on our own authority. Needs an
    A/B against a real BBS before touching, and it is deliberately left alone
    here: the airtime model reads the constant, so if it ever drops, T1 tracks
    it automatically.

### Deliberately deferred

- **Terminal autoanswer.** `Ax25Connection` is symmetric: it accepts any SABM
  addressed to its local address regardless of role, so the Terminal is
  already an answering station. An unsolicited inbound connect to MYCALL is
  accepted silently and yanks the operator into converse mode mid-typing.
  There should be a setting, defaulting to off — the PMS is the thing meant to
  answer. Not addressed here by decision, not oversight.
- **Callsign collision guard.** The PMS and the Terminal are both fed every
  decoded frame unconditionally, with nothing preventing an operator from
  configuring the same callsign in both. If MYCALL equals the PMS listen or
  alias address, *both* state machines answer the same SABM — two UAs on the
  air, two sessions believing they own the peer, duelling acks. Belongs with
  the autoanswer setting.

## 7. File transfer

**Direction chosen: YAPP**, for both the Terminal and the PMS.

The rationale is interop, not elegance. The Terminal's value is dialing BBSes
we do not control, and YAPP is what those BBSes speak — it is the native
binary-transfer protocol of the AX.25 packet BBS world. A protocol of our own
design would work only against our own mailbox, which is the one case the user
already has other options for. Implementing YAPP on both sides means the
Terminal can pull files from real packet BBSes, and other people's terminals
can transfer against our PMS.

### 7.1 The constraint that governs every decision here

At these speeds **turnarounds dominate, not bit rate.** One HF round trip is
~8.4 s (§3). A protocol that acknowledges every block therefore moves one
paclen per round trip:

```
64 bytes / 8.4 s ≈ 7.6 B/s        against a 37.5 B/s channel
```

— roughly 80% of the link thrown away on protocol chatter. So the data phase
must **stream, with no application-layer acknowledgements**, and let AX.25's
own window carry the reliability.

This is also the whole reason the obvious candidates are wrong:

| Protocol | Verdict |
| --- | --- |
| **YAPP** | **Chosen.** Native to AX.25 BBSes, does not re-do error control, interops with stations we do not control. |
| **XMODEM / YMODEM** | Disqualified. Stop-and-wait: one block per round trip, by construction. |
| **ZMODEM** | Poor fit. Designed for a full-duplex reliable stream; its ZRPOS rewind fights AX.25's own retransmit, and it degenerates badly on half duplex with multi-second turnarounds. |
| **Kermit** (long packets + sliding window) | Workable and famously robust, but its value is surviving 7-bit, flow-controlled, lossy paths — none of which apply here. Pays for machinery we do not need. |
| **FBB B1F/B2F** | Best answer *if* Winlink interop ever becomes a goal for the PMS: compression and resume built in, and it is what Winlink speaks. Worth revisiting as a second protocol, not a replacement for YAPP. |

### 7.2 Do not duplicate the ARQ

`Ax25Connection` already provides reliable, ordered, error-free, 8-bit-clean
delivery: FCS validation, REJ, sequenced retransmit. Any file protocol that
brings its own error correction pays twice — two retransmit timers fighting
each other, and two sets of checksums on a link where every byte costs a third
of a second. What is genuinely needed on top of AX.25 is only *metadata,
framing, compression, and resume*. YAPP is a good fit precisely because it was
designed against this link layer and assumes it.

### 7.3 Open question to settle before implementing

**Read the YAPP spec first.** Its exact acknowledgement discipline in the data
phase needs verifying: the recollection driving this choice is that YAPP
streams data blocks rather than acking each one, but that is not verified here,
and if it does ack per block it collides head-on with §7.1. That single fact
determines whether YAPP is usable as-is on HF, needs a streaming variant, or
should be restricted to the VHF profile. Settle it before writing code.

### 7.4 Prerequisites in our code

1. **`sendData()` has no backpressure.** It appends straight into
   `m_sendBuffer` with no cap, so a 200 KB file means 200 KB queued in RAM
   draining at ~30 B/s with no clean cancel. Needs a `bytesQueued()` accessor
   and a ready-for-more signal so the sender paces against the link.
2. **An explicit binary mode switch.** `PmsMailbox::onLinkData` splits on CR
   and treats Ctrl-Z (0x1A) as end-of-message; `TncTerminal::sendToPeer` does
   `toLatin1()` and appends CR. Binary payloads trip all three. A real transfer
   state that bypasses the line parser is required, on both sides.
3. **The PMS never sends RNR**, so there is no inbound flow control at all. A
   sender that outruns us has no way to be told.
4. **`MAXFRAME = 1`** (§6 item 9) is the throughput ceiling. Transfers work
   without it; they stop being painful with it.

### 7.5 Design notes for the implementation

- **Block size should equal paclen**, so one transfer block is exactly one
  I-frame. A loss then retransmits exactly one block, with no fragmentation
  amplification.
- **Compression matters more than protocol choice.** At ~30 B/s, deflate's
  2–4× on text is the single largest win available anywhere in the transfer
  path — larger than any framing decision.
- **Resume is not optional on HF.** Sessions die mid-transfer; a transfer that
  cannot resume from an offset is useless for anything but small files.
- **CRC per block for detection only**, not recovery — AX.25 already recovers.

