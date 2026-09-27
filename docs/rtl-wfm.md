# RTL broadcast FM at 48 kHz

Implementation of RFC #5468 section 4, layered on the capture/parking work in
draft #5924. Production receiver admission remains one. This does not qualify
multiple receivers, transmit, other radio families, or new architecture limits.
The free-pan amendment and placement/styling of the new RX controls remain
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
values and is backed by a typed backend verb. RX controls expose 50 and 75 µs;
old schema-one settings without this optional per-slice field retain the previous
75 µs response. Changes become observed and persistent only after DSP adoption.
Unsupported values, stale model owners and foreign-thread requests do not dispatch.
The existing RtlSlices owner retains the setting across other modes and restart.
Pilot status is never persisted.

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
and observations remain fenced by capture/session/receiver lifetime.

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

HD Radio/nrsc5 implementation is deferred until the current RTL sprint finishes.
Its future branch needs wide capture IQ before this analog filter, its own
station-centered extraction and 744187.5 Hz conversion, and explicit paired
44.1→48 kHz PCM conversion. RADE's demodulated 24 kHz speech input is unsuitable.
No nrsc5 dependency, speculative digital framework or automatic fallback is added.
