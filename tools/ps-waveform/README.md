# PureSignal predistortion waveform (prototype)

A FLEX-6000 has no internal sampler and no Adaptive Predistortion. This is a
legacy waveform that brings WDSP's PureSignal to one: it pre-distorts the
transmit I/Q it hands the radio, and learns the correction from an external
coupler sample of the amplifier output.

It has to fit a FLEX-6000 legacy waveform: a static ARMv7 (Cortex-A8)
binary on a small, shared `/user` partition. So it carries **only**
PureSignal's own WDSP files (`calcc`, `iqc`, `delay`, the NURBS/spline
fitter, `fir`, `utilities`, `impulse_cache`) with WDSP's POSIX port, and no
FFTW library and no WDSP transmit channel. `ps_core.c` supplies the one
thing those files need from the channel: the `txa[]`/`ch[]` slot holding
their pointers and locks.

| File | What it does |
|---|---|
| `ps_core.[ch]` | Wraps WDSP's calibration (`pscc`) and correction (`xiqc`) |
| `ps_align.[ch]` | Lines the feedback up with the reference: loop delay to a hundredth of a sample, and a slow tracker for the NCO offset between them |
| `ps_waveform.c` | The waveform: radio API, VITA-49, USB modulator and demodulator, DAX IQ feedback; `selftest` is the CTest `ps_waveform_selftest`, which also runs the whole transmit loop through the waveform against an amplifier model |
| `ps_sim.c` | Closed loop against a PA model; the CTests `ps_waveform_sim_test` and `ps_waveform_sim_bandwidth_limit_test` |
| `build.py`, `package/` | Builds `AetherPS.ssdr_waveform` with an ARMv7 (FLEX-6000) and an arm64 (FLEX-8000) binary |

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
  quarter sample at 24 kHz. `ps_align` finds the delay to ~0.03 sample by
  envelope correlation (immune to the NCO offset) and interpolates the
  reference.
- **NCO offset.** The feedback comes from a separate receiver, so it rotates
  against the reference. A ~1 Hz phase tracker removes 50 mHz to 2 Hz
  offsets without tracking away the amplifier's AM/PM.

One correction: WDSP's `xiqc` indexes its curves by amplitude on a scale
where 1.0 is the transmit peak, so `ps_core_correct` scales into that range
and back.

**Size and speed.** The ARMv7 static build of the core plus the simulator is
168 KB. One fit takes ~135 ms on a desktop core (i9-13980HX); expect 2-4 s
on a FLEX-6000's Cortex-A8, which is still usable (fits run in the
background while transmitting). Measure it on a real radio.

## The waveform

`AetherPS` registers the mode **PS** with the `RAW` underlying mode, so the
radio exchanges 24 kHz I/Q with it. With PS selected on a slice:

- **Receive:** the slice's I/Q is demodulated as USB and returned as audio.
- **Transmit:** mic audio becomes upper-sideband I/Q (200-2800 Hz), is
  pre-distorted by `ps_core`, and goes back to the radio.
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
   IQ channel 1 (or set `AETHER_PS_DAXIQ` in `launch.sh`).
3. Install `AetherPS.ssdr_waveform`; select PS on the transmit slice.
4. On a FLEX-8000, turn APD off so the two corrections don't fight.

The log shows the registration reply, the loop delay it found, and the fit
count while transmitting.

## Testing on a radio, in order

1. **No RF.** Install; select PS; check the registration reply has stream
   IDs and that receive audio comes through. This settles whether `RAW`
   exists on that firmware.
2. **Dummy load, low power, correction off** (`AETHER_PS_CORRECT=0`): a
   two-tone from the mic input. Check the transmitted spectrum, how wide the
   radio's transmit path is after the waveform, and that the log finds a
   loop delay.
3. **Correction on**: compare IMD3 before and after at several powers.

Every transmit is keyed by a person, into a dummy load. Nothing here keys
the radio.

## On a FLEX-8600 (SmartSDR 4.3.16), no RF, 2026-11-11

- The waveform starts as a legacy waveform, connects, and registers: PS
  appears in every slice's `mode_list` within a second of installing.
- A slice set to PS reports `mode=PS chain_mode=RAW filter_lo=300
  filter_hi=3000`, so the RAW I/Q is cut to the slice's one-sided filter.
- With PS selected, DAX audio from that slice flows, and the waveform is
  still registered afterwards.
- `filt <slice> -3000 3000` (and wider) is accepted with `R|0` on a PS slice,
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

## Not yet known

- Whether the radio's transmit filter or ALC still acts on a `RAW`
  waveform's output, how wide that path is, and whether it can be made
  symmetric about the carrier.
- Whether DAX IQ reaches an on-radio process (it doesn't reach a container
  on the FLEX-8600) and whether a FLEX-6000 with one SCU can monitor its
  own transmit frequency in full duplex.
- The full-scale level of `RAW` transmit samples (`AETHER_PS_PEAK`).
- How long a fit takes on a FLEX-6000's Cortex-A8.
