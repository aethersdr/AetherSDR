# AetherSDR patches to RNNoise

The source snapshot is pinned to xiph/rnnoise commit
`70f1d256acd4b34a572f999a05c87bf00b67730d` (see `COMMIT`). AetherSDR carries
one local addition to the otherwise exact upstream snapshot:

1. `src/denoise.c` + `include/rnnoise.h`: `rnnoise_process_frame_with_dry_mix()`
   — denoise a frame while retaining `dry_mix` of the original spectrum.
   `rnnoise_process_frame()` becomes a thin wrapper that passes `dry_mix = 0`,
   and both call a shared `rnnoise_process_frame_impl()`.

## Why it is not done outside the library

RN2 can be configured to leave a constant noise floor under the denoised audio
so the receiver does not go dead between phrases (`Rn2SettingsModel::rxDryMix`,
PR #4689). Blending a dry copy of the *waveform* into RNNoise's output comb
filters, because the wet path has been through an FFT, a 480-sample delay and
an overlap-add that the dry path has not. Blending in the *spectral* domain,
before `frame_synthesis()`, puts both paths on one synthesis timeline and there
is no phase error to hear.

The dry spectrum is snapshotted from `st->delayed_X` **before**
`rnn_pitch_filter()` mutates it, so wet and dry describe the same frame.

## Compatibility

`rnnoise_process_frame()` is bit-identical to upstream: with `dry_mix == 0`
both guarded blocks are skipped and the only added work is a `MIN16`/`MAX16`
clamp of a constant. `tests/rnnoise_filter_test.cpp` asserts this sample for
sample (`testSpectralDryMixUsesOneSynthesisTimeline`), along with the linearity
of the blend between the `dry_mix = 0` and `dry_mix = 1` outputs.

## When refreshing RNNoise

First check whether upstream has gained an equivalent API — if it has, drop
this patch and call theirs. Otherwise reapply only this addition, keeping the
snapshot of every other file exact, and re-run `rnnoise_filter_test` (it fails
loudly if the legacy entry point stops being bit-identical).
