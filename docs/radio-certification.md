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
| `TX:FWDPWR` | dBm | counts only | rises with drive | halve RF power → drops ≈6 dB | ±3 dB |
| `TX:REFPWR` | dBm | counts only | ≪ forward into a load | key into a dummy load | ≥15 dB below forward |
| `TX:ALC` | dB | **host-side** | gain the ALC applies | quiet input → gain rises | ±3 dB |
| `TX:COMPPEAK` | dB | host-side | compression applied | — | not yet wired |
| `TX:MIC` | dBFS | host-side | pre-gain mic level | — | not yet wired |
| `TX:HWALC` | dBFS | **no** | — | Flex RCA jack; no HL2 equivalent | — |

### Radio / hardware

| source:name | unit | HL2 | Expectation | Certification stimulus | Tolerance |
|---|---|---|---|---|---|
| `RAD:PATEMP` | degC | yes | 20–60 idle | key 10 s → **rises ≥0.5 °C** | rise is the check, not the value |
| `RAD:+13.8A` | Volts | **no** | — | HL2 reports no supply voltage | — |
| `AMP:*`, `TGXL:*` | — | **no** | — | external amp / tuner only | — |

### Non-meter telemetry that still needs surfacing

| Signal | HL2 source | Why it matters |
|---|---|---|
| ADC overload | `0x00[24]` | clipping the converter; invisible in any audio meter |
| ADC clip count | discovery `0x1B[1:0]` | saturating counter — "did we clip at all recently" |
| TX IQ FIFO depth | RADDR `0x00` | the oracle calls it the most important number in the protocol |
| TX inhibit | `0x00[25]`, **active low** | the radio refusing to key, distinct from us not asking |

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
| `TransmitModel::setRfPower` | 0–100 | drive `0x09[31:28]` + PA enable | halve → `TX:FWDPWR` drops ≈6 dB | ±3 dB | **no — unrunnable** |
| `TransmitModel::setRfPower(0)` | — | disables the PA | forward power to the floor | — | **no — unrunnable** |
| `AudioEngine::setPcMicGain` | 0–100 | host-side, pre-modulator | halve → `TX:MICPEAK` drops ≈6 dB | ±1 dB | **yes — 6.023 dB measured** |
| `SliceModel::setAgcThreshold` | 0–100 | WDSP `SetRXAAGCTop` | raise → audio floor rises | ±3 dB | no |
| `SliceModel::setRfGain` | dB | **NOT WIRED** | LNA gain is connect-params only on HL2 | — | n/a |
| `TransmitModel::setTunePower` | 0–100 | **NOT WIRED** | tune uses full drive | — | n/a |
| `SliceModel::setSquelch` | on/off | **NOT WIRED** | — | — | n/a |
| `setFilter(low, high)` | Hz | WDSP passband | tone outside the passband is rejected | ≥30 dB | no |
| `setMode` | enum | WDSP mode + passband | sideband flips; passband follows the mode | — | partly — `rx` phase |

### Gaps this table makes visible

- **The RF power rows are unrunnable, not unimplemented.** Certifying them by
  effect needs `TX:FWDPWR`, which is defined-but-never-fed on this backend (see
  below). Until a power meter is published with a documented scale, the drive
  control cannot be certified by effect on the HL2 at all — so `radiocert`
  reports `rfPowerExercised: false` rather than implying a sweep happened.
- **`SliceModel::setRfGain` has no runtime path.** The AD9866 LNA gain is sent
  once in the connect parameters and never again, so the preamp/attenuator
  control does nothing after connect. It is also the control an operator reaches
  for first when the ADC overloads. `radiocert` only reports this on a radio
  whose `family()` is `hl2` — asserting it generically told Flex operators a
  working control was broken.
- **`TX:FWDPWR` and `TX:REFPWR` are defined but never fed.** The counts are
  uncalibrated, so they are deliberately not published — which leaves two power
  meters on screen that can never move. Either publish with a documented scale
  or stop defining them.
- **`TX:ALC` is computed and thrown away.** `Hl2TxDsp` emits `alcGain` and
  nothing consumes it, while an ALC meter is exactly what tells an operator
  whether their mic gain is sane.
- **Tune power is not separable from transmit power.** TUNE keys at whatever
  drive is set, which on a fresh connect is the operator's full RF power.

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
