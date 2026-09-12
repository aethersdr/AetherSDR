# AudioEngine RX rate domains (RFC #5468 A2)

The typed speaker route accepts 24 or 48 kHz producer PCM. The negotiated
device format remains independent: `m_rxProducerRate` describes main input;
`m_rxOutputRate` describes device output. `DEFAULT_SAMPLE_RATE` stays 24000.
Legacy byte input and every Kiwi auxiliary route retain 24 kHz stereo.

## Queues and processing

Raw queues and whole NR2 packets count time in their producer domain. Each
source's processed output FIFO counts time in the device domain. Presentation
delays, input caps and output-to-input conversions use the appropriate rate.
Main and auxiliary sources each own left/right output converters; conversion
preserves channel separation, and equal-rate output needs no converter.

Aggregate diagnostics retain the actual queued-byte count and separately sum
durations in each queue's domain, including every auxiliary source and RADE.
The duration peak is tracked independently of the byte peak, so a later rate
change cannot reinterpret historical storage as a different elapsed duration.
This is summed application queue occupancy, not end-to-end playback latency;
the device queue remains a separate measurement.

Main client EQ, gate, compressor, de-esser, tube and PUDU prepare at the main
producer rate. Each Kiwi source has independent 24 kHz client-effect history.
The existing main objects remain the UI parameter owners; auxiliary objects
copy atomic parameter values without copying processing history. Concurrent
sources never alternate through the same stateful processor. The RX EQ
analyzer uses main input when a typed main source is present, otherwise Kiwi,
so a single analysis window never combines different sample clocks.

Noise filters follow the producer domain, including the optional wrappers
described in [noise-filter-rate-domains.md](noise-filter-rate-domains.md).
RNNoise uses its existing native 48 kHz stereo entrypoint. For noise-reduction
algorithms compiled for the platform, missing or invalid selected processing
withholds that source, and preparation failures are reported. A2 leaves
unavailable-platform setter behavior, including non-macOS MNR, unchanged.
Heavy preparation occurs on the engine owner or the existing auxiliary
initialization worker, outside the device callback. This change does not
establish real-time preparation latency.

## Epochs and transitions

The retained typed frame carries its producer's revocable lease. The ingress
gate rejects malformed, obsolete, duplicate and replayed frames. One active
speaker producer owns main playback; a distinct producer cannot replace it
until the old lease is revoked. An auxiliary producer has one registered route;
queued frames cannot recreate a removed route or feed multiple profile IDs.
The compatibility byte feed cannot duplicate a live typed speaker feed.

A format change, new epoch, discontinuity or replacement clears that source's
raw/packet/output queues, converters and processing history before admission.
Main optional filters are reconstructed with their settings; auxiliary
Specbleach and NVIDIA objects are reconstructed because their ordinary reset
does not promise complete algorithm-state retirement. Mute/disable also clears
auxiliary client-effect and converter history. A device format change clears
all device-domain queues and converters while preserving producer domains.

Retirement runs on ingress where needed, on the speaker timer and immediately
before device submission. Invalid sources lose their admitted application
queues. Already submitted device audio is a mixed stream, so `QAudioSink::reset`
flushes the complete short device queue and restarts it; other sources retain
their application FIFOs. If revocation is observed after a mixed chunk was
prepared, that chunk is discarded. This is bounded polling at the audio drain,
not a claim that physical output can be recalled at the exact revocation instant.

## Fixed contracts and scope

CW decoder/sidetone conversion and decoded RADE speech keep their existing
fixed-rate contracts. RADE output converts from 24 kHz to the device rate,
independently of the main producer. TX admission, cancellation and deferred
release continue through the inherited #5591 coordinator and wiring.

The new RTL 48 kHz producer and multi-receiver runtime remain disabled.
Recording/container changes (A3), TCI conversion (A4), and decoder/clock
conversion (A5) remain separate; accepting test-injected speaker48 PCM does
not qualify those consumers for a live 48 kHz producer.

## Software evidence

`audio_engine_rates_test` drives production ingress, queueing, effect processing
and drain into a `QBuffer`. It covers the 24/48-to-24/44.1/48 matrix, stereo and
frequency content, legacy equivalence, bounded queues, transitions, stale
frames, concurrent Kiwi/effects and fixed CW/RADE domains. Optional unavailable
algorithms are explicitly skipped. `pcm_compatibility_test` exercises actual
backend/model routing. `audio_engine_pcm_lifetime_test` overlaps auxiliary
ingress/retirement with the production DSP initializer; its race claim requires
instrumented-Qt TSan. These tests open no audio device, radio transport or socket.
They do not establish audible playback, native device negotiation/reset behavior,
physical RX/TX, sustained performance or RF bandwidth.
