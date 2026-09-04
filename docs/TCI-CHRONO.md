# TCI TX_CHRONO pacing — investigation notes (#5133)

Date: 2026-09-03
Branch: `fix/5133-tci-ft8-tx-spikes`
Base: `origin/main` @ `52a0f3dc`
Issue: [aethersdr/AetherSDR#5133](https://github.com/aethersdr/AetherSDR/issues/5133)
Status: read-only findings. No code change yet.

Related: [`docs/architecture/tx-audio-signal-path.md`](architecture/tx-audio-signal-path.md),
[`docs/architecture/audio-pipeline.md`](architecture/audio-pipeline.md),
Thetis TCI oracle (`/Users/patj/oracles/thetis-tci-oracle.md`, TX audio / chrono section).

---

## Symptom

FT8 over TCI (WSJT-X or JTDX) produces a short wideband spur on the panadapter
and waterfall, typically **one to two seconds into the over**. The rest of the
transmission is a clean constant-envelope tone and usually **decodes**.

This is a **shared TX-audio clocking** problem, not an Icom-only path. The
GitHub reports are Flex:

| Reporter | Radio | Client | OS | Notes |
|---|---|---|---|---|
| KN7K | FLEX-8600 fw 4.1.5 | JTDX | Win11 | Random bars; predates 26.8.3 |
| g0cgl / EI4KF | FLEX-8400M fw 4.2.20 | JTDX | Win11 | Same op, two sessions 2026-09-01: CAT bundle (17:35) + Desktop perf log (15:19). PGXL+TGXL. |
| skerker | FLEX-8400 | WSJT-X | — | No PGXL, no APD, **no** burst events |
| local (Pat) | IC-7300MK2 | WSJT-X | macOS | Same 1–2 s spur, consistent. Gold chrono log: `aethersdr-20260901-204049.log`. |
| local (Pat) | FLEX-8400M KI6BCJ | TCI | macOS | `aethersdr-20260830-174437.log` — only 2 chronos, confirms Mac Flex path. |

FT8 is a continuous tone. Any dropped, doubled, or zero-padded block is a
timebase discontinuity and shows as a short splatter tick. Average rate can
still be 48 kHz, which is why the decoder survives.

---

## The path

WSJT-X / JTDX do **not** push TX audio. AetherSDR pulls it.

1. Client: `trx:0,true,tci;`
2. `TciServer` keys via `setTransmit(..., PttSource::Dax)` (audio source) and
   waits for radio-confirmed TX.
3. On confirm, `startTxChrono()` sends type-3 **TX_CHRONO** frames to that
   WebSocket.
4. Client answers each chrono with one type-2 **TX_AUDIO** block.
5. `onBinaryMessage()` classifies layout, resamples 48 kHz → 24 kHz, applies
   TCI gain, and `QMetaObject::invokeMethod`s `AudioEngine::feedDaxTxAudio`.
6. `feedDaxTxAudioInternal()` packetizes **immediately, unpaced**:
   - Flex (not host-modulating): VITA-49 `dax_tx` (PCC 0x0123 int16 mono).
   - Icom / HL2 (`takesTxAudioOverSeam`): `txFinalMonitorPcmReady` →
     `RadioModel::submitTxAudio`. Icom `TxPacketizer` then clocks RS-BA1 at
     10 ms, cap 250 ms, drop-oldest.

`TciServer` has **no thread of its own**. The chrono timer runs on the GUI
event loop. `AudioEngine` is on the audio thread.

---

## Chrono as implemented

```cpp
// src/core/TciServer.cpp
constexpr int kTxChronoSamples = 2048;       // float payload length to the client
constexpr int kTxChronoStereoFrames = 1024;  // 2048 floats = 1024 L/R pairs
constexpr qint64 kTxChronoPeriodNs =
    (kTxChronoStereoFrames * 1'000'000'000LL) / 48000LL;  // 21.333… ms
constexpr int kTxChronoPollMs = 5;
```

A fixed 21 ms `QTimer` would run ~1.6 % fast and warp digital tones, so the
pacer polls at 5 ms (`Qt::PreciseTimer`) and emits from a monotonic
accumulator:

```cpp
m_txChronoAccumNs += m_txChronoClock.nsecsElapsed();
m_txChronoClock.restart();
while (m_txChronoAccumNs >= kTxChronoPeriodNs) {
    sendTxChronoFrame(client);
    m_txChronoAccumNs -= kTxChronoPeriodNs;
}
```

That `while` is **unbounded**. After a GUI stall of N ms it fires
`floor(N / 21.333)` chrono frames back-to-back. There is no outstanding-request
cap and no forget/reset of backlog.

`startTxChrono()` also sends one chrono immediately, then starts the timer.

### What Thetis does (authority we follow)

From the Thetis TCI oracle, TX chrono:

- targets the negotiated buffering interval **plus 50 ms**;
- permits **at most 64 outstanding** requests;
- forgets stuck outstanding counts after `max(250 ms, 4 × buffering)`.

Conformance line in the same oracle: *“TX chrono pacing recovers from missing
responses without unbounded growth.”* We fail that today.

The Opus TX path already solved the sibling problem for itself:
`OpusTxPacer` with `kMaxPacketsPerDrain = 3` and a ~200 ms queue cap. DAX/TCI
has no equivalent.

---

## Evidence corpus

Win+Mac × Flex+Icom. GitHub zips unpacked under `/tmp/issue-5133-logs/`.
Desktop log stays on `~/Desktop/`. Mac pulls live under
`/private/tmp/aethersdr-logpull/20260903-165337/` (not in git).

| Source | Who | Radio / OS | `aether.cat` | `aether.perf` | What it is good for |
|---|---|---|---|---|---|
| `support-bundle-20260821-094759.zip` | KN7K | FLEX-8600 / Win11 | off | **on** | GUI stall rates (~every 2.2 s) |
| `support-bundle-20260821-112251.zip` | KN7K | FLEX-8600 / Win11 | **on** | **on** | Six FT8 overs + APD insanity overlay |
| `support-bundle-20260901-181521.zip` | g0cgl / EI4KF | FLEX-8400M / Win11 | **on** (CAT-only) | off | 14 TCI overs; extra chrono in second second |
| `~/Desktop/aethersdr-20260901-151922.log` | g0cgl / EI4KF | FLEX-8400M / Win11 | off | **on** | Same day, earlier session. Stalls *during* live overs; PTT bounce |
| `aethersdr-20260901-204049.log` | Pat | IC-7300MK2 / macOS | **on** | — | **Gold:** 56 chrono / 53 full FT8; first extra at +2–3 s in 67 % |
| `aethersdr-20260830-174437.log` | Pat | FLEX-8400M / macOS | **on** | — | Mac Flex path exists; only 2 chronos |
| newest 5 Mac logs (2026-09-02/03) | Pat | IC-7300MK2 / macOS | on, no client | — | Not FT8. Icom `dropping xmit` / CI-V backlog only |
| `support-bundle-20260830-152309.zip` (#5340) | WA8PAM | FLEX-6400 / Win11, **2 pans + 2 TCI clients** | **on** | off | Slice B `trx=1`. Same +2–3 s extra chrono as #5133 |
| `support-bundle-20260830-153508.zip` (#5340) | WA8PAM | same | **on** | off | Slice A `trx=0`. Same extras, same audio level as B |

`logTxAudioSummary()` is `aether.cat` info. Without that category the TCI
numbers are invisible. Enable **TCI/CAT/rigctld** in Settings → Logging for
any follow-up bundle.

`logTxAudioSummary()` is `aether.cat` info. Without that category the TCI
numbers are invisible. Enable **TCI/CAT/rigctld** in Settings → Logging for
any follow-up bundle.

### KN7K perf (first bundle, ~36 h)

GUI event-loop overruns (`uiLagMaxMs`), 1-second windows:

| Stall | Count | Meaning for chrono |
|---|---|---|
| > 25 ms | 90,541 / 129,838 (70 %) | ≥ 1 missed 21.3 ms tick |
| > 50 ms | 16,893 (13 %) | ≥ 2 missed ticks |
| > 100 ms | 36 | |
| max | 1,675 ms | 78 frames if replayed unbounded |

`PerfStall kind=uiHeartbeat` ~1,600/hour during operating hours, median 53 ms,
p99 80 ms — **one stall every ~2.2 s**. A 12.64 s FT8 over statistically
contains several. The first one after key-up is the 1–2 s spur.

### TCI TX summaries (g0cgl + KN7K second bundle)

Per-second bins look healthy:

- ~48 audio blocks/s (`kTxSummaryEveryBlocks = 48`)
- `inputFramesSrc = blocks × 1024` on every over
- `layout=duplicated-stereo`, `clips=0`
- end-of-over `effective48k` ≈ 48.00–48.07 kHz
- PTT → chrono start on Flex: **70–100 ms** (not the 1–2 s delay)

That **clears**:

1. **Block-size mismatch.** We ack `audio_stream_samples:` verbatim but keep
   hardcoded 2048. WSJT-X/JTDX in these bundles used 1024 stereo frames. A 2:1
   mismatch would show `inputFramesSrc` vs `requested48k` diverging for the
   whole over. It does not.
2. **Per-block mono/stereo flip.** A mono classification adds +1024 to
   `inputFramesSrc` over `requested48k`. The delta is never positive across
   thousands of blocks.

That does **not** clear catch-up. The summaries are **1-second bins**. A 50 ms
stall that emits 3 chronos at once, then three TX_AUDIO blocks in one shot,
still averages to 48 blocks/s.

What the bins *do* show is extra **requests**, not extra **audio**:

- g0cgl: most overs have seconds with `dreq=50176` (49 chronos) vs
  `dinf=49152` (48 audio blocks), often in the **second second** of TX.
  Occasional `dreq=51200` (two extra chronos in one second).
- KN7K first-second `effective48k` (this field is **chrono request rate**,
  `requested48k / elapsed`, not audio delivery rate): n=6, mean **46623**,
  worst **45302**. Chrono running ahead of the client at the start of the over.

`effective48k` in `logTxAudioSummary()`:

```text
effectiveRate48k = m_txChronoRequestedFrames / elapsedSec
```

So a low first-second number is “we asked slower than 48 kHz for this window
because start-up / stall stretched elapsed,” or, when `requested48k` is
*ahead* of `inputFramesSrc`, “we asked for more blocks than the client
answered.” Either shape is a discontinuity waiting to happen; the 1 s bin
cannot say whether the answers later arrived as a clump.

### g0cgl session faults (CAT-only, 14 overs)

1. **17:41:30** `trx:true` then `trx:false` 47 ms later. One silent block
   (`peak=0`, `blocks=1`). PTT bounce, not a mid-tone spur.
2. Several overs with two extra chronos in one second.
3. TX 13 started at `:30.615` (late into the slot).

One GUI watchdog line in that bundle, at process start (`~1234 ms`), not
during an over. `aether.perf` was off, so intra-over stalls are not recorded.

---

## Follow-up logs (2026-09-03)

Same-day Windows Flex log from EI4KF on the Desktop, plus the Mac/Icom
sessions pulled from `patj@mac.jensencloud.net`. Together with the GitHub
bundles this is Win+Mac × Flex+Icom.

### EI4KF Desktop — `~/Desktop/aethersdr-20260901-151922.log`

Same operator as the 17:35 CAT bundle (Erik EI4KF, FLEX-8400M, 26.9.1,
Win11, Ryzen 9 7950X, PGXL 3.9.1 + TGXL). **Different session** (15:19–15:24).
`aether.cat` is off, so no chrono summaries. `aether.perf` is on.

Three `xmit` cycles:

| TX | Window | Duration | uiHeartbeat stalls in window |
|---|---|---|---|
| 1 | 15:22:01.619 → 15:22:02.486 | **0.867 s** | none (uiLagMax 22.7 ms) |
| 2 | 15:22:30.709 → 15:22:43.721 | 13.012 s (full FT8) | four: **+7.90 / +8.81 / +9.40 / +9.50 s**, 59–84 ms |
| 3 | 15:23:00.023 → 15:23:13.690 | 13.667 s (full FT8) | none in-window; first 2.5 s uiLagMax 27–30 ms |

TX 1 is the same PTT-bounce class as 17:41:30 in the CAT bundle. TX 2 proves
the GUI thread still hiccups **during** a live over on this machine (enough
to emit 3–4 extra chronos if the pacer had been running). TX 3’s first-two-second
uiLag (~30 ms) is only one missed tick — the 1–2 s spur does not require a
heroic stall, just the unbounded `while`.

No `apd insanity` in this file (`aether.connection` status decode is not
verbose enough here). PGXL came ready at 15:21:55, immediately before the
bounces.

### Mac logpull — `/private/tmp/aethersdr-logpull/20260903-165337/`

Newest five files on the test Mac were **not** FT8/TCI sessions (connect,
volume, short local PTT). `TciServer` was listening but there was no
`client connected` and no `TX_CHRONO started`. Remote grep found the real
overs; those two files were pulled as well.

Audio-summary presence (skill check, newest five):

| log | startup | rx | tx | cw | aux | failure | category |
|---|---|---|---|---|---|---|---|
| 20260902-203157 | yes | yes | yes | yes | yes | no | yes |
| 20260902-203328 | yes | yes | yes | yes | yes | no | yes |
| 20260902-212828 | yes | yes | yes | yes | yes | no | yes |
| 20260903-072440 | yes | yes | yes | yes | yes | no | yes |
| 20260903-152458 | yes | no | no | no | no | no | yes |

20260903-152458 never connected a radio (shutdown-only). TX/CW/aux present
on the others is event-dependent and expected.

Icom-specific noise in the newest files, **not** the FT8 spur:

- `RadioModel: no command plane for this backend, dropping xmit 1/0` and
  `dropping transmit set dax=1` — Flex text still emitted at Icom, then
  dropped. TCI still broadcasts `trx:0,true` from model state.
- `civ-timeout-backlog` incidents (queueDepth 14–24, `lastResponseMs` 427–1025).
  20260903-072440 has five short keys (1.3–3.3 s), no chrono, no TCI client.

### Mac Icom FT8 — `aethersdr-20260901-204049.log` (the money log)

| | |
|---|---|
| Build | 26.9.1 |
| OS | macOS 26.6, Apple M5 |
| Radio | IC-7300MK2 (`family=icom`) |
| Client | TCI from `::ffff:*.*.*.1` at 20:48:39 |
| Overs | **56 chrono sessions, 53 full ~13.6 s FT8** |

Same 1-second-bin health as Flex: `layout=duplicated-stereo`, `clips=0`,
stop `effective48k` 47.9–48.05 k, `inputFramesSrc = blocks × 1024`.
`trx:true` → chrono start is **0–2 ms** (Icom `setKeying` is optimistic).

Catch-up is **not** rare. 210 extra-chrono seconds across 53 full overs
(`dreq=50176` or `51200` vs `dinf=49152` — one or two unanswered chronos
in that second). Extra events run the whole over (GUI keeps hitching);
the **first** one is what matches the waterfall tick:

| First extra at | Count (of 55) |
|---|---|
| **+2.0 s** | 13 |
| **+3.0 s** | 24 |
| +4.0 s | 6 |
| +5.0 s | 7 |
| later | 5 |

All extra-chrono events (not just first), 0.5 s bins: +2.0 (13), +3.0 (27),
+4.0 (11), +5.0 (24), then 12–26 per second through +13.0. The pacer
replays a stall whenever one happens; the spur people notice is the
**first** replay.

**37 / 55 = 67 % of overs put the first extra chrono at +2–3 s.** That is
the local Mac + IC-7300MK2 + WSJT-X symptom (“usually a second or two in”)
in the same numbers we already had from g0cgl’s Flex CAT log. Family does
not matter; the pacer does.

First-second `effective48k` (chrono *request* rate): n=56, min **46802**,
p50 48003, max 48959. Worst first seconds are ~2.5 % slow, then the extra
chrono at +2–3 s is the catch-up.

`trx:0,true` count is 113 vs 56 chrono starts — extra true/false pairs are
short keys or bounces, same class as EI4KF’s 0.87 s `xmit` and g0cgl’s
47 ms abort, not the mid-tone spur.

### Mac Flex — `aethersdr-20260830-174437.log`

Same Mac, `family=flex`, FLEX-8400M KI6BCJ. TCI client connected; only
**two** chrono sessions. Too small to histogram; confirms the Mac also
runs the Flex path.

---

## Issue #5340 — two slices, two WSJT-X, same chrono (2026-08-30)

[#5340](https://github.com/aethersdr/AetherSDR/issues/5340) is a *different
user-facing claim* (Slice B gets ~20 PSK Reporter spots vs Slice A ~200;
ALC gauge floors at −20 dBFS on B) on a **two-slice / two-pan** station.
Pulled both attached bundles:

| Zip | Reporter label | Radio |
|---|---|---|
| `support-bundle-20260830-152309.zip` | Slice B TX | FLEX-6400 fw 4.2.18, WA8PAM, 26.9.1, Win11, **GTX 1050 Ti**, `PanadapterLayout2v` |
| `support-bundle-20260830-153508.zip` | Slice A TX | same station, same settings |

`aether.cat` on, `aether.perf` **off**. Unpacked under `/tmp/issue-5340-logs/`.

### Load that #5133 single-slice logs did not have

- **Two SpectrumWidgets** (`slice added 0` and `slice added 1`, two waterfall
  pipelines 1262×64 / 1383×64).
- **Two TCI clients** from `::1` in every session: `audio_start:0` then
  `audio_start:1`. Two WSJT-X instances, `trx_count:2`,
  `trx0=slice0/dax1 trx1=slice1/dax2`.
- Chrono still runs on the **GUI thread**. Two pans + two RX-audio TCI
  pumps is exactly the event-loop backlog the unbounded `while` is
  sensitive to. GPU is a 1050 Ti.

PTT routing is clean — not a wrong-slice key:

```
trx=1 rxSlice=1(trx1) -> txSlice=1(trx1) (the requested slice)
trx=0 rxSlice=0(trx0) -> txSlice=0(trx0) (the requested slice)
```

### Chrono extras — same signature as #5133, on **both** slices

| | Slice B (trx=1) | Slice A (trx=0) |
|---|---|---|
| Full-ish overs | 4 (one 7.6 s abort) | 4 (one 4.3 s abort) |
| Extra-chrono seconds (`dreq−dinf ≥ 1024`, ~1 s bin) | **14** | **12** |
| Typical first extra | **+2.07 / +3.08–3.10 s** | **+2.07 / +3.09–3.10 s** |
| Stop `effective48k` | 48.00–48.04 k | 48.01–48.07 k |
| peak / rms / gain | 0.976–0.983 / ~0.66 / 0.99 | 0.976–1.000 / ~0.66 / 0.99 |
| clips | 0 | 2 samples on one over |

First extra at +2–3 s is the same bin as the Mac IC-7300MK2 gold log
(67 %) and g0cgl’s Flex CAT log. Catch-up is **not** unique to slice B.
If anything the two-pan + two-client load makes extras *routine on every
over of both slices* — which is what the graphics-backlog hypothesis
predicts, and what a single-slice quieter station (skerker) did not see.

`aether.perf` is off, so we cannot count `uiHeartbeat` inside the overs.
The only stalls in these files are startup `MainThreadWatchdog` ~1.8–2.0 s
(connect / first paint), not the mid-over hitch. The 1 s `dreq=50176`
lines are still the catch-up proxy.

### What #5340 is *not*

Audio leaving AetherSDR is the same on A and B (Pat’s in-thread reading
holds). The ALC −20 on B is the **last-definition-wins ALC meter index**
(`MeterModel` stores `ALC` as a scalar; `COMPPEAK` is already per-slice).
That is #3830, not chrono.

The 20-vs-200 spot split is **not** explained by chrono: the discontinuity
hits both slices equally. Reporter later reproduced the spot split on
SmartSDR 4.2.2 as well, with a local friend seeing equal on-air level —
so that half is not AetherSDR TX audio. Chrono can still put a short
spur on *every* over of a two-pan station; it just is not the A-vs-B
discriminator.

### Implication for the #5133 fix

Two pans + two TCI clients is a **forced** GUI-load case for the pacer.
A clamp that is clean on a single-pan Mac Icom session still has to be
clean here: extras per over should drop on *both* trx=0 and trx=1. This
is the busy-window regression test we already wanted, with a real
two-slice operator log instead of a synthetic “make the waterfall fast.”

---

## Root cause (current best)

**Primary, family-independent:** TX_CHRONO is a wall-clock accumulator on the
GUI event loop with **unbounded catch-up**, and DAX/TCI emission is **unpaced**.

After a 50–80 ms stall (panadapter / waterfall / any GUI hitch):

1. The `while` loop emits several chrono frames in one timer tick.
2. The client answers with a burst of TX_AUDIO (or a short / zero-padded
   block if it has nothing ready).
3. Those blocks are queued to the audio thread and packetized immediately.
4. Flex: a clump of `dax_tx` VITA packets hits the radio in one shot.
   Icom: the same clump hits `TxPacketizer` (250 ms, drop-oldest; underflow
   is silence, which the radio jitter buffer treats as a discontinuity).
5. The radio sees a phase/timebase step → short wideband spur.
6. Average rate stays ~48 kHz, so the rest of the over decodes.

**Why ~1–2 s in:** first GUI stall *after* the over is already keyed, not the
PTT handshake. Flex PTT→chrono is ~80 ms; Icom is 0–2 ms (optimistic key).
The Mac IC-7300MK2 session puts the **first extra chrono at +2–3 s in 67 %
of overs**. KN7K’s heartbeat stalls were every ~2.2 s. Same clock, both
families.

This is the same clocking defect on Flex, Icom, and HL2. Downstream packetizers
differ; the burst is produced above them.

---

## Ruled out (for this spur)

| Hypothesis | Why not |
|---|---|
| Icom-only PTT / unkey settle / 1250 ms confirmation timeout | Would abort or bounce the over, not tick a live tone. Flex reports have the same spur. |
| `audio_stream_samples` false ack (2× rate) | Bundles show 1024-frame blocks matching chrono. Still a real defect — see below. |
| Per-block L/R layout flip | Frame counts never show a 2× jump. `layout=` is a sticky OR, but `inputFramesSrc` would move. |
| APD failing to converge | KN7K-only overlay (bars every ~3.3 s, 119 `apd insanity` pairs, `equalizer_active=0`). Does not explain g0cgl, skerker, or Icom. |
| Mic bleed before `tciAudioFresh()` | Plausible on Icom/macOS for the first ~20–50 ms after key, not a 1–2 s mid-tone tick. Do not start there. |
| Icom `dropping xmit 1` / `dropping transmit set dax=1` | Flex command-plane text at an Icom. Separate seam leak. TCI still keys via `setKeying`; 53 full overs on the money log prove audio flowed. |

### APD is a separate Flex defect

KN7K had APD enabled with no usable sampler. The radio sampled at +3.3, +6.5,
+9.7, +13.0 s into each over and rejected the model every time (`PA gain
characteristic is nonmonotonic`, `Excessive uncorrected frequency error`, …).

`apd insanity "reason", "reason"` contains no `=`, so the status splitter puts
the whole body in `object`. `RadioModel` folds tokens into junk bare-flag keys;
`FlexBackend::decodeApdStatus()` ignores them. **No log line, no UI.** Fix
visibility separately from chrono. Do not treat APD as the TCI spur.

---

## Code defects to fix (chrono first)

1. **Clamp chrono catch-up.** 1 frame per poll tick; cap `m_txChronoAccumNs`
   to a bounded backlog (Thetis-shaped outstanding window, not unbounded
   replay). Forget stuck outstanding counts after a few hundred ms.
2. **Pace DAX/TCI emission** in `feedDaxTxAudioInternal()` the way
   `OpusTxPacer` paces Opus (`kMaxPacketsPerDrain`, queue cap).
3. **Stop lying about `audio_stream_samples:` / `tx_stream_audio_buffering:`.**
   Echoing the request while the pacer stays on hardcoded 2048 is a false ack.
   Reply with the value in force, or honor the client per session.
4. **Latch mono/stereo for the TX session** from the first non-silent blocks;
   skip silence in the vote. Not implicated in these logs, still a landmine.
5. **Intra-second telemetry.** One line per poll (or on burst): chrono frames
   emitted *this tick*, TX_AUDIO gap since last block, accumulator ns.
   1-second summaries cannot prove or disprove a clump. Needs `aether.cat`.
6. **APD insanity visibility** (separate PR): intercept before the bare-flag
   fold; `qCWarning(lcTransmit)` so it lands in a default support bundle.
7. **Icom command-plane leak** (separate): stop sending Flex `xmit` /
   `transmit set dax=` through `RadioModel::sendCommand` on a backend with
   no command plane. Logged on every recent Mac Icom connect; not the spur.

---

## How to prove a fix

Primary reproduction is the local Mac + IC-7300MK2 + WSJT-X path that wrote
`aethersdr-20260901-204049.log` (first extra chrono at +2–3 s in 67 % of
overs). Secondary is EI4KF/g0cgl Flex+JTDX on Windows.

1. Enable **TCI/CAT/rigctld** (`aether.cat`) and **Performance** (`aether.perf`).
2. FT8 over TCI, same radio, waterfall at a normal FPS.
3. Watch for `TCI TX summary` plus the new burst line: `chrono_this_tick>1`
   aligned with a waterfall tick ~1–2 s in. The 1 s `dreq=50176` vs
   `dinf=49152` line is the current proxy for that burst.
4. After the clamp: `chrono_this_tick` never exceeds 1–2; first extra-chrono
   histogram at +2–3 s goes to ~0; no spur; decode still succeeds;
   `effective48k` at stop stays ~48000.
5. Repeat with the window busy (fast waterfall) and minimized. The busy case
   is the regression test; minimized is the control. EI4KF TX 2 (stalls at
   +8–9.5 s) is the “busy mid-over” shape. #5340 (two pans, two WSJT-X,
   GTX 1050 Ti) is the real two-slice busy case — extras must drop on
   **both** trx=0 and trx=1.
6. Automation: no TX-keying verb is required to *see* chrono, but a live over
   still keys the radio. Use dummy load / ANT2 gate. A useful new verb would be
   a read-only `tci chrono` snapshot (outstanding, accum ns, last gap).

Do not use 1-second `blocks=48` as proof of cleanliness. That was the mistake
in the in-thread Flex analysis of KN7K’s second bundle. Do not use the newest
Mac logs as a TCI-FT8 sample — they have no client and no chrono.

---

## Local references

Code:

- Pacer: `src/core/TciServer.cpp` (`m_txChronoTimer` ctor ~457, `startTxChrono`
  ~3375, `sendTxChronoFrame` ~3428, `logTxAudioSummary` ~3446)
- False ack: `TciServer.cpp` `audio_stream_samples:` / `tx_stream_audio_buffering:`
  ~1283
- Layout vote: `TciServer.cpp` `onBinaryMessage` ~2486
- Unpaced DAX TX: `src/core/AudioEngine.cpp` `feedDaxTxAudioInternal` ~9045
- Bounded Opus pacer: `src/core/OpusTxPacer.h`
- Icom packetizer (downstream only): `src/core/backends/icom/IcomAudio.h`
  `TxPacketizer::kMaxPendingBytes = 24000`
- Thetis chrono contract: `/Users/patj/oracles/thetis-tci-oracle.md`
  (“TX audio and `trx`”, outstanding-request cap)

Logs (this round):

- GitHub zips: `/tmp/issue-5133-logs/`
- EI4KF Desktop: `/Users/patj/Desktop/aethersdr-20260901-151922.log`
- Mac pull: `/private/tmp/aethersdr-logpull/20260903-165337/`
  - gold Icom FT8: `aethersdr-20260901-204049.log`
  - Mac Flex: `aethersdr-20260830-174437.log`
- #5340 zips: `/tmp/issue-5340-logs/` (`152309` = slice B trx=1,
  `153508` = slice A trx=0)
