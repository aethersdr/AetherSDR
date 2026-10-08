# Optional DeepFist receive decoder

This implementation supplies a shared `CwRxModel` receive-backend interface and
an optional DeepFist backend. ggmorse remains the default receive decoder and
continues to decode transmit sidetone. Only the selected receive backend runs.
The RFC is #4817. DeepCW is the third backend, behind the same interface.

DeepFist consumes selected-slice pre-monitor PCM through `PcmFrame`, resamples
on its worker, and uses the pinned native Lyra frontend. Source changes, discontinuities,
queue overflow, resets and backend switches invalidate previous work. The
queue holds at most two seconds of PCM. Model loading and inference run off
the UI thread; stopping joins the current inference before destroying state.

The single DeepFist choice enables activity normalization, completed-mark
admission and bounded pending-character carry. These are developer settings;
there is no second normalized decoder in the UI. It also lowers the
activity-gate threshold from the stream default 12 to 3 so weak off-air CW is
not gated out (#5950); that value is what every user of this backend runs.
Output has no calibrated
ggmorse cost. Each letter is coloured by the model's own posterior (cost =
1 − posterior, the four ggmorse colour bands) and is never filtered by the
sensitivity slider; pitch and speed are not invented.
DeepFist output does not feed automatic callsign spotting. Other monitored
slices and speaker gain/mute do not alter the selected decoder input. This is
an audio slice tap, not an RF separation claim.

## Distribution

DeepFist builds whenever ONNX Runtime is found (Apple Silicon macOS, Linux and
Windows); there is no separate option. The model is never bundled with the
application. `DEEPFIST_MODEL_BASE_URL` defaults to N9BC's versioned
`exp27_bt-champion` release
(`https://github.com/n9bc/DeepFist/releases/download/exp27_bt-champion/`), whose
`deepfist.onnx` and `deepfist.onnx.json` match the pinned lengths and hashes
below. That release publishes no LICENSE; the manifest's LICENSE asset carries
its own source, the same pinned bytes from the DeepFist repository's first
commit (`061fc1d7`). The RFC's upstream-only hosting decision remains in effect;
there is no AetherSDR mirror. A developer can point `AETHER_DEEPFIST_MODEL_DIR`
at a local verified bundle instead.

The downloader verifies exact lengths and SHA-256 hashes, takes a cache lock,
uses atomic file replacement, and checks the complete bundle before loading.
A populated verified cache works without the source remaining available.
Hashes prevent silently accepting a changed upstream asset but cannot make a
removed asset available to new installations.

| Asset | Bytes | SHA-256 |
|---|---:|---|
| deepfist.onnx | 13051998 | 6d2d4e3d66f9001d15e21a1b38b79150eae19ead86a310202900ee69d672b94d |
| deepfist.onnx.json | 1257 | 840ceb8dba9d46d04495547a8a3789968b1acd2f8ac3a3a5c631f84008ac2217 |
| LICENSE | 1068 | 9ad70a9ed30d58502e29f9e691a008ee7bccb6eba49d4384f2b7e675d68dc4f3 |

The original PR's qualification used a bundle extracted from the Lyra 0.24.0
installer as an archive. It is not committed or bundled into AetherSDR. Native helpers retain
their pinned historical MIT license and attribution under
`third_party/deepfist`; later upstream licensing does not identify the license
of a different checkpoint or future update.

## Validation contract

`cw_rx_model_test` exercises the production receive facade without sockets,
model downloads, audio devices or radio hardware. It also builds when DeepFist
is disabled. The committer and injected model-assets tests also run in the
default build, without ONNX Runtime or weights. Optional worker tests use
injected PCM and download replies. Real inference tests require the pinned local
bundle and return skip code 77 when absent; a skipped test is not model proof.
Tests that need ONNX Runtime do not run in the default CI graph, which installs none.

Synthetic replay has shown useful gains from normalization and emission
protection. Those measurements are not an accuracy estimate for arbitrary
on-air conversations. Live listening also showed cases where ggmorse read
better. No backend is claimed to be universally superior, and DeepFist does
not train or improve itself during use.

The preserved comparison build, Fldigi experiment and experimental ggmorse
changes are separate from this PR. No Fldigi implementation or comparison
panel is included here. The merged ggmorse concurrency fix (#5645) is preserved:
worker-owned engine, coherent parameter updates, frame-bounded work and joined teardown. A5 extends
that worker with typed source leases and generations; TX sidetone stays on its
existing byte API.
