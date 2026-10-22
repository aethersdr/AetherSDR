# RTL AM and synchronous AM reception

AM and SAM use the existing shared-capture extractor and prepared WDSP
receiver bank at 48 kHz. AM invokes the envelope detector; SAM invokes the
vendored `amd.c` carrier PLL with dual-sideband detection and fade leveling.
The upstream PLL frequency bound is ±2 kHz. An abrupt carrier jump after a
dropout can require retuning to reset acquisition. No worker, decoder library,
transmit path or receiver-count default changes.

## RF and audio contract

RF cuts straddle the dial carrier with at least 100 Hz on each side, bounded
at ±12 kHz. Asymmetric cuts are supported. RTL analytic IQ is conjugated to
WDSP's receive convention, preserving the same signed RF meaning as USB/LSB.
RIT translates the extractor without moving the dial. The RF NBP receives
the signed cuts; the post-detector bandpass receives 50 Hz to the larger
absolute cut with 4096 taps, removing the demodulated carrier before the AGC and PCM taps.
This `WdspChannel::AmReceive` recipe is opt-in, so other WDSP owners retain
their existing AM behavior. The analytic post-filter outputs are averaged to cancel opposite Hilbert
components, yielding the original real audio waveform in both mono channels
before independent PCM delivery. SAM mode selection is real coherent detection,
not an envelope alias. There is no asserted lock indicator.

The existing transaction placement treats AM/SAM as converter-DC-sensitive.
Explicit follow tuning and recentering prefer a legal clear capture; free
browsing does not drag stations into capture or move them to compensate.
Converter DC suppression at the wanted RF carrier remains destructive and
is distinct from audio DC removal. A muted USB bootstrap preserves the saved
capture center and reserves the first restorable saved ID, including sparse
singleton IDs and invalid earlier records.

## Receiver controls and state

AGC mode/threshold/off gain, NB/NB2/fill, NR, ANF, MN, tracking notches and RIT
use the existing accepted recipe. MN maps the audio position from 300 Hz to
200 Hz below the wider RF cut (at least 300 Hz), with 400 Hz paired RF
notches; it does not place a notch on the carrier. Absolute tracking notches
keep their RF meaning, including a notch deliberately placed on a carrier.
APF remains CW-only. AM/SAM squelch is not exposed. Advanced radio-specific
algorithms remain unavailable.

Each slice remembers AM and SAM filter cuts separately, like its other modes.
A first entry uses the mode default; later entries recall only accepted cuts,
including after reconnecting. Graph changes,
parking, receiver recreation and mode transitions invalidate the matching
receiver/presentation epochs. Speaker monitor controls remain downstream of
independent TCI/DAX, selected CW-decoder and selected native-ASR routes.
Normal launches admit one receiver; 2/4/8 receivers remain process-only
evaluation with no sustained-capacity qualification implied.

## Validation

`rtl_am_sam_pipeline_test` drives analytic modulated IQ through production
RF extraction, WDSP and receiver PCM. Assertions cover actual modulation
level, harmonic distortion, DC suppression, carrier offsets, quadrature
rejection that distinguishes SAM from envelope AM, asymmetric RF cuts, RIT,
dropout/fade recovery, retune/park/recreate epochs and eight mixed
AM/SAM/USB/LSB/CW/CWR/FM receivers. Receiver gain/noise/notch controls are
measured on audio, not inferred from a successful setter.

`rtl_model_acceptance_test` drives public SliceModel controls and injected
USB through the production worker to verify adoption, refusal, sparse
restore, persistence and parking. Native ASR regressions include AM and SAM.
These are offline checks; a candidate still requires operator listening and
known RF stimulus before making a hardware intelligibility claim.
