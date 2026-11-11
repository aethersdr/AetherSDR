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
| `ps_sim.c` | Closed loop against a PA model; the CTest `ps_waveform_sim_test` |

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

## Next

1. A waveform that registers with `underlying_mode=RAW` and applies
   `ps_core_correct` to its transmit I/Q. The gating question: does `RAW`
   exist and transmit on 6000 firmware, and how wide is the radio's TX path
   after it?
2. Feedback from a DAX-IQ stream on the coupler's receive input during TX
   (full duplex), into `ps_align` + `ps_core_calibrate`.
3. Transmit tests are keyed by a person, into a dummy load, with APD off.
   Nothing here keys the radio.
