# RX noise-filter rate domains (RFC #5468 A2 prerequisite)

This prerequisite prepares the optional wrappers for AudioEngine's rate-aware
RX input. It does not change AudioEngine wiring, its 24 kHz default, radio
production rates, or the disabled RTL multi-receiver runtime.

`DeepFilterFilter`, `SpecbleachFilter` and `MacNRFilter` take an optional
constructor `sampleRate` (24000 by default). `NvidiaAfxFilter` takes it after
the existing optional pack directory. The only supported domains are 24000 and
48000; other rates leave initialization invalid. `process()` consumes and
returns stereo float32 at that immutable domain, and `sampleRate()` identifies
it. Prepare a new instance when the input rate changes; device rate never
selects the wrapper's processing domain.

| Wrapper | 24 kHz | 48 kHz |
|---|---|---|
| RNNoise (already available) | Existing `Legacy24k` contract | `Native48k` with `PreserveRxStereo`, through `process48kStereo()` |
| SpectralNR (already available) | Existing constructor rate | Pass 48000 to the existing rate-aware constructor |
| DeepFilterNet | Existing 24-to-48-to-24 SRC and model | Native48 model input/output; no intermediate24 stage |
| NVIDIA AFX | Existing 24-to-48-to-24 SRC and model | Native48 model input/output; no intermediate24 stage |
| Specbleach | Existing library rate and 40 ms frame request | Native48000 library rate and the same 40 ms request |
| macOS MNR | Existing 512-point FFT / 256-sample hop | 1024-point FFT / 512-sample hop; same frequency resolution and elapsed estimator history |

DeepFilterNet and NVIDIA retain their existing mono algorithm and stereo
level-balance behavior. They do not become independent stereo denoisers in this
change. The default playback path and native stereo RNNoise/MNR do not inherit
that limitation. `MonoDspStereoAdapter` takes its rate as the second constructor
argument; its five-second queue cap and power-envelope timing follow that rate.
Legacy24 coefficients remain unchanged. DeepFilterNet's three-hop model delay
is expressed in the selected producer domain before pairing delayed stereo.

Each concurrently processed source must own its wrappers. Alternating main48
and Kiwi24 through one instance is invalid even when the rates happen to match:
algorithm history and queued samples belong to one source. Revoke admission,
discard queued processing/output samples, and replace the source's prepared
state at a rate, session, source, or discontinuity boundary. Construction may
load models and allocate; it belongs outside an active processing callback.

`MacNRFilter::reset()` clears its complete state. DeepFilterNet's reset recreates
the model. NVIDIA's existing reset only clears wrapper FIFOs/SRC, and
Specbleach's existing reset clears its noise profile and stereo adapter while
the library may retain overlap history. For a strict new epoch, recreate the
complete NVIDIA/Specbleach object. A2 must apply this lifecycle rule to main,
legacy Kiwi and each managed Kiwi source, preserving effect configuration.

The tests distinguish evidence layers. `nr_rate_domain_test` compiles the real
DeepFilterNet/NVIDIA wrapper translation units and substitutes local C APIs
with an explicit half-gain transfer function (plus DFN3's documented delay).
It verifies rate conversion, full-bandwidth native input, algorithm call counts,
irregular block queues, lifecycle, and concurrent-source isolation; it does not
perform neural inference or load an SDK/GPU. `specbleach_rate_domain_test`
executes the bundled library when enabled. `mac_nr_filter_test` executes vDSP
on macOS. None opens a socket or an audio/radio device. Hardware/model inference,
audible playback and the final AudioEngine route require their own evidence.
