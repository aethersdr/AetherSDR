# Fixed-rate receive decoder PCM

`DecoderPcmAdapter` converts typed producer frames into mono float32 at 24,000
samples per second. Each consumer owns an independent adapter. Producer and
speaker-device rates remain independent; no producer configuration changes here.

## Samples and continuity

- Mono24 passes through exactly. Stereo becomes `L / 2 + R / 2`, including at
  24 kHz; this avoids overflowing the addition of two finite floats.
- Input at 48 kHz uses the existing continuous `Resampler`, staged in fixed
  256-frame input batches. There is no finite-tail flush, padding or reuse of
  recording state.
- An accepted empty block still carries a source-end position and can require
  a reset. Filter overflow produces an immediate empty discontinuity; subsequent
  valid audio starts fresh.
- Gaps, explicit discontinuities, format/session/receiver changes and selection
  changes discard filter/staging history. Reset preserves replay cursors and
  live-source pins. A competing live producer cannot replace the selected source.
- Native routes validate slice purpose and ID. A DAX route validates Auxiliary
  purpose; its caller checks the separately supplied channel. Retired receivers
  cannot return through a still-live old producer. Source pins are bounded at 32.
- A `PcmEpochLease` retains only immutable metadata and an atomic revocation
  witness. Converted blocks and queued decoder results check that witness again
  when consumed; they do not retain the original PCM buffer.

## Receive selection

`DecoderAudioModel` binds the selected live slice to its native pre-monitor PCM
or its assigned DAX channel. It acquires a distinct central DAX consumer hold.
Speaker mix, gain and mute do not affect this route. Selection, tuning, mode,
channel, removal and connection changes retire pending input and detector context.
The model has at most 65,536 pending frames in 256 blocks and one scheduled drain.
Overflow discards the backlog and resets the decoder before remaining audio.

RTTY uses this model and keeps its existing decoder worker. Its ring, filters,
bit timing and Baudot shift state reset together. Generation and epoch checks
prevent old queued text or statistics from publishing after reset, stop or source
revocation. Worker teardown joins before destroying decoder state.

AetherClock binds an operator-selected slice through its existing engine and DAX
hold provider. Its production callbacks carry a selection generation; disconnecting
a Qt signal alone would leave previously posted calls deliverable. Converter,
detector, frame/vote/lock and diagnostics history reset together.

CW receive uses the same selected-source model. After route/epoch/replay admission,
`nativePcmReady` publishes the original `PcmFrame` and `pcmReady` publishes the
converted `DecoderPcmBlock`. `CwRxModel` sends only the matching input to its
selected backend: GGMorse receives mono24 blocks, while DeepFist keeps native
24/48 kHz mono/stereo frames and performs its own worker-side 3.2 kHz conversion.
The shared adapter also supplies admission/discontinuity detection on the native
path; its converted samples are unused by DeepFist. No new producer relabels
converted samples, and both paths retain the original revocation witness.

GGMorse owns its engine on the worker and replaces its detector state on input
reset, discontinuity, source change or ring overflow. The ring and each queued
result carry an input generation; publication and estimate getters reject retired
sources. Existing parameter snapshots and locked values survive worker resets.
A stopped GGMorse backend retains its operator locks across neural selection;
only the selected backend runs. Closing the panel releases the CW DAX hold.
TX sidetone remains a separate fixed24 GGMorse instance.

## Clock time mapping

Decoder sample indices always count local 24 kHz mono frames. Let `O` be the
first input sample of the current segment, `E` the original producer frame's
exclusive end, `R` its rate, `D` the converter's group delay in input frames,
and `H` the host arrival time captured before conversion. Decoder sample `s`
maps to:

```text
hostMilliseconds(s) = H - (E - O + D) * 1000 / R + s * 1000 / 24000
```

Subtract integer positions before converting to floating point, and retain
fractional milliseconds until output rounding. Using the original end accounts
for staged input; subtracting the measured converter delay keeps acoustic content
aligned. WWV/WWVB's existing detector-delay compensation remains separate.

Producer frames carry positions, not capture UTC. This preserves an arrival-based
estimate and cannot establish radio, device, network or RF latency.

## Evidence boundary

The converter, routing, RTTY and clock tests feed real production components with
constructed PCM and injected model state. They bind no synthetic firmware peer,
open no sound device and perform no radio/TX operation. Their rate, lifecycle and
timing assertions do not qualify hardware convergence or whole-track acceptance.
