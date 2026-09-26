# RTL M1 capture and audio runtime

M1 connects the transactional capture owner, prepared receiver registry,
per-receiver RF extraction, typed PCM and accepted-state settings. It remains
receive-only. The qualified production admission constant is **one**; eight
stable slots are storage, and four receivers are an offline measurement target.
Increasing admission requires the integrated architecture evidence below.

**Operator-requested design revision (2026-09-25):** RTL free panning keeps
configured slices at their absolute RF settings while the capture moves. A slice
outside usable capture is parked until its complete guarded passband fits again.
This supersedes RFC #5468 §1's previously approved preserve-all-active/refuse
policy for pan-driven capture moves. Ozy approved implementation and receive-only
validation; Jeremy's maintainer decision on this UX/RFC change remains required
before merge. This revision does not raise production receiver admission.

**Offscreen reveal correction (2026-09-26):** the offscreen indicator's
single click and full Qt double-click sequence now use the existing confirmed
receiver `Center` intent at its preserved RF. A zoom-style `Range` request is
still constrained to current capture and cannot stand in for a deliberate reveal.
Center-lock uses the same owner dispatch; Kiwi display routing and optimistic
backends retain their previous pan request. A zoom arriving before capture
adoption coalesces its span without replacing the pending reveal's target.
The opt-in real-widget regression injects USB below the production worker and
covers left/right, narrow/full spans, captured/parked slices, trailing release,
readback failure/compensation and newer pan supersession. Offline Flex command
capture and Hermes receiver-state checks do not claim live hardware coverage.

## Acquisition and ownership

The RTL transaction owns one capture stream. Its complete desired state includes
center, requested sample rate, direct sampling, offset tuning, PPM, tuner gain
and configured receiver passbands. A future device with multiple independently
tunable streams needs separate capture descriptors and membership per stream;
antenna-port count alone cannot establish that topology.
Hardware changes quiesce USB, apply and verify the complete readback, prepare
compatible DSP-active receivers and adopt the resulting bank before
acknowledgment. Configured slice identity and settings do not depend on whether
the slice currently has an active DSP receiver. The `active` selection flag is
independent of `inCapture`, which reports confirmed DSP membership. Preparation
failure after a hardware change compensates hardware and prepares the restored
state. Failed compensation withdraws valid capture. Superseded results cannot
publish or persist, and rapid pan moves coalesce to the latest operator request.

Receiver-only changes prepare on the existing bounded registry pool while USB
continues. The acquisition context adopts the requested revision at a sample
boundary. Unchanged FM receivers share ownership across banks; callbacks only
borrow pointers. Final destruction and WDSP planning run on the pool. A reused
slot waits for retirement and gets a new instance. The mailbox holds one work
item; the transaction owner coalesces one further complete desired state.

## FM extraction and mixing

FM/FMN use separate phase-continuous NCOs, paired r8brain histories and fixed
1024-sample planar WDSP blocks at 48 kHz. An FM slice is DSP-active only when
its full RF filter interval plus the 3 kHz guard fits inside the confirmed
usable capture. Both interval edges must fit under `SharedCapturePolicy`'s
conservative rounding; a slice whose filter only partly fits is parked. Its
passband is never clipped into an aliased channel. Parked slices retain
configuration but emit no audio or decoder data. Returning to capture prepares
a fresh receiver and resumes after adoption, without replaying old output. A gap, invalid block
or WDSP underrun withdraws an active receiver; replacement is prepared away
from acquisition. WDSP's nonblocking exchange advances its ring on underrun,
so continuing that instance could mislabel stale samples. Unchanged siblings
retain history and keep progressing.

A receiver joining an existing capture starts at the next exact coincidence of
its integral hardware sample clock and the 48 kHz clock. This is at most one
second for a coprime rate (usually far shorter). NCO phase uses the absolute
capture position. Arbitrary input partitioning cannot change converter calls.
All FM receivers use the same fixed-rate graph/filter length and retain its
common causal delay; output positions label that common output timeline.
Generated late-join comparisons check alignment after startup transients.

FM explicitly uses 5 kHz deviation and FMN 2.5 kHz. The opt-in WDSP receive
recipe sets unity panel gain and enables its FM limiter at 0 dB maximum gain.
The main RX AGC is bypassed by WDSP in FM. Previously, the unconfigured panel's
gain of four and disabled limiter caused about 75% hard clipping at unity
monitor gain for a settled 1 kHz, 2.5 kHz-deviation input. The recipe applies
before the independent tap. Other WDSP owners retain their existing settings.

Each independent slice tap precedes monitor gain, mute and balance. The mixer
retains independent left/right channels and indexes fixed queues by sample
position. It emits 128-frame stereo quanta. A missing slice has a 2048-frame
(42.67 ms) deadline; expired positions become silence and late audio is dropped,
never moved to the current clock. Each slot has 8192 frames of storage. Summed
monitor levels use 1/N headroom followed by bounded clipping; adaptive level
normalization is outside M1. A 128-packet SPSC mailbox carries owned numeric
blocks to the backend thread, which constructs revocable typed PCM. Queue gaps
are marked discontinuous. `AudioEngine::DEFAULT_SAMPLE_RATE` remains 24000 and
the audio device's output rate remains independently negotiated.

WFM and the other existing non-FM modes retain their exclusive legacy DDC and
24 kHz format. Their demodulation is unchanged; monitor mute continues to clock
an independent pre-monitor tap. They cannot join a multi-FM bank. Selecting FM
with an incompatible inherited wide/sideband filter chooses a 16 kHz passband;
ordinary filter requests and saved restores are never resized. This mode
transition was approved in [the #5924 maintainer ruling](https://github.com/aethersdr/AetherSDR/pull/5924#issuecomment-5787852269). WFM stereo and normalization qualification
remain S1/S2; no replacement WFM claim is made here.
The desktop FM/FM-N presets use the existing DFM width ladder intersected with
the backend's declared filter range; FMN and NFM share symmetric edge rules.
Fixed radio filter lists retain precedence. WFM filter and squelch controls
are unavailable with accessible reasons, and the backend refuses those edits.

## FM squelch

FM/FM-N have independent, acquisition-owned signal-level gates before both the
receiver tap and monitor mix. The detector is the peak FFT bin in the receive
passband, including edge bins, using the existing 2048-point Blackman-Harris
capture FFT normalized by FFT size. These are **dBFS/bin**, not calibrated dBm
or integrated channel power. This coarse gate does not distinguish two signals
inside the same passband. Its scale is `-120 + 1.2 * level`, for levels 0–100.
The display and Auto estimator consume the same bin scale. Auto retains its
trimmed noise-floor estimate, 0.1 EMA and operator-selected 5–20 dB margin;
enabling Auto never misuses that margin as an absolute threshold.

The gate has 3 dB hysteresis, a 150 ms hang and 5 ms audio ramps. It starts
closed until measured and closes after 100 ms without detector input. The
detector runs at 30 Hz independently of display throttling, sharing the FFT
and a fixed buffer without allocating a detector frame. Threshold changes
travel in the existing capture transaction and preserve the receiver's DSP
epoch/history. Capture/receiver changes reset detector history. Legacy modes
remain unsquelched; changing into one confirms squelch Off. No RF test or
calibration is implied by these offline semantics. The scale, presets and FM
recipe remain subject to maintainer review under RFC #5468.

## Capture placement and display geometry

FM/FM-N establishment and necessary capture recentering prefer a hardware
center one quarter of the capture rate above the selected FM carrier. The existing
receiver NCO translates the unchanged absolute RF to baseband. The requested
viewport is fitted to the confirmed usable capture. For each slice that
will be DSP-active, its complete passband and guards must fit; a configured
slice that cannot fit remains parked at its original RF. The center preference
keeps converter DC at least 48 kHz from the selected FM carrier (or outside
its guarded passband, whichever is wider) where a legal placement allows it.
Other receiving FM slices can overlap DC; Radio Health reports that condition.
Legal integer-Hz tuner bounds and the automatic 24 MHz
direct-sampling boundary still constrain capture movement. DC overlap is
reported truthfully rather than silently moving a slice or refusing browsing
solely to keep another slice active. This does not enable librtlsdr offset
tuning, alter the R82xx guard, erase FFT bins, subtract a signal mean, or notch
demodulated audio. This FM-specific DC preference does not alter WFM or other
legacy demodulators. Free-pan membership and parking apply to every configured
RTL slice.

Ordinary in-window frequency changes keep the existing legal capture, including
when the operator tunes near converter DC. A transition into overlap produces
an existing configuration warning. The spectrum context menu's **Move capture
away from DC** action explicitly requests DC-cleared placement while preserving
every configured slice's absolute RF and settings. Slices outside the resulting
usable capture remain parked. It is dimmed with an accessible reason unless the
backend declares support. A busy or impossible DC-clearing request refuses.
Radio Health reports accepted capture center/rate, usable RF edges, FM DC-clear
status and the last request outcome. This is geometry and converter-relative
evidence, not a measurement of a physical dongle's DC bias or calibrated RF
power.

RTL zoom and in-capture pan crop contiguous original bins from the 2048-point
capture FFT. The sixteen-bin zoom floor is 18.75 kHz at 2.4 MS/s; neither
interpolation nor additional resolution is claimed. Panning past the usable
capture moves the hardware capture across supported RF, including a drag at
full zoom-out. It does not change the sample rate or any slice's configured RF,
mode, filter or other settings. The viewport and capture are distinct: a slice
outside the visible zoomed viewport keeps receiving whenever its full guarded
passband remains inside usable capture. A slice outside usable capture is
parked and clearly reported out of capture, and resumes automatically
when the confirmed capture again contains the whole passband. The existing
explicit `rtl/sample_rate.set` extension remains the hardware sample-rate
control. The normalized slice state exposes `inCapture`; the desktop gives an
accessible out-of-capture reason even when its marker is offscreen. Squelch
sees only current-capture detector input for DSP-active slices.

The native spectrum widget follows the same confirmed observation during a
pan, zoom or VFO-edge gesture. Pointer movement sends intent; quantized or
hardware-confirmed backend geometry updates the axis and waterfall while the
pointer is still held. Pending or refused changes keep the accepted view and
continue ingesting current FFT rows. A failed retune restores the previous
confirmed capture, view and capture membership after compensation; failed
compensation withdraws capture rather than showing false geometry or stale
receive data. Release flushes the latest requested geometry without
substituting an older observation. Flex retains its optimistic preview and
stale-status hold; Kiwi's independent display path is unchanged.

Deferred native FFT/waterfall delivery carries the pan object's geometry
revision. A frame superseded by an accepted range or session change is discarded
instead of being painted against a different axis; unchanged/clamped geometry
does not discard fresh frames. A backend-generation guard also rejects queued
raw FFT delivery from a retired backend. These guards add no worker or transport.
The socket-free `pan_frame_guard_test` is in the default graph. The real-widget
gesture regression is opt-in with `AETHER_BUILD_SPECTRUM_GESTURE_TEST=ON`; it
drives production mouse/wheel paths, without hardware or a firmware peer.

The neutral confirmed-tune intent admits receiver RF and Preserve/Reveal/Center
display intent together. Typed entry requests centering; publication waits for
DSP/capture adoption. An in-capture Center can move only the viewport, without
USB writes. A distant Center follows its selected slice with a capture retune;
other configured slices may park. The transaction fits the selected slice's
complete guarded passband and requires its RF to center in the real 2048-bin view
within half a bin plus integer-Hz tolerance. At full width this view requirement
can leave converter DC on the selected FM carrier when no DC-clear position also
fits. Radio Health reports the accepted overlap. If no legal capture can satisfy
both the passband and Center view, the request refuses. Exact behavior at the
tuner RF limits has no dedicated regression yet. Pan requests beyond capture
use the same confirmed transaction and latest-request ordering. Refusal, supersession,
failed hardware application and rollback preserve accepted receiver and view
observations.
Other backends retain their existing policy. These desktop verbs add no headless
control grant. The placement preference, separation, zoom floor and explicit
action are scoped UX choices requiring maintainer ratification.

Synthetic DC regression compares clean and biased eight-bit IQ through the
production RF extractor and WDSP FM channel at quarter-rate and both minimum
separation edges, including low modulation index, bias steps and arbitrary
callback chunks. Centered biased IQ and blind mean subtraction are negative
controls. This is offline DSP evidence; actual hardware convergence remains
a separate authorized receive test.

## Accepted-state persistence

RadioModel supplies its settings scope and reported-serial identity through a
neutral seam hook. The backend does not derive persistence identity from a
later USB enumeration. After accepted readback, successful migration/claim of
`RtlSlices` removes Tuning, Passband and SpanRate from generic OperatingState
ownership together. RF gain uses `storeRtlRfGainPreservingLegacy`, retaining the
legacy downgrade snapshot and unknown fields. A handled but refused feature
write never falls through to a second generic writer.

Saved receivers are considered in ascending stable-ID order against the
accepted, fixed capture. Restore does not retune, resize a passband or move a
sibling. Valid configured slices outside that capture remain parked with their
settings and stable IDs, subject to the current configured-slice capacity and
addressable-slot bounds. Production still admits one configured slice; fixture
capacity can exercise multiple configured slices without raising that limit.
Explicit accepted removal is separate. If no saved slice is valid, retain the
session's valid initial receiver. Pending or refused requests never feed the
document writer; an accepted pan preserves every configured slice's absolute RF
and settings while persisting confirmed capture geometry. Capture membership is
derived again from that geometry and the configured passbands; it is not a saved
slice setting.
Monitor controls are prepared with the bank and applied before its first block.
Accepted FM/FM-N enabled/threshold values are restored and saved in `RtlSlices`.
The desktop separately owns `ReceiveSquelchIntent-<stable ID>` (schema 1), which
retains the manual choice and Auto preference for an identified radio. Accepted
Off wins over an old Auto preference. AGC fields remain preserved without a new
AGC implementation claim. Real numeric USB serials remain identities; synthetic indices
use the model's anonymous family scope. Duplicate real serials still share one
settings identity and cannot be distinguished by this schema.

## Evidence and admission gates

Socket-free tests cover transactional failure/compensation, delayed retirement,
registry reuse, four distinct generated FM carriers through extraction and real
WDSP, arbitrary/one-sample chunks, capture edges, late-join alignment, mixer
holes/headroom/stereo, native PCM revocation and slot reuse, and persistence
ownership. The ordinary-C++ allocation probe does not intercept malloc, Qt or
private allocators. WDSP's C allocation guard now observes the executing thread;
its process-wide counter remains available for resource accounting. RTL warms
platform TLS before entering acquisition callbacks.

The changed socket-free tests contain assertions for interior and capture-moving
drags (including full zoom-out), guarded-passband edge parking,
offscreen-but-captured reception, parked/resumed slice identity and settings,
fixture-only multiple membership, rapid coalescing, retune rollback, stale FFT
and waterfall revision rejection, and silence while every slice is parked.
Typed Center assertions cover an in-capture view move without USB writes and a
distant full-width retune that parks a sibling. Execution results on the final
source head, mutation sensitivity and native receive convergence are separate
evidence; the presence of assertions does not establish that they pass.
Family-swap coverage must keep Flex, Icom and Hermes behavior. These tests do
not raise the qualified production capacity of one.

Before raising `kQualifiedReceiverCapacity`, freeze 1/2/4-distinct-FM workloads
and run at least 30 minutes with the actual integrated application, spectrum,
USB and audio paths on the target architecture. Require zero unexpected USB
restarts, zero stale-epoch acceptance, zero steady-state queue drops or receiver
withdrawals, bounded late-slice silence, and continuous sample counts. Record
CPU, thermal state, callback latency, startup/planning latency and cancellation
latency; admission needs headroom rather than component-only throughput. Cold
and warm FFTW wisdom runs use separate task-owned directories and production
planning. Native Mac/Linux/Windows RTL-on/off builds, ASan/UBSan, instrumented-Qt
TSan, affected A-series tests and real receive convergence are distinct evidence.
Linux ARM needs representative native hardware; macOS ARM is not a substitute.

The shared model opts into `IRadioBackend::receiveControlPolicy()` for RTL.
`Confirmed` keeps slice frequency, mode, filter, squelch and monitor gain/pan/mute at the
last backend observation until capture/DSP adoption publishes a new report.
Pan center and bandwidth likewise wait for the backend geometry report; a
dispatch returns false to gesture callers so they cannot advance the view on
that basis. Other backends retain their existing optimistic policy by default.
This policy grants no capabilities and adds no duplicate requested-state store.

Control routing checks the current model object's identity and connected
backend, and RTL admits controls only for published stable receiver IDs. A
pending new member, staged/disconnected object or retired object whose numeric
ID was reused cannot address a live replacement. Returning a control to its
observed value still supersedes an earlier pending request. Accepted filter
edges are applied exactly rather than normalized again by SliceModel.

The injected full-model regression covers sparse ID 3 at production admission
one, pending/refused/superseded requests, compensation failure, persistence,
pan geometry, reentrant edits, foreign-thread refusal and object/ID reuse. It
uses the real model, backend and worker with an injected USB device, without
opening hardware or sockets. Multiple membership is admitted only by its test
fixture; it does not raise production admission.

This implementation is local to this PR and does not import PR #5919 or its
stack. Reconciliation with that routing work remains a later integration task:
typed dispatch alone is not acknowledgment, and every adapter must preserve
sparse stable IDs. The publication-policy seam, RTL-owned settings hooks/domain
transfer and FM passband transition were ratified in the ruling linked above.
A second backend adopting the settings takeover must justify it separately.
The new DC placement and joint tune/view intents still need ratification. Offline
model acceptance does not establish live receive convergence or release readiness.
The operator-requested free-pan and parked-slice policy also needs Jeremy's
explicit UX/RFC decision before merge; prior approval of #5468's whole-set
refusal policy does not grant that decision. Native receive-only verification
must confirm pointer behavior, actual capture readback, parked/resumed routing
and launcher revision before live receive convergence is claimed.


### Runtime diagnostic readback

The existing backend `health` snapshot exposes cumulative per-connection
`rtlQueueDrops`, `rtlMixerLateFrames`, `rtlMixerRejectedBlocks` and
`rtlMixerConfigurationFailures`. The acquisition callback publishes only atomic
counter mirrors; the backend's existing service timer samples them into its
owner-thread cache. Counters are independently sampled, not a coherent event
trace. Legacy-only sessions leave the FM pipeline values unreported until that
pipeline has actually processed a callback; disconnected snapshots are empty.
A mixer configuration rejection discards its old buffered audio and requests
repair through the existing transaction owner before any receiver processing.

These counters do not measure callback p99/max, USB control latency, RF extractor
group delay, or the frozen 1/2/4-receiver hardware workload. Those qualification
gates remain open. The model publication contract above must survive later
integration with other receive-control routing changes.

### Driver-specific prerequisites

The control audit compared [upstream librtlsdr at 797f8143](https://github.com/osmocom/rtl-sdr/blob/797f8143266d983c56d8f35d2d442527529dd8a5/src/librtlsdr.c)
with [RTL-SDR Blog at aed0ea19 (V1.4.0)](https://github.com/rtlsdrblog/rtl-sdr-blog/blob/aed0ea19f3a273370a13c9009b96313c75d54c7b/src/librtlsdr.c).
Mac and Nobara's audited libraries were built from that clean Blog source;
the Windows DLL hash matched the vendor's V1.4.0 release archive. This is a
statement about those build artifacts, not every installed RTL library.

Both sources reject offset tuning on R820T/R828D with `-2`, but Blog first
toggles bias-tee GPIO. Its offset getter still reads the tuning offset, not
antenna power. The private USB adapter therefore refuses R82xx offset changes
before calling the setter. A matching disabled offset remains a no-op; it does
not prove bias tee is off. Failed offset requests preserve accepted capture
state through the existing transaction compensation path. Supported tuners keep
normal offset control; actual I/O errors and direct-sampling conflicts remain
refusals. The socket-free adapter regression covers these distinct cases.

Before an authorized hardware run, record the loaded library path/hash/source,
tuner and dongle variant, and the operator's known bias-tee/EEPROM configuration.
Blog's forced-bias EEPROM option can enable antenna DC during device open and
ignore an off request. Do not open a device to discover that setting, use offset
tuning as a bias-tee probe, or interpret offset readback as a DC measurement.
No EEPROM, GPIO, driver or bias-tee changes are part of offline qualification.

Blog mode `0` also permits automatic Q direct sampling below 24 MHz for R820T
(excluding its recognized Blog V4L variant); R828D does not take that path.
Its direct-sampling getter reports the current path, not the remembered mode.
M1 currently selects Q sampling below 24 MHz independent of tuner identity.
Thus V4/upconverter HF selection and manual mode-0 behavior need an explicit
device-specific policy/qualification decision before claiming support. Do not
weaken exact transaction readback to accept a mismatched sampling path. The
source probes used stubbed low-level I/O, and the adapter tests used injected C
calls: neither reproduces a reported user's hardware/driver problem nor qualifies
live reception. Production admission remains one.

## Continuous high-resolution display spectrum

The display observes 65,536 consecutive full-capture IQ samples with a
four-term Blackman-Harris window. At 2.4 MS/s its observation is 27.3067 ms
and bin spacing is 36.6211 Hz; the 2,048-point path had 1,171.875 Hz spacing.
A preallocated circular buffer spans variable USB callbacks. No partial
window is zero-padded into a display frame. Sample-count deadlines pace
frames independently of callback partitioning, with overlapping windows at
higher frame rates. At the lowest supported rate (225,001 S/s), initial
window fill takes 291.27 ms; subsequent frames can overlap at the requested
rate. Frame rate alone does not shorten the observation interval.

The complete capture remains available. Viewport cropping selects genuine
bins after the transform, without an NCO, downsampling, or a second receive
path. No audio channel consumes this display history. Capture/adoption
boundaries, rollback and malformed USB callbacks discard partial display
history; the existing worker token, backend receiver generation and accepted
frame checks fence queued frames. The device API does not report a sample
sequence for otherwise valid callbacks, so undetectable upstream loss cannot
be claimed to be identified by this accumulator.

Squelch retains its original 2,048-point transform, peak-bin scale and
nominal 30 Hz schedule. Display amplitude remains `20 log10(abs(FFT) / N)`;
coherent Blackman-Harris gain is about -8.904 dB. Noise power per display bin
falls about 15.05 dB for the 32-fold narrower bin spacing, as expected; this
does not recalibrate squelch or claim dBm. With optional IQ DC suppression disabled, DC and neighboring bins remain
unaltered. Sixteen genuine bins remain the minimum view, now 585.94 Hz at
2.4 MS/s. This is an analysis window, not interpolated detail.

The bounded design comparison and generated carrier/noise fixtures favor the
larger full-capture transform over a translated, filtered and decimated
2,048-point display stream: it preserves view-independent history and avoids
filter/NCO settling on zoom. The display's persistent arrays occupy 2 MiB,
plus FFTW plan storage and queued output frames. Planning/allocation and
teardown use the existing float FFTW planner lock outside acquisition.
[`rtl_spectrum_resolution_test`](../tests/rtl_spectrum_resolution_test.cpp)
pins two carriers 500 Hz apart, positive/negative/edge/DC mapping, window gain,
noise power, sample partitioning, exact cadence, discontinuity refill and
squelch independence. Injected-worker, model and real-widget tests cover the
production boundary, raw DC crop, parked receivers and offscreen reveal.
Live performance and delivery evidence are revision-specific in the PR body.

## Device PPM and optional IQ DC suppression

`RtlDeviceSettings` owns the schema-1 `RtlDevice` document in the exact
reported-serial scope. It stores integer `ppm` (-1000 through +1000) and
boolean `dcSuppression` (default false). Calibration never falls back to the
family row or a USB index. Anonymous devices remain session-only; identical
reported serials necessarily share the same identity. On opening USB, a serial
mismatch clears the restored correction and refuses writes to that scope.
Unreadable, malformed and future documents are preserved. Unknown fields in
a supported document survive an accepted update. `OperatingState` and the UI
do not write these fields.

The existing `rtl/ppm.set` transaction remains the hardware path. Fractional,
boolean, string and out-of-range inputs are refused. The owner saves only after
hardware readback and DSP adoption publish an accepted capture; pending,
superseded and rolled-back requests cannot save. A save failure is distinct
from hardware failure: applied state remains visible with a session-only
reason. The RTL Receiver settings page discovers `rtl` and `settingsVersion=1`,
uses `settings.get` plus `extensionStatus("rtl", "settings", ...)`, and keeps
an explicit applied readout while controls are pending. IDs survive page
recreation without reuse, and connection changes retire pending UI intent.
These device-specific controls and their presentation remain draft policy
requiring maintainer review.

Positive PPM represents a fast crystal in
[Osmocom librtlsdr](https://github.com/osmocom/rtl-sdr/blob/master/src/librtlsdr.c):
the assumed clock is multiplied by `1 + ppm / 1e6`, and frequency correction
updates the sample clock and tuner configuration. If a stable reference reads
low with the existing correction, increasing correction moves its indicated
frequency upward. No measured or estimated calibration is automatically
chosen. Integer PPM is the driver's precision, not a promise of absolute
frequency accuracy.

`rtl/dc_suppression.set` requires a boolean. It changes software state in the
same bounded capture transaction, with explicit DSP adoption and rollback;
it never enables hardware offset tuning, bias tee or another device control.
The acquisition thread applies a continuous complex DC blocker before display
and audio. Its response is `g(1-z^-1)/(1-r*z^-1)`, with
`r=exp(-2*pi*5/sampleRate)` and `g=(1+r)/2`, following
[Julius O. Smith's DC blocker](https://www.dsprelated.com/freebooks/filters/DC_Blocker.html).
Double state makes the very small coefficient stable at RTL sample rates.
The filter allocates no memory, preserves every sample position and is
independent of callback partitioning. The raw 2048-point squelch measurement
keeps its original scale and cadence before correction. Display bins remain
real transformed IQ; no bin is hidden, zeroed or interpolated.

Correction is off by default. A constant step decays by 60 dB in about 220 ms;
allow 300 ms to settle after enabling, a hardware acquisition restart or a
recognized malformed callback. Samples continue through during settling;
there is no artificial gap or promise of transient-free reception. Ordinary
receiver-only changes retain estimator history. DC toggles reset the receiver
DSP generation and spectrum window while USB capture continues, so stale
frames and audio cannot be adopted as the new revision.

At 2.4 MS/s the response is approximately -3.01 dB at 5 Hz, -0.043 dB at
50 Hz, -0.011 dB at 100 Hz and -0.00043 dB at 500 Hz from capture center.
A real constant carrier at exact center is indistinguishable from converter
DC and is removed too; AM/CW reception there can be severely distorted.
FM spectra can also contain a center component. This is why suppression is
optional and the existing DC-clear capture placement remains useful. Numerical
fixtures measure both signs of a 48 kHz carrier placement for FM, FMN, AM and
CW, plus exact-center and 50 Hz cases. They quantify IQ effects, not live
audio intelligibility or radio calibration. Revision-specific performance,
mutation, live reception and delivery evidence belongs in the PR report.
