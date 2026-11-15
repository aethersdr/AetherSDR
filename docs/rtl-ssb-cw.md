# RTL SSB and CW reception

USB, LSB, CW and CWR use the existing shared-capture extractor, prepared
receiver registry and WDSP channels. Their independent slice PCM and speaker
mix are 48 kHz. There is no new worker, decoder engine or dependency. The
normal one-receiver admission and process-only 2/4/8 evaluation limits are
unchanged; mode support is not sustained-capacity qualification.

## Frequency and filter contract

USB filters are positive relative to the displayed carrier; LSB filters are
negative. CW/CWR filters straddle the displayed carrier. The dial and persisted
filter describe RF, not an audio-shifted station frequency. CW translates the
carrier to positive pitch and CWR reverses the detuning sense. The extractor
translation and WDSP filter shift cancel at the RF boundary, including RIT.
CW pitch is session-wide, while RIT and receive DSP belong to each receiver.

RTL analytic IQ is conjugated at the native sideband receiver boundary to
match WDSP's receive convention. FM/FM-N/WFM keep their existing orientation
and demodulator recipes. AM/SAM use the same native bank; their carrier and
audio-filter contract is described in [AM/SAM reception](rtl-am-sam.md).

All affected receiver controls are prepared and adopted before readback or
persistence. Invalid controls or failed preparation retain the prior accepted
state. Parked receivers retain their settings and supply no PCM. Follow-tuning
CW/CWR prefers a legal DC-clear capture placement; free browsing does not drag
parked stations back into capture.

## Control ownership

`ReceiveDspControl` declares the supported feature and mode combinations;
absence preserves other radios' existing capability semantics.
`ReceiveCwPitchControl` enables the existing pitch verb and confirmed pitch
readback without enabling transmission or host microphone modulation. A
control the receiver does not support is dimmed with a reason.

AGC, blanking, filtering and peaking run inside the individual receiver. Client
AetherDSP processing remains on the presentation stream. Similar control
names do not imply identical proprietary-radio DSP algorithms or calibrated
RF levels.

## Receiver notches and noise processing

NR and ANF use the existing WDSP adaptive filters; MN is a 400 Hz local
notch positioned across the signed receive passband. The shared tracking-notch
bank holds up to 32 RF-frequency notches, each 200–12,000 Hz wide. Its
frequency conversion includes receiver carrier, RIT and signed CW pitch.
Only overlapping notches reach each prepared receiver filter. Global TNF
bypass and manual-notch enable are independent. The bank is persisted with
the RTL settings and adopted transactionally, including parked receivers.
There is no radio-stored permanence or selectable Flex notch-depth algorithm.

## Decoder and transcription source

The existing selected-receiver decoder route resamples native 48 kHz PCM to
the CW decoder's 24 kHz contract. CW and CWR both enable the decoder and use
the appropriate zero-beat direction. Speaker mute and gain do not affect the
decoder feed. Selection, retuning, parking and retired PCM leases invalidate
queued audio from the old source.

Copy Assist uses the selected native receiver before speaker controls and
client DSP. Its existing presentation pre/post-DSP preference is preserved
for other radios. The settings control states when that choice is unavailable.
ASR carries a source lease and generation through queued input, inference and
transcript delivery, so text from a retired source cannot enter a new session.
Flex retains its existing presentation route.

## Validation

`rtl_ssb_cw_pipeline_test` feeds analytic generated IQ through the production
extractor and WDSP channels. It checks sideband rejection, passband cuts,
CW/CWR pitch and RIT sense, gain and peaking, and keyed RF through the actual
CW decoder. Its optional external-IQ fixture exports demodulated speech for
`asr_whisper_backend_test` with an explicitly supplied local speech model.

Model tests cover accepted state, refused requests, persisted restore,
parking and typed intents. Native ASR tests cover selected-source routing and
retired input/results. These are offline proofs. A testable build still needs
operator listening tests, a known RF stimulus and representative multi-receiver
continuity measurements; finite PCM alone does not establish intelligibility.
