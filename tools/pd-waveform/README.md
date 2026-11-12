# AetherPD: a pre-distortion waveform (prototype)

**AetherPD** (PD = pre-distortion) brings WDSP's PureSignal-style
pre-distortion to FlexRadio radios.

A FLEX-6000 has no internal sampler and no Adaptive Predistortion. This is a
legacy waveform that brings WDSP's PureSignal to one: it pre-distorts the
transmit I/Q it hands the radio, and learns the correction from an external
coupler sample of the amplifier output.

It has to fit a FLEX-6000 legacy waveform: a static ARMv7 (Cortex-A8)
binary on a small, shared `/user` partition. So it carries **only**
PureSignal's own WDSP files (`calcc`, `iqc`, `delay`, the NURBS/spline
fitter, `fir`, `utilities`, `impulse_cache`) with WDSP's POSIX port, and no
FFTW library and no WDSP transmit channel. `pd_core.c` supplies the one
thing those files need from the channel: the `txa[]`/`ch[]` slot holding
their pointers and locks.

| File | What it does |
|---|---|
| `pd_core.[ch]` | Wraps WDSP's calibration (`pscc`) and correction (`xiqc`) |
| `pd_align.[ch]` | Lines the feedback up with the reference: loop delay to a hundredth of a sample, and a slow tracker for the NCO offset between them |
| `pd_waveform.c` | The waveform: radio API, VITA-49, USB modulator and demodulator, DAX IQ feedback; `selftest` is the CTest `pd_waveform_selftest`, which also runs the whole transmit loop through the waveform against an amplifier model |
| `pd_sim.c` | Closed loop against a PA model; the CTests `pd_waveform_sim_test` and `pd_waveform_sim_bandwidth_limit_test` |
| `build.py`, `package/` | Builds `AetherPD.ssdr_waveform` with an ARMv7 (FLEX-6000) and an arm64 (FLEX-8000) binary |

## What the simulation shows

Two-tone at +-700 Hz, 24 kHz complex (the waveform API's only rate), a
memoryless PA with AM/AM and AM/PM, the radio's TX filter modeled after
the correction, and the reference taken before that filter (all a
waveform knows).

| Case | IMD3 |
|---|---|
| No filter, whole-sample delay | -29.3 -> -70.3 dBc |
| +-5 kHz filter, delay +0.37 sample, NCO offset 2 Hz | -29.3 -> -68.9 dBc |
| +-1.5 kHz filter | no improvement: IMD3 (+-2.1 kHz) is outside the filter |

Two things the radio path needs that ANAN hardware gives for free:

- **Sub-sample delay.** WDSP rejects every fit when the pairs are off by a
  quarter sample at 24 kHz. `pd_align` finds the delay to ~0.03 sample by
  envelope correlation (immune to the NCO offset) and interpolates the
  reference.
- **NCO offset.** The feedback comes from a separate receiver, so it rotates
  against the reference. A ~1 Hz phase tracker removes 50 mHz to 2 Hz
  offsets without tracking away the amplifier's AM/PM.

One correction: WDSP's `xiqc` indexes its curves by amplitude on a scale
where 1.0 is the transmit peak, so `pd_core_correct` scales into that range
and back.

**Size and speed.** The ARMv7 static build of the core plus the simulator is
168 KB. One fit takes ~135 ms on a desktop core (i9-13980HX); expect 2-4 s
on a FLEX-6000's Cortex-A8, which is still usable (fits run in the
background while transmitting). Measure it on a real radio.

## The waveform

`AetherPD` registers the mode **PD** with the `RAW` underlying mode, so the
radio exchanges 24 kHz I/Q with it. With PD selected on a slice:

- **Receive:** the slice's I/Q is demodulated as USB and returned as audio.
- **Transmit:** mic audio becomes upper-sideband I/Q (200-2800 Hz), is
  pre-distorted by `pd_core`, and goes back to the radio.
- **Feedback:** while the radio transmits, a DAX IQ stream from a panadapter
  on the coupler's receive input is mixed to the transmit frequency,
  decimated to 24 kHz, and lined up with what was sent. The loop delay is
  found again on each transmission (the first ~0.75 s), then the
  calibration runs. If the pairs stop matching (coherence under 0.3 for 8
  blocks), it finds the delay again rather than calibrating on bad pairs.

It never keys the radio. It only answers the radio's own transmit stream.

Build: `python3 -I build.py --output OUT --host` (zig 0.17). The package is
~200 KB with both binaries.

### Setup on the radio

1. An external directional coupler and attenuator from the antenna output
   into the transmit side's receive input (RX A for ANT1), at -20 dBm or
   lower there. Turn on full duplex.
2. A panadapter on that input, centred on the transmit frequency, with DAX
   IQ channel 1 (or set `AETHER_PD_DAXIQ` in `launch.sh`).
3. Install `AetherPD.ssdr_waveform`; select PD on the transmit slice.
4. On a FLEX-8000, turn APD off so the two corrections don't fight.

The log shows the registration reply, the loop delay it found, and the fit
count while transmitting.

## Testing on a radio, in order

1. **No RF.** Install; select PD; check the registration reply has stream
   IDs and that receive audio comes through. This settles whether `RAW`
   exists on that firmware.
2. **Dummy load, low power, correction off** (`AETHER_PD_CORRECT=0`): a
   two-tone from the mic input. Check the transmitted spectrum, how wide the
   radio's transmit path is after the waveform, and that the log finds a
   loop delay.
3. **Correction on**: compare IMD3 before and after at several powers.

Every transmit is keyed by a person, into a dummy load. Nothing here keys
the radio.

## On a FLEX-8600 (SmartSDR 4.3.16), no RF, 2026-11-11

- The waveform starts as a legacy waveform, connects, and registers: PD
  appears in every slice's `mode_list` within a second of installing.
- A slice set to the waveform's mode reports `chain_mode=RAW filter_lo=300
  filter_hi=3000`, so the RAW I/Q is cut to the slice's one-sided filter.
- With PD selected, DAX audio from that slice flows, and the waveform is
  still registered afterwards.
- `filt <slice> -3000 3000` (and wider) is accepted with `R|0` on a PD slice,
  but no status reports new edges, so whether a RAW slice takes a symmetric
  or wider filter is still open.
- The radio's transmit filter reads `lo=50 hi=4000`. If the RAW transmit
  path is cut to that one-sided band, only the products above the carrier
  inside it can be corrected; the first transmit test settles it.

## On the FLEX-8600 bench, 2026-11-11, before the transmit runs

- **DAX IQ samples are little-endian floats** (headers big-endian), unlike
  the waveform's own audio streams. The feedback path reads them that way;
  the loop selftest sends them that way, and fails if they are read
  big-endian.
- **Transmit settings belong to each client.** `transmit set rfpower=`
  from a second client is acknowledged and changes only that client's
  copy; the transmitting client (the operator's GUI) sets power, DAX TX
  and the like.
- **RX A at 5 W on 40 m**, RF-S2K+ knob counter-clockwise, preamp off,
  measured on DAX IQ: radio leakage with RX A terminated +53.3 dB, the
  sample +92.8 dB, noise about -8 dB on the same scale. The sample is
  39.5 dB above the leakage. With the VNA-measured coupling that puts the
  sample near -31 dBm and the leakage near -70 dBm.

## First transmit runs, FLEX-8600, 2026-11-11 (40 m, 5 W, dummy load)

Keyed through the operator's AetherSDR automation bridge (its DAX TX audio
is only forwarded when the app itself holds the transmit intent). Measured on
a second RX A panadapter with its own DAX IQ channel: **the radio deactivates
an older stream when another client opens the same DAX IQ channel**, so the
meter must never share the waveform's channel.

| Run | Signal | IMD3 | IMD5 | IMD7 |
|---|---|---|---|---|
| 1 | radio two-tone (710/1900 Hz), USB | -27.4 dBc | -41.0 dBc | -52.2 dBc |
| 2 | AetherPD two-tone, correction off | -27.9 dBc | -41.8 dBc | -53.6 dBc |

- **RAW transmit works**: the waveform's own I/Q reaches the amplifier
  essentially unchanged (run 2 matches run 1).
- **On the 8600, DAX IQ can't reach an on-radio waveform**: the stream
  request is refused with "Unable to Resolve the MAC address of Client with
  IP 127.0.0.1 using ARP". The runs used the waveform on a LAN host instead.
  Whether a FLEX-6000 delivers DAX IQ to an on-radio waveform is still open.
- **The loop closes**: delay found (2176-2576 samples, about 100 ms from a
  LAN host; it changes per transmission), envelope correlation 0.98, NCO
  offset tracked at about -0.01 Hz, WDSP fitting about 1.6 times a second.
- **Envelope delay is not carrier delay**: the radio's transmit filter put
  them 3.3-4 samples apart (coherence 0.82 instead of 0.999). The aligner
  now refines the delay on carrier phase after removing the NCO offset, and
  waits 1 s after key-up before measuring.
- **Open: a periodic test signal is ambiguous.** A 710/1900 Hz two-tone
  repeats every 100 ms (2400 samples), so every multiple of that lag
  correlates perfectly and the delay keeps being re-found. Next: open each
  test transmission with a noise burst for acquisition, as the simulator
  does, and check for sample slips between a LAN host and the radio.
  (Done: each test transmission now opens with noise.)

## Correction on air, FLEX-8600, 2026-11-11 (40 m, 5 W, dummy load)

**WDSP accepts no fit without a transmit-path fit.** The radio filters the
waveform's I/Q before its amplifier. Below x = 0.1, the reference/feedback
pairs scattered 39 degrees in phase, so every fit failed WDSP's
sin^2+cos^2 check (off by 0.06-0.07; the limit is 0.05). `pd_eq` fits a
31-tap FIR from reference to feedback on the first second after each delay
lock, and calibration sees the reference through it. That brought the
scatter to 6 degrees, and fits are accepted. `pd_waveform_replay` replays
an `AETHER_PD_DUMP` capture through this offline.

**Train it on noise across the correction band.** The fit is only right
where it was excited. With a 300-3000 Hz preamble, the first fit took IMD3
from -29.3 to -50.1 dBc, and later fits drifted it back to -42 dBc. With
the preamble at -4500 to +7500 Hz:

| Run 3f, two-tone 710/1900 Hz | IMD3 lo/hi | IMD5 lo/hi | IMD7 lo/hi |
|---|---|---|---|
| Uncorrected | -31.0 / -30.5 | -44.3 / -44.7 | -56.0 / -56.1 dBc |
| First fit | -44.5 / -58.1 | -57.4 / -61.8 | -74.2 / -66.0 dBc |
| After 17 s | -48.0 / -51.5 | -59.8 / -61.4 | -72.7 / -69.5 dBc |

At higher power (40 m, 17 s runs with correction on; the uncorrected
baselines are the radio's own 710/1900 Hz two-tone at the same level):

| Power | IMD3 lo/hi | IMD5 lo/hi | IMD7 lo/hi |
|---|---|---|---|
| 10 W uncorrected | -27.5 / -27.0 | -39.2 / -39.3 | -48.2 / -48.2 dBc |
| 10 W corrected, after 17 s | -48.0 / -49.1 | -54.3 / -55.8 | -69.4 / -62.8 dBc |
| 25 W uncorrected | -27.7 / -27.2 | -37.6 / -37.7 | -45.4 / -45.4 dBc |
| 25 W corrected, after 17 s | -47.8 / -48.4 | -55.4 / -55.5 | -63.0 / -61.2 dBc |

That is about 21 dB on IMD3 and 15-18 dB on IMD5 and IMD7, and still
improving when each run ended. The loop delay is found on noise; the first
lock was wrong on three runs out of three (coherence 0.01-0.03) and was
found again within 2 s.

The transmit-path fit still left -10.6 dB (5 W), -8.7 dB (10 W) and
-17.9 dB (25 W) of the feedback unexplained.
Something in its training data isn't linear, and the
cause is open. On speech it would train on 200-2800 Hz only, the same gap
that caused the drift. Voice use needs a fit regularized toward a pure
delay, or a stored measurement of the radio's path.

**A waveform must stop sending at UNKEY_REQUESTED.** Until this was fixed,
the waveform answered every transmit packet whatever the interlock state.
On every key-down it held the interlock in UNKEY_REQUESTED for 28-531 s,
with the two-tone still on the air. `xmit 0` and the client's PTT off were
both accepted and changed nothing; only taking the slice out of PD
released it. Now it goes silent, as the waveform-sdk example does, and
the radio reaches READY 0.1 s after unkey. Bench keying checks for READY
after every key-down.

## Not yet known

- Whether the radio's transmit filter or ALC still acts on a `RAW`
  waveform's output, how wide that path is, and whether it can be made
  symmetric about the carrier.
- Whether DAX IQ reaches an on-radio process (it doesn't reach a container
  on the FLEX-8600) and whether a FLEX-6000 with one SCU can monitor its
  own transmit frequency in full duplex.
- The full-scale level of `RAW` transmit samples (`AETHER_PD_PEAK`).
- How long a fit takes on a FLEX-6000's Cortex-A8.
