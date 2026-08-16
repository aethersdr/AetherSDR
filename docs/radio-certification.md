# Radio certification reference

The table `radiocert` works from, and the checklist for bringing a new radio up.

Why the tool is shaped this way — and what it still cannot do — is in
[`CERTIFICATION.md`](CERTIFICATION.md). Read that before trusting a clean report.

Two rules run through all of it, both learned expensively:

1. **Readback is not proof.** A model keeps whatever string or number it is
   handed. Every entry below is therefore certified by an **observable effect**,
   not by reading back what was written. The mode map passed for twelve modes
   while the backend mapped nine of them.
2. **A meter that is defined but never fed is worse than a missing one.** It
   renders as a real instrument reading a quiet band. `IRadioBackend::meterUpdate`
   had no consumer at all for an entire bring-up, and the S-meter was correct
   for days without ever being visible.

---

## Meters

`source:name` is the pair `MeterModel::findMeter()` takes. "HL2" says whether the
Hermes-Lite 2 can physically produce it.

### Receive

| source:name | unit | HL2 | Idle expectation | Certification stimulus | Tolerance |
|---|---|---|---|---|---|
| `SLC:LEVEL` | dBm | yes | −140 … −40, updating | inject a known carrier off-centre; level tracks it | ±3 dB relative |
| `SLC:LEVEL` age | ms | yes | < 500 while receiving | — | a stale S-meter is a dead S-meter |

### Transmit

| source:name | unit | HL2 | Keyed expectation | Certification stimulus | Tolerance |
|---|---|---|---|---|---|
| `TX:MICPEAK` | dBFS | yes | tracks input | inject −20 dBFS tone → reads −20 | **±1 dB** |
| `TX:SWR` | SWR | yes | 1.0–1.5 into a dummy load | key with audio | **±0.3** |
| `TX:SWR` idle | SWR | yes | **absent** | key with no audio | present-while-idle means the ratio saturated |
| `TX:FWDPWR` | dBm | yes | rises with drive | raise the drive **one nibble** → rises | see the note below on halving |
| `TX:REFPWR` | dBm | yes | ≪ forward into a load | key into a dummy load | ≥15 dB below forward |
| `TX:ALC` | dBFS | **host-side** | post-ALC transmit peak | sweep the input 20 dB → reading does **not** move | **±1 dB across the sweep** |
| `TX:COMPPEAK` | dB | host-side | compression applied | PROC on → rises above 0 | reads 0 with PROC off |
| `TX:MIC` | dBFS | host-side | pre-gain mic level | — | not yet wired |
| `TX:HWALC` | dBFS | **no** | — | Flex RCA jack; no HL2 equivalent | — |

Four of those rows read "counts only", "not yet wired" or "gain the ALC applies"
until 2026-08-10, when a live run showed all four meters defined and fed. The
stimulus column is the part that stayed wrong longest, and it is the part that
matters: **`TX:ALC` is the post-ALC transmit PEAK in dBFS, not the gain the ALC
applied.** That is what `Hl2Backend::defineMeters` publishes and what
`MeterModel::swAlc()` binds to, so the old "quiet input → gain rises, ±3 dB"
asked for a quantity this radio never produced — and would have been recorded as
a failure by anyone who ran it, on a meter that turns out to be exact.

`TX:FWDPWR` and `TX:REFPWR` are published in dBm through the reference curve in
`Hl2Backend::directionalWatts()`, which is **uncalibrated** — the value is an
estimate, not a measurement, and the meter descriptions say so. Uncalibrated is
not the same as absent, and this table said "counts only" long after they were
being published; a stale not-fed claim is what switches off the check that
would notice the meter regressing (CERTIFICATION.md 1.37).

Both of the changed stimulus cells above are measurements, not preferences —
`TX:FWDPWR`'s nibble step replaces a halving the hardware refutes, and
`TX:ALC`'s no-movement sweep replaces envelope tracking that would fail a
correct meter by ~17 dB. The numbers are under *Certified by effect,
2026-08-10* below.

### Radio / hardware

| source:name | unit | HL2 | Expectation | Certification stimulus | Tolerance |
|---|---|---|---|---|---|
| `RAD:PATEMP` | degC | yes | 20–60 idle | key 10 s → **rises ≥0.5 °C** | rise is the check, not the value |
| `RAD:+13.8A` | Volts | **no** | — | HL2 reports no supply voltage | — |
| `AMP:*`, `TGXL:*` | — | **no** | — | external amp / tuner only | — |

### Certified by effect, 2026-08-10 (Hermes-Lite 2, 14.200 MHz USB, dummy load)

Gateware v74, `radiocert meters 14.200` plus a manual drive sweep, host-side
1 kHz test tone as the stimulus. Every row below is an **observed effect**, not
a readback.

| meter | stimulus | observed | verdict |
|---|---|---|---|
| `TX:MICPEAK` | −20 dBFS tone | **−19.977 dBFS** (+0.023 dB) | **CERTIFIED** — and again at −10 dBFS: −9.969 |
| `TX:SWR` | tone into a dummy load | **1.0**, age 43–531 ms | **CERTIFIED** — a flat load reads flat |
| `TX:SWR` idle | unkeyed | **absent** (null, age −1) | **CERTIFIED** — the `kMinForwardCountsForSwr` gate does its job; no 255.99:1 |
| `TX:REFPWR` | 5.57 W forward into a dummy load | **0.001 W**, ≈37 dB below forward | **CERTIFIED** — requirement is ≥15 dB |
| `RAD:PATEMP` | 10 s key | 32.70 → **43.06 °C** (+10.36) | **CERTIFIED** — requirement is a ≥0.5 °C rise |
| `TX:ALC` | tone swept −10 → −30 dBFS | **−1.41 dBFS at every level** | **CERTIFIED** — see the normalisation check below |
| `TX:COMPPEAK` | PROC off | **0 dB**, age 0–28 ms | **LIVE** — reads zero with the compressor off, which is correct |
| `SLC:LEVEL` | receiving | −105.7 dBm, age 15–78 ms | **LIVE** |
| `RAD:+13.8A` | — | not defined | **correct** — the HL2 reports no supply voltage |
| mic gain control | 100 → 50 | **6.023 dB** | **CERTIFIED** — arithmetic says 6.02 |

**The ALC normalisation check, and why it certifies the whole host TX chain.**
A post-ALC peak meter has a known answer available without any calibration: it
must be **constant** while its input is not. Sweeping the injected tone over
20 dB, at fixed drive:

| Injected tone | `TX:MICPEAK` | `TX:ALC` | `TX:FWDPWR` |
|---|---|---|---|
| −10 dBFS | −9.98 dBFS | **−1.41 dBFS** | 1.988 W |
| −20 dBFS | −19.98 dBFS | **−1.41 dBFS** | 1.999 W |
| −30 dBFS | −30.00 dBFS | **−1.41 dBFS** | 2.000 W |

The pre-ALC meter tracks the input to within 0.02 dB and the post-ALC meter does
not move at all — which is the ALC doing its job, measured rather than assumed.
Constant forward power across the same sweep says the transmitted level is
**drive-limited, not audio-limited** on this radio, so an operator's mic gain
cannot change their output power; only the drive control can.

Because these two meters sit on opposite sides of the ALC, agreeing with each
other in opposite directions, they certify the host transmit path between them:
test tone → mic gain → ALC → modulator → IQ on the wire → PA → 2 W into a load.
This is the HL2 analogue of the IC-705's live-voice run below, with a synthetic
stimulus instead of speech — weaker as an audio-quality check and stronger as a
level check, since the tone's amplitude is exactly known.

`TX:FWDPWR` is **live and uncertified**, and the distinction is the point.
It reads 5.57 W at full drive and 1.17 W at 25 %, so it plainly tracks drive.
But the certification stimulus in the table above — halve the control, expect
−6.02 dB — cannot be run on this radio:

| Change | Expected | Measured |
|---|---|---|
| 100 % → 50 % | −6.02 dB | **−4.44 dB** |
| 50 % → 25 % | −6.02 dB | **−2.33 dB** |

Two independent reasons, and neither is a fault in the control:

1. **Halving the slider does not halve the drive.** The gateware decodes only
   the drive register's top nibble, so the slider's 101 positions are 16 radio
   states and 100→50→25 % is nibble 15→7→3 (`HERMES.md` 17.7).
2. **The instrument is uncalibrated by construction.** Forward power comes from
   Quisk's `HL2FilterE3` *reference* curve, which is a different board's
   calibration (`HERMES.md` 17.5).

So a failing delta here cannot be attributed to the control rather than to the
curve, and **`radiocert` must not report one as a control defect.** Certifying
drive by effect on this radio needs either a per-unit power calibration or an
external power meter; until then the honest claim is "monotonic in drive",
which is what the nibble sweep in `HERMES.md` 17.7 shows.

### Icom (IC-705) — measured with `controls meters`, radio idle on 20 m

Ages are from one live run; the point is the STATUS column, not the numbers.

| source:name | CI-V | unit | range | poll | Status |
|---|---|---|---|---|---|
| `SLC:LEVEL` | `15 02` | dBm | −140…−10 | 100 ms | **LIVE** (150 ms) |
| `RAD:+13.8A` | `15 15` | Volts | 0…16 | 1000 ms | **LIVE** (801 ms) |
| `RAD:OVF` | `15 07` | Percent | 0…1 | 500 ms | **LIVE** (67 ms) — was `NEVER FED`; see the one-byte decode above |
| `TX:FWDPWR` | `15 11` | Watts | 0…12 | 200 ms | LIVE when keyed; reads 0 with no modulation |
| `TX:SWR` | `15 12` | SWR | 1…6.4 | 200 ms | IDLE (transmit-only) |
| `TX:ALC` | `15 13` | Percent | 0…100 | 200 ms | IDLE (transmit-only) |
| `TX:COMPPEAK` | `15 14` | dB | 0…25.5 | 200 ms | IDLE (transmit-only) |
| `RAD:PACURRENT` | `15 16` | Amps | 0…4 | 500 ms | IDLE (transmit-only) |

### Certified by effect, 2026-08-06 (IC-705, 7.200 MHz LSB, 10 W dummy load)

**Every transmit meter, against live voice — and the whole host transmit path
with them.** The stimulus was an operator speaking into a Bluetooth headset
paired to **the Mac running AetherSDR**, selected as AetherSDR's TX input. The
radio's own microphone was not involved at any point, and the audio travelled
the full host chain:

    BT headset -> macOS input -> AetherSDR TX capture -> TX DSP -> submitTxAudio
                -> Icom LPCM UDP audio stream -> WLAN modulator -> RF

`MOD Input` was `WLAN voice / WLAN data` throughout, which is what that path
requires: the radio modulates from the network because the network is where the
audio came from.

**Confirmed on an unrelated receiver.** A Kenwood TH-D75 sitting beside the
IC-705 heard the transmission off air. That matters more than every internal
measurement above put together — this document's closing section says a
self-check "demodulates our own transmission, which is a different path from the
panadapter but still our own code. Two errors in the same direction agree." A
separate radio from a different manufacturer shares none of that code and can
agree with none of those errors. It is the strongest evidence available without
a lab, and it was one HT on a desk.

| meter | stimulus | observed | verdict |
|---|---|---|---|
| `TX:FWDPWR` | voice | 0 -> 2 -> **5 W** -> 1 -> 0, tracking syllables | **CERTIFIED** — and the unit contract with it: 5 W arrived as 5 W, not as `10^(5/10)/1000` |
| `TX:SWR` | voice into a dummy load | **1.0**, steady | **CERTIFIED** — a flat load reads flat, which is the one SWR value that can be checked against a known answer |
| `TX:ALC` | voice | 0 -> 73 -> 96 -> **98 %** -> 67 | **CERTIFIED** — tracks the envelope |
| `TX:COMPPEAK` | voice, PROC on | **8.9 dB** | **CERTIFIED** — reads zero with the compressor off |
| `RAD:PACURRENT` | voice | 0.25 -> **1.40 A** | **CERTIFIED** — the PA draws under modulation |
| `RAD:+13.8A` | voice | 7.98 -> **7.58 V** | **CERTIFIED** — the supply sags under PA load and recovers |

All six were `IDLE` and unproven before this. Keying alone is **not** the
stimulus: an earlier run keyed with no modulation and read `FWDPWR 0 W`,
`ALC 0 %`, `SWR null` while the PA drew 0.43 A — proof that the radio was
transmitting and nothing more. **Voice is the stimulus**; a carrier would
certify power and SWR but leaves ALC and COMPPEAK unexercised, because neither
means anything without an envelope.

> **This certifies the HOST TRANSMIT PATH, not just the meters.** Five watts of
> RF from a voice spoken into a Mac audio device is an end-to-end proof of the
> capture, the TX DSP chain, the encoder, the UDP audio stream and the radio's
> modulator — every stage between a microphone and an antenna.
>
> **Do not diagnose this path with `opusTxPacing`.** An earlier run read
> `opusTxPacing.packetsSent: 0` and concluded the transmit path was dead. That
> counter belongs to the AudioEngine's **Opus** transport (KiwiSDR / WebSocket).
> An Icom negotiates `Lpcm1ch16` and carries it on its own UDP audio stream, so
> the counter was never going to move no matter how well transmit was working.
> Reading a neighbouring subsystem's counter and believing it is the same class
> of error as trusting a meter that is defined but never fed.

The remaining rows in the original idle snapshot were not certified by that
snapshot. The keyed evidence above resolved the earlier `TX:FWDPWR` and
`TX:ALC` unit-contract defect; Appendix C of the Icom design doc retains the
failure history because it is the reusable lesson.

### Certified controls and presentation, 2026-08-13 (IC-7300MK2, CI-V B6)

This pass followed protocol bytes through the model into the actual widget and
then repeated the state checks after a complete AetherSDR restart. The operator
subsequently completed the remaining manual surface checks and confirmed the
controls behaved correctly.

| Surface | Wire/live evidence | Product evidence |
|---|---|---|
| RF Power 7 % | `14 0A 00 18` (raw 18) | model and actual slider 7 before and after restart |
| Mic Gain 10 % | `14 0B 00 26` (raw 26) | model and actual slider 10 before and after restart |
| Monitor Level 10 % | `14 15 00 26` (raw 26) | model and actual slider 10 before and after restart |
| ATU successful → bypass | click emitted/read back `1C 01 00` | button changed to Bypass without keying |
| NR/NB external changes | raw `16 40` / `16 22`, then periodic replies at about 3 s | model and actual DSP buttons followed on/off |
| Forward power at idle | backend retained the last 16 W sample after emergency unkey | actual power gauge cleared to 0 immediately and stayed 0 after late replies |

The 0–255 control cases establish the radio's integer display contract: decode
with floor and encode with ceiling. They do **not** establish a generic meter
scale; power and other `15 xx` meters remain model-calibrated.

The 16 W sample was also a safety finding. Both an ATU cycle and two-tone at a
10 percent Tune Power setting exceeded the authorized 10 W according to the
radio's calibrated CI-V Po meter, and the run unkeyed immediately. It proves
that a percentage ceiling is not a watt ceiling and that two-tone's drive comes
from Tune Power, not RF Power.

### Non-meter telemetry that still needs surfacing

| Signal | HL2 source | Why it matters |
|---|---|---|
| ADC overload | `0x00[24]` | clipping the converter; invisible in any audio meter |
| ADC clip count | discovery `0x1B[1:0]` | saturating counter — "did we clip at all recently" |
| TX IQ FIFO depth | RADDR `0x00` | the oracle calls it the most important number in the protocol |
| TX inhibit | `0x00[25]`, **active low** | the radio refusing to key, distinct from us not asking |

---

## Certifying a whole radio at once — the control registry

The tables in this document are hand-maintained, and hand-maintained tables go
stale silently. The Icom bring-up produced a second instrument that does not:
`IcomControls.h` declares every CI-V message the backend names, and the bridge's
`controls` verb reports it joined against what the running backend has actually
observed. Three things it does that a table cannot:

**It separates four wiring states, not two.** `both`, `send-only`,
`decode-only`, `declared-only`. A control that is read but never written and one
that is written but never read are different bugs with different symptoms — the
first is a dead slider, the second is a control that opens at *our* default on a
radio set to something else — and "not working" hides both. Seven Icom constants
turned out to be `declared-only`: named in the codec, reached by nothing.

**It separates declared from observed.** Every row carries `sentThisSession` and
`seenThisSession`. A row claiming `both` that has never been seen after a full
connect is the interesting case, and it is the one no document can report.

**It answers for all of them at once.** `controls scrub` drives every settable
control through its seam verb *at its current value* and verifies scheduler
admission or immediate wire dispatch. Nothing on the radio moves. Poll
`civ scheduler` until `idle:true` with no new timeout to complete the
dispatch/readback proof. On the IC-705 that is 26 controls in one call: 18
linked, 0 broken, 8 that cannot be re-asserted without changing an operator
setting.

Three design choices in the scrub are worth copying into any backend that grows
one:

- **Three outcomes, not two.** `NOT-TESTED` is a real state — a control the
  harness could not drive safely — and collapsing it into pass or fail
  misreports it. Eight of the IC-705's rows land there and none of them is a
  fault.
- **Defeat the dedupe first.** NR, NB and both notches suppress an enable that
  matches the last one sent. Correct in normal use; fatal to a linkage check,
  because re-asserting the current value is exactly what the dedupe swallows.
  Clearing the sentinel makes the verb send the same value it would have sent
  anyway, so the radio still does not move and the frame becomes observable.
- **Never scrub what transmits.** PTT, the antenna tuner and power-off are
  excluded outright. A scrub that has to be supervised is a scrub nobody runs.

### Meters: age is the certification, not the definition

Rule 2 at the top of this document says a meter that is defined and never fed is
worse than a missing one. `controls meters` is that rule made runnable: it
reports every meter's scale, its poll interval, and **how long ago it last
produced a reading**, with four statuses —

| Status | Means |
|---|---|
| `LIVE` | a reading arrived within a few poll intervals |
| `STALE` | far older than its own interval — it was working and stopped |
| `IDLE` | transmit-only and correctly quiet while receiving |
| `NEVER FED` | defined, and no reading has *ever* arrived |

The distinction between `IDLE` and `NEVER FED` is the whole point. Both read as
"no value" on a gauge, and only one is a bug.

It found one on its first run. `RAD:OVF` reported `NEVER FED` while `civ trace`
plainly showed `15 07` replies arriving twice a second: **the ADC-overflow reply
is one byte, not the two-byte BCD level every other `15 xx` uses**, so
`decodeLevel` rejected it before `markAnswered`, the poller re-asked on the
in-flight timeout forever, and the indicator that tells an operator they are
clipping the converter never moved once. A definition-based audit would have
passed it — the meter was defined, mapped, polled and published.

### What the harness still cannot tell you

- **That the radio obeyed.** Scrub plus a clean scheduler drain proves an intent
  reached the wire and the radio answered its transaction. An `FB` means
  "understood", not "applied" — an IC-705
  answers `FB` to a P.AMP2 request above 50 MHz that it then ignores.
- **That the value is right.** A control can be linked, in range, and scaled
  wrongly. `14 02` published as decibels instead of percent would still scrub
  green; only reading the unit against the model's guide catches that.
- **That the UI reaches the seam.** `uiTarget` is a claim about which widget
  drives a control, and nothing checks it. Driving the widget and watching the
  wire — which is what the bridge's `invoke` plus `civ trace` do together — is
  still a per-control test.

---

## Controls

**Every control is certified by its effect, never by readback.** A slider that
reports the value it was given proves only that the model has a variable.

The **Certified** column says whether `radiocert meters` actually exercises the
control today. A row that cannot be run is marked as such rather than quietly
omitted — an uncertified control and a certified one must never look alike in
this table, which is the same rule the report itself follows.

| Control | Range | HL2 path | Observable effect | Tolerance | Certified |
|---|---|---|---|---|---|
| `TransmitModel::setRfPower` | 0–100 | drive register, **top nibble only** | one nibble up → `TX:FWDPWR` rises | monotonic; see below | **partly — monotonic proved, ≈6 dB not applicable** |
| `TransmitModel::setRfPower(0)` | — | disables the PA | forward power to the floor | — | **yes — 0 % reads the 0.001 W floor** |
| `AudioEngine::setPcMicGain` | 0–100 | host-side, pre-modulator | halve → `TX:MICPEAK` drops ≈6 dB | ±1 dB | **yes — 6.023 dB measured** |
| `SliceModel::setAgcThreshold` | 0–100 | WDSP `SetRXAAGCTop` | raise → audio floor rises | ±3 dB | no |
| `IRadioBackend::setPanRfGain` | −8…+32 dB | AD9866 LNA `0x0a[5:0]`, at runtime | step the gain → the pan echoes the value the hardware took | echo must equal the request | **yes — `control-effect` phase** |
| `SliceModel::setRfGain` | dB | **dead — Flex wire text** | none; nothing on a non-Flex backend receives it | — | n/a — the operator's slider does not use it |
| `TransmitModel::setTunePower` | 0–100 | **NOT WIRED** | tune uses full drive | — | n/a |
| `SliceModel::setSquelch` | on/off | **NOT WIRED** | — | — | n/a |
| `setFilter(low, high)` | Hz | WDSP passband | tone outside the passband is rejected | ≥30 dB | no |
| `setMode` | enum | WDSP mode + passband | sideband flips; passband follows the mode | — | partly — `rx` phase |

### Gaps this table makes visible

- **The RF power rows are runnable but not conclusive.** They were listed as
  unrunnable because `TX:FWDPWR` was defined-but-never-fed. It is fed, and a
  sweep runs — but neither premise of the ≈6 dB criterion holds on this radio
  (the control is 16-step, the instrument is another board's calibration), so a
  failing delta cannot separate a control fault from a curve error. See the note
  under the transmit table. `radiocert` still reports `rfPowerExercised: false`,
  which remains the right answer: the verb does not drive the slider, and the
  sweep that produced these numbers was manual.
- **`SliceModel::setRfGain` is a dead end, and the RF Gain slider does not use
  it.** The setter's whole body is `slice set N rfgain=X`, Flex wire text no
  seam backend can receive — but `SpectrumOverlayMenu` emits `rfGainChanged`
  unconditionally and only falls back to the slice setter when there is no
  radio model or no pan id. On the HL2 the slider therefore routes
  `rfGainChanged` → `RadioModel::setPanRfGainFor` → `Hl2Backend::setPanRfGain`
  → `applyLnaGainDb` → `MetisClient::setLnaGainDb`, writing AD9866
  `0x0a[5:0]` at runtime. `radiocert` used to assert the opposite as a
  hardcoded, family-gated finding on every HL2 run; it now MEASURES the
  control instead, which is what let the gate go (CERTIFICATION.md 1.14, 1.40).
- **The RF gain check's verdict rests on the echo, and its S-meter delta is
  evidence only.** The tempting expectation — an 8 dB LNA step must move
  `SLC:LEVEL` by 8 dB — is wrong twice over. `Hl2DbReference` is moved in the
  same call as the gain and both the spectrum and the S-meter render through it,
  precisely so a gain change does **not** slide the display (`HERMES.md` 17.4),
  so the expected delta is **0 dB**, not the step size. And a reading near
  −step has two causes this measurement cannot separate: the register never took
  the value, or the reading is dominated by converter noise that does not rise
  with the gain. Measured on a quiet 20 m, an 8 dB step moved `SLC:LEVEL` by
  −6.3 dB — within 2 dB of the defect signature — while the raw dBFS behind it
  *rose* 1.74 dB, proving the register **had** been written. So the stage
  publishes both hypotheses (`sLevelExpectedDeltaDb`,
  `sLevelDeltaIfRegisterNotWrittenDb`), marks the delta
  `sLevelDeltaIsConclusive: false`, and raises its one concern on a missing
  echo. Closing the effect half needs the raw pre-reference dBFS, which the seam
  does not expose (CERTIFICATION.md 2.4).
- **`TX:FWDPWR` and `TX:REFPWR` are published and uncalibrated**, not absent.
  They read in dBm through a reference curve for a different board. The gap is
  a per-unit calibration, not a missing meter.
- **`TX:ALC` is consumed.** `MeterModel::swAlc()` carries it to the Phone/CW
  applet's ALC gauges. It was computed and discarded for a while, and the note
  saying so outlived the wiring.
- **Tune power is not separable from transmit power.** TUNE keys at whatever
  drive is set, which on a fresh connect is the operator's full RF power.

---

## Remote-control convergence checklist

Treat each operator control as four paths, not one:

| Path | Evidence required |
|---|---|
| Set | actual widget → model intent → backend verb → expected protocol bytes |
| Same-session reply | radio response → model value → actual widget value |
| External change | front panel or safe raw command → periodic poll/unsolicited frame → widget |
| Restart | close the application, reconnect, and adopt radio state without replaying a client default |

For Icom, connect-time reads plus CI-V Transceive are insufficient. Transceive
is not a complete subscription, so readable NR/NB functions and levels, RF and
mic controls, monitor, VOX, notch, preamp, attenuator, and tuner state need a
bounded periodic poll. Allow at least two poll periods before declaring an
external-change failure.

DATA mode needs its own row in that table, because it is invisible in the
command every other mode check uses. Commands `01`, `04` and `06` carry one
mode byte, and USB and USB-D share it — a certification pass that reads back
`04` and sees `01` cannot tell them apart, and will pass a client that never
sent DATA at all. Command `26` carries mode, DATA state and filter slot for the
selected VFO together, so certify against `26 00` in both directions and verify
DATA on the radio's own display, not only in AetherSDR's mode text. The
same-frame property is the point: a filter change that goes out as `06` clears
DATA on the radio, so "changed the IF filter, still in USB-D" is a required
check, not an incidental one.

A send-only control is different. The IC-7300MK2 RX-ANT command is shown
optimistically after an operator click because live firmware acknowledges the
documented read form with bare `FB` instead of returning state. Certification
must not call that a subscription or a restart-persistence pass: reconnect may
list ANT1/RX-ANT, but it must neither claim a selection nor replay client state.

For 0000–0255 percentage controls, compute expected wire values independently
using the radio display contract: write `ceil(percent * 255 / 100)`, read
`floor(raw * 100 / 255)`. Do not reuse that conversion for meters. Meter values
use published, often model-specific calibration curves.

### Transmit safety and presentation gate

Before any automated key:

1. require explicit authorization naming the exact dummy-load antenna port and
   physical watt limit;
2. assert that exact live port from `dumpTree`; unreadable is failure;
3. require tuner bypass unless an ATU cycle is separately authorized;
4. stage and verify both RF Power and Tune Power — two-tone uses Tune Power;
5. apply the bridge percentage ceiling, but never describe it as watts;
6. require a fresh calibrated forward-power sample and force-unkey on an
   over-limit value, missing telemetry, high SWR, timeout, or disconnect; and
7. confirm radio/model transmitting false and the visible power gauge zero.

The watt watchdog is reactive. If even a brief overshoot is unacceptable, use
an external interlock or do not run unattended. Never use an ATU tune cycle to
restore state after a test unless its drive is inside the authorization.

For TX meters, certify the presentation lifetime as well as calibration:
startup idle = zero, active key = live value, immediate unkey = zero, and late
post-unkey reply = still zero. A backend retaining the last sample for
diagnostics is acceptable; a visible gauge retaining it is not.

---

## Bring-up order

Each phase depends only on the ones before it. Run them in order; a failure in
an early phase makes every later result meaningless rather than merely wrong.

| Phase | Depends on | Establishes |
|---|---|---|
| `tune` | nothing | the dial goes where it is told; every mode maps |
| `rx` | tune | wire handedness, sideband correctness, passband follows mode |
| `tx` | rx | keying, modulation, the transmitted sideband |
| `meters` | tx | the instruments themselves, against known stimuli |

**Meters last, deliberately.** They are not trustworthy until something has
checked them, so no earlier phase may draw a conclusion from one. A transmit
stage that reports "no RF" because SWR is missing is really reporting "no SWR
reading" — the same statement only after this phase has run.

## What certification still cannot tell you

Kept here rather than omitted, because omitting it is how a wrong-sideband
transmitter passed every check it had:

- **Sideband against an unrelated receiver.** The self-check demodulates our own
  transmission, which is a different path from the panadapter but still our own
  code. Two errors in the same direction agree.
- **Audio quality.** Level and frequency can be perfect while the audio is
  clipped or unintelligible.
- **Occupied bandwidth, harmonics, IMD.** The receive window is tens of kHz wide
  and centred on the transmit frequency; it cannot see a harmonic by construction.
- **Absolute power in watts.** Uncalibrated counts must not be dressed up as
  watts.
