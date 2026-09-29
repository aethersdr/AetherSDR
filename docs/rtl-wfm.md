# RTL broadcast FM at 48 kHz

Implementation of RFC #5468 section 4, layered on the capture/parking work in
draft #5924. Production receiver admission remains one. This does not qualify
multiple receivers, transmit, other radio families, or new architecture limits.
The free-pan amendment and placement/styling of the dedicated WFM applet remain
subject to maintainer review. The new recipe is enabled in this candidate after
generated-signal qualification. The previous installed WFM build is retained for
live comparison and rollback; this draft does not claim release qualification.

## Signal path and RF meaning

An active WFM receiver extracts paired 384 kHz IQ from the shared capture into
2048-frame blocks. An independently prepared WDSP BPS filter applies the saved
RF edges before WDSP converts to its documented 192 kHz broadcast decoder;
1024 DSP frames produce 256 independent L/R frames at 48 kHz. The BPS filter
uses 2049 Blackman-Harris coefficients and a 4096-point overlap-save FFT. Its
complete requested interval includes 3 kHz transition guards. The capture
policy checks actual captured RF, including when low input rates need bounded
interpolation; interpolation never expands captured coverage.

Filter width is an RF selection, not an audio low-pass. The preserved default
200 kHz selection is not a flat 200 kHz usable-bandwidth claim: the fixed
192 kHz decoder conversion measured flat response at the sampled positive
offsets through +90 kHz, about -0.195 dB at +93 kHz, and -6.02 dB at +96 kHz.
The +100 kHz edge is already strongly attenuated. The corresponding negative
response is inferred from the symmetric filter; this sweep did not measure it.
Narrower filters intentionally trade modulation sidebands for adjacent-channel
rejection. Saved edges are never silently resized on
restore; entering WFM from a narrow incompatible mode selects the existing
200 kHz broadcast preset. Nonempty saved sessions bootstrap with muted, narrow
legacy AM before adopting their original receiver ID and recipe. This keeps the
sole native reservation free for sparse IDs and avoids FM's automatic DC
placement moving capture.
Old WFM could store narrow filters it did not apply. A saved WFM interval
outside the new declared range is preserved and refused with a configuration
warning, leaving the bootstrap muted. The operator must explicitly choose a
supported WFM filter and unmute; reconnect never silently widens the saved RF
selection. Validation may use an explicitly authorized 200 kHz selection.

`WdspChannel::WbfmReceive` explicitly opts into this recipe. Existing WDSP
owners keep their configuration. The wrapper applies paired gain 0.25 before
the independent receiver tap and speaker mixer. WDSP's ordinary RX panel and
notch bandpass do not control WBFM, so this gain and RF filter have explicit
owners. There is no narrow-FM FFT squelch layered over broadcast FM: WDSP's
internal automatic squelch remains in use; the manual SQL control is unavailable.

Two opted-in decoder corrections preserve stereo. Adjacent IQ phase differences
average instantaneous frequency over one sample, weakening the 38 kHz difference
channel. A bounded, preallocated 13-tap linear-phase FIR corrects that response
through 53 kHz. The existing DC blocker moves after the L/R matrix for this
recipe, with identical independent histories, so it cannot phase-shift the sum
channel relative to the difference channel. Legacy WBFM retains its previous
placement. See `third_party/wdsp/AETHERSDR-PATCHES.md` for source provenance,
coefficients, noise gain and refresh instructions.

## Confirmed controls and observations

The optional `broadcastFmReceive` capability declares supported de-emphasis
values and typed control/diagnostic support backed by backend verbs. The dedicated
WFM applet exposes 50 and 75 µs;
old schema-one settings without this optional per-slice field retain the previous
75 µs response. Changes become observed and persistent only after DSP adoption.
Unsupported values, stale model owners and foreign-thread requests do not dispatch.
The existing RtlSlices owner retains the setting across other modes and restart.
Pilot status is never persisted.

The WFM applet follows the selected owned slice and is presented only while a
connected backend declares broadcast FM reception and that slice's accepted mode
is WFM. The standard applet container owns its dock/float placement, button and
open/closed choice. Temporary unavailability suppresses presentation without
closing the saved workspace item. Generic RX controls retain their behavior;
the broadcast-specific row is removed from RX.

The single Mono/Stereo button requests a prepared receiver recipe. Mono selects
real L+R mono output; Stereo selects automatic pilot-based stereo with ordinary
mono fallback. The accepted per-slice choice persists in RtlSlices and defaults
to the previous automatic behavior for older settings. The button cannot claim
accepted state before DSP adoption. Decoder observations remain separate from
that selection, with current stereo shown through a success theme token and
plain accessible text. A missing pilot alone is not a poor-signal verdict.

The applet Settings drawer independently persists Lock Scope and Diagnostics
visibility in one nested client UI object. The scope plots measured 19 kHz pilot
magnitude and the detector's actual acquire/release thresholds in relative
normalized discriminator units. It retains at most 160 observations and up to
40 seconds; hiding it clears history and stops its plotting/history work without
stopping the receiver. Receiver changes and stale observations clear the scope.
This is not a PLL, SNR, calibrated RF-power or channel-separation instrument.

Diagnostics describe the measured pilot detector's consecutive-block hysteresis,
lock duration, loss/reacquisition counts and up to five seconds of unchanged
pilot state. Durations come from completed decoder samples. The optional C
snapshot is coherently read with bounded atomic retries, carried by the existing
receiver PCM lifetime fences and published to the model at most four times per
second plus actual status changes. Unknown or stale measurements are unavailable;
no quality percentage is invented. Configuration and processing-health readouts
are distinct from reception measurements.

Native NRSC-5 digital FM is a separately requested next checkpoint and RFC amendment,
not covered by the existing analog architecture approval or the qualification
below. The analog checkpoint exposes no ineffective digital state or empty metadata
controls. Future digital program/ID3 metadata and analog RDS have different decoder
sources; RDS remains separate from the requested native digital implementation.

The stereo indicator reports the last completed decoder block carried by a
current-session/current-revision independent audio packet. It is unavailable
outside receiving WFM, acquiring after adoption, and mono or stereo only after
current decoder evidence. Every accepted revision clears the previous claim,
including repair of an unchanged recipe. A 500 ms absence of accepted decoder
observations returns it to acquiring. Park, removal, mode change and disconnect
retire the corresponding stream. The model drives accessible status text;
selecting WFM alone cannot establish stereo.

RF/de-emphasis changes prepare replacements away from acquisition and publish
new receiver and speaker epochs. Startup positions align to the final 48 kHz
sample lattice, including nonzero and coprime capture-rate origins. Each packet
declares 48 kHz, stereo layout and actual frame count. Monitor mute, gain and
balance affect the speaker mix after the independent slice tap. Queued stale PCM
and observations remain fenced by capture/session/receiver lifetime. The mixer
starts each new epoch at the same exact capture/audio lattice point as extraction;
it does not demand rounded-down samples that the extractor cannot produce.

Nonblocking opted-in WFM prepares eight WDSP exchange slots. The seven initial
output credits cover the largest single 8192-complex-sample USB callback burst
at the admitted capture rates. This is finite headroom, not a guarantee against
arbitrary queued callbacks or worker starvation. WDSP still reports a real
underrun when exhausted, and the receiver still withdraws immediately. Output
bytes and their write cursor are complete before WDSP publishes output credit.
Blocking WFM and legacy channels retain depth two, including reused channel IDs.

The prepared WFM exchange contributes 2048 audio frames (42.667 ms) of transport
delay, including the prior-output worker stage, versus 512 frames (10.667 ms)
at depth two. The extra delay is 32 ms; filter, device and other DSP delays are
additional. Ring storage increases from 72 to 288 KiB per opted-in channel.
The mixer's unchanged 2048-frame missing-contribution deadline is a separate
capture-position bound, not a total audio-latency limit.

The `aether.rtl.receive` debug category is disabled by default. When explicitly
enabled, owner-thread records identify the first failing process result,
extractor reason, capture/IQ positions and exact missing mixer frames. Its fixed
queue reports overflow separately; acquisition performs no logging or allocation.
Health status also exposes the existing accepted/requested capture revisions and
trace-drop count, allowing bounded observation with detailed logging disabled.

Accepted 48 kHz speaker PCM also drives RX device negotiation: prefer 48 kHz,
then 44.1 kHz, then 24 kHz through the shared stereo-preserving policy. A 24 kHz
fallback is usable but cannot retain 15 kHz audio, and the actual device rate
is reported independently of the producer. Deferred requests check the current
producer lease and sink generation. Same-rate epochs and brief retirement gaps
do not renegotiate the sink rate. Legacy 24 kHz policy and fixed CW/Quindar/RADE
contracts remain unchanged; speaker-only changes retain producer queues and effects.

## Qualification evidence and limits

`wdsp_wbfm_test` uses analytical continuous-FM IQ (75 kHz peak deviation scale,
10% pilot, independent left/right tones). Rectangular phase accumulation at the
DSP rate is not a valid reference because it cancels the discriminator defect.
It checks the documented 1/2 kHz reference against at least 40 dB separation,
plus low/high audio references, pilot acquisition/loss, mono fallback, both
de-emphasis responses, 15 kHz bandwidth, RF selection/polarity, no-signal noise,
rate/pilot offsets, output geometry, allocation scope and lifetime.

Before the fixes, filtering had no measured effect, the 90% mono reference
peaked at 3.258 with clipping, and 50/75 µs selections had identical response.
The filter/headroom correction measured over 91 dB adjacent-channel selection,
peak about 0.815 without clipping, a 15 kHz 50/75 µs amplitude ratio of 1.483,
and over 86 dB rejection 3 kHz beyond the 60 kHz RF filter's edges. These are
generated-signal measurements, not antenna selectivity or hardware reception proof.

A 97.5% modulation stress vector reports separation separately and requires
finite, unclipped headroom. Its separation is limited by high-order RF sidebands
outside the 192 kHz decoder input, so the 40 dB reference result does not claim
40 dB for every modulation waveform. A future remedy would require preserving
those sidebands through discrimination; neither gain nor relabeling sample rates
can recover them. Live strong/weak-station listening remains separate evidence.

`rtl_wfm_pipeline_test` drives production extraction, immutable receiver banks,
paired 48 kHz taps and the speaker mixer with generated IQ. It verifies positions,
frame counts, independent monitor routing, parking/resume and repair epochs.
Injected-device model/runtime tests verify accepted controls, persistence,
capture guards, low-rate reconnect and stale status. `wfm_controls_test` verifies
capability gating, confirmed selection, keyboard behavior and accessible state.
No new test binds a socket or represents third-party firmware.

The no-device `audio_engine_rates_test` admits real typed PCM, dispatches the
queued speaker-rate change, and checks actual final output at 48/44.1 kHz and
the 24 kHz fallback. It checks 15 kHz content, independent channels, stale
requests, retirement gaps and retained auxiliary queues. The injected boundary
is device opening; physical sink behavior still requires live validation.

The opt-in `AETHER_WFM_BENCHMARK=1` fixture additionally runs production capture
DDC, the 65,536-bin display FFT at 25 FPS, extraction, WDSP, paired taps and mixer.
Four RF seconds at 2.4 MS/s completed in 1.905 seconds (2.10×), with 100 FFT frames
and waterfall rows, 192,000 frames on each 48 kHz output, and zero observed repair,
drop, late-frame or mixer-error counters. The feed is capped at 2.1×: this is a
sustained lower bound, not maximum speed. Preparation, USB byte conversion,
backend timers, AudioEngine and GUI are outside that timed measurement.

Mutation checks demonstrate that wrong IQ-to-audio positions fail the pipeline
test and a hidden 48→24→48 bottleneck fails the actual WAV spectral test even
when rate/duration metadata remain unchanged. Source restoration is followed by
passing rebuilds and tests.

Native validation is on Nobara in this work order. Hosted checks cover their
configured platforms; no local Mac/Windows/ARM hardware result is implied.
Production planning uses its real persistent wisdom path, never the test planner
time limit. Receiver admission remains one while multi-receiver, long-duration,
architecture-specific and operator acceptance work remains separate.

## Future digital reception

Digital FM/nrsc5 implementation is deferred until the current RTL sprint finishes.
Its future branch needs wide capture IQ before this analog filter, its own
station-centered extraction and 744187.5 Hz conversion, and explicit paired
44.1→48 kHz PCM conversion. RADE's demodulated 24 kHz speech input is unsuitable.
No nrsc5 dependency, speculative digital framework or automatic fallback is added.
