# aetherd — Yaesu FT-991 backend design

Status: implemented (first cut)
Family id: `ft991`
Protocol authority: the **Yaesu FT-991/FT-991A CAT Operation Reference
Manual**, published by Yaesu on its own download site — the vendor's complete,
open specification of the serial command set this backend speaks (`FA`, `MD`,
`TX`, `SM`, `PC`, `GT`, `AI`, `ID`, …). The audio plane is the radio's
built-in **USB Audio CODEC**, a standard USB Audio Class device that needs no
vendor protocol at all; the serial plane rides the radio's built-in Silicon
Labs CP2105 dual UART (the *Enhanced* COM port carries CAT). No undocumented
traffic is spoken: every byte this backend sends appears in the CAT manual.

## What the device is

A 100 W HF/50/144/430 MHz transceiver that demodulates and modulates
**inside the radio**. What reaches this host is:

- **RX**: the demodulated channel audio (≤ ~3.2 kHz wide) on the USB codec;
- **TX**: baseband transmit audio we play into the codec (the radio's
  DATA-USB path), plus CAT `TX1;`/`TX0;` for keying;
- **CAT**: a polled command plane at 4800–38400 baud, 8N2.

There is no IQ and no wideband data. That makes this the first backend whose
"panadapter" is honest about being a **lens on one channel**: an audio-band
FFT mapped onto RF using the dial frequency and the mode's sideband. The
waterfall covers the audio passband (0–4 kHz of RF around the dial), not a
band. Radio control, the host DSP chain (NR2/RN2/NR4/DFNR — the radio
declares `hasRadioSideDsp=false`, so the host modules are the operator's DSP),
and the TCI server all work exactly as on any other seam backend.

## Shape of the backend

The ColibriNANO backend is the structural template (one receiver, one pan,
authoritative local state, I/O-thread device object), with the DSP chain
*subtracted* — the radio already demodulated — and a transmitter *added*:

- ONE slice, ONE pan, fixed: pan id `ft991-0`, slice id 0. The slice
  frequency IS the radio's dial (VFO-A). `createPanadapter()` stays `false`.
- `canTransmit=true`: keying is CAT `TX1;`/`TX0;`, always and only from
  `setKeying()`/`setTune()` — the engine TX guard sits above the seam
  (Principle VI: nothing here keys on restore, connect, or discovery).
- `hostModulates=true`. Strictly the RF modulator is in the radio, but what
  the capability actually gates — the mic-source list, the PC-audio lock,
  TCI's TX routing — is *where the TX audio comes from*, and that is this
  host: `submitTxAudio()` receives the engine's processed TX audio and plays
  it into the codec. `setTune()` generates its own 1.5 kHz tone at the
  operator's TUNE power (with the `PC` drive swapped and restored around it).
- `clientSettingsDomains` is **EMPTY**. The FT-991 persists its own VFO,
  mode and settings across power cycles — it is radio-authoritative
  (Constitution II/III), like the Flex and unlike HL2/Colibri. The client
  must restore nothing.
- The CAT plane is polled (`AI0;` is set at connect; auto-information is
  deliberately not used so every state read is an explicit, parseable
  query/response pair): `FA`/`MD0`/`TX` at a few Hz, `SM0` for the S-meter,
  `PC`/`GT0` slowly. Turning the radio's own dial therefore updates the
  slice, the pan window and TCI within a poll period — the radio is the
  authority; the client follows.

## The panadapter is a lens (and says so)

`Ft991Spectrum` runs a real-input FFT over the codec audio and keeps the
bins from 0 to `kAudioSpanHz` (4 kHz). The window is **symmetric about the
dial** — `[dial−4k, dial+4k]`, centre == dial — and the backend maps audio
frequency `a` onto it per the mode's sideband:

- USB-side modes (USB, DIGU, CW, RTTY-U displayed as DIGU): `RF = dial + a`
  fills the RIGHT half; the left half is padded at the frame's own
  measured floor plus a small deterministic ripple. Both halves of that
  choice were forced by real failures: a sentinel far below the floor
  (−160 dBm) poisons the display's auto-level statistics, and a *constant*
  pad is exactly the clipped-floor signature SpectrumWidget's headroom
  recovery hunts (long identical runs at the frame minimum) — it ratcheted
  the range down 24 dB/s forever.
- LSB-side modes (LSB, DIGL, CWL): `RF = dial − a` fills the LEFT half,
  bins reversed; right half padded.
- AM/FM (envelope audio, no sideband identity): mirrored onto BOTH halves.

Symmetric-about-the-dial is load-bearing, not cosmetic. The first cut put
the dial at the window's left EDGE (window `[dial, dial+4k]`), which
collided with the GUI's slice-follow policy: it re-centres the pan onto
the active slice, the backend mapped that centre back to `dial − span/2`,
and the pair recursed — synchronously, through the seam, walking the REAL
radio down 2 kHz per round via CAT until the stack overflowed. With
centre == dial the re-centre request is the identity and dies in the
change gate. A depth guard in the tune verbs (`kMaxTuneVerbDepth`) remains
as a tripwire: any future policy loop truncates and logs instead of
crashing.

`panCenterBandwidthChanged` always reports exactly the window the bins
cover (the #4142/#4470 honesty rule — the padded half is *reported* as
covered and carries the explicit below-floor pad), and
`panBandwidthLimitsChanged` reports min == max == 8 kHz so the zoom clamp
knows there is nothing to zoom. `setPanBandwidth` re-emits the unchanged
span (snap-back); `setPanCenter` retunes the dial (moving the window IS
retuning the radio). CW is mapped like USB *without* a pitch correction,
deliberately: the slice filter overlay uses the same audio-offset
convention, so a tuned-in CW station falls inside its displayed passband.

Levels are dBFS of the codec stream plus one constant (`kFullScaleDbm`),
uncalibrated by construction — and worse than on an IQ backend, because the
radio's own AGC sits in front of the codec and moves the floor. Accepted
and documented: this display is for seeing the channel, not measuring it.

## Threading

The Colibri discipline: the backend owns a `QThread` (`ft991-io`);
`Ft991Device` — the QSerialPort, the QAudioSource/QAudioSink, the FFT and
the resamplers — lives on it. Qt delivers serial and audio readyRead on the
owning thread, so unlike the Colibri DLL there is no foreign-thread hop.
Control verbs travel GUI → I/O via queued `invokeMethod`; counters the GUI
reads (`linkStats()`, `healthSnapshot()`) are atomics in the device. Audio
and spectrum come up as queued signals carrying `std::vector<float>`.

## Audio plane

- **RX**: the codec is opened at its preferred format (the WfmDemodulator
  rule: capture at the device's native rate; forcing one makes the OS mixer
  resample). Channel 0 is converted to float, linearly resampled to 24 kHz
  (AudioEngine's native RX rate — 48 kHz native makes this an exact 2:1),
  shaped by the **slice passband** (a host-side 4th-order Butterworth
  biquad cascade — the radio already demodulated, so this is the only
  place the app's filter handles can narrow anything; wide-open edges are
  skipped, and the spectrum is deliberately fed PRE-filter, like a real
  pan), then duplicated to interleaved stereo and emitted as
  `audioFrameReady` + `sliceAudioFrameReady(0, …)` — the latter is what
  feeds TCI channel 1 (the Colibri-proven route). The filter shapes both
  feeds (the Flex semantic: DAX carries the slice's filtered audio); the
  speaker copy additionally applies the slice mute/gain/balance, and the
  per-slice copy is pre-fader, so muting the monitor does not stop a
  decoder. The host DSP chain (NR2/RN2/NR4/BNR/DFNR, client RX EQ) then
  applies in AudioEngine exactly as on HL2/Colibri.
- **TX**: `submitTxAudio()` (int16 interleaved stereo at the engine's rate)
  is mixed to mono, scaled by the MIC gain, resampled to the codec output's
  rate and written to the QAudioSink. Frames arriving while the radio is
  not keyed by us are dropped — silence into a live SSB transmitter is no
  RF, but the gate costs nothing and keeps the path inert when unkeyed.

The device selection is a case-insensitive substring match on the device
description ("USB Audio CODEC" by default — what the FT-991 enumerates as),
overridable in `Ft991Settings` for stations with more than one codec.

## Connect sequence

open serial (8N2) → `AI0;` → `ID;` (must answer `0670`, the FT-991's CAT
identity — anything else is a refused connect naming what answered) →
`FA;` + `MD0;` (so the first pan/slice emission carries the radio's real
state, not a placeholder) → open codec in/out → `opened()` → the backend
emits pan geometry, zoom limits, slice state, meters. No reply within the
retry budget → `openFailed` with the usual suspects named (wrong port, menu
031 CAT RATE mismatch).

## Persistence

`Ft991Settings`, one root key `"Ft991"` (Principle V): `baudRate` (default
38400 — set menu 031 CAT RATE to match), `audioInHint`, `audioOutHint`.
Radio identity for `radio_settings` scoping is the discovery serial
(`ft991-<port>`); with `clientSettingsDomains` empty there is no operating
state to scope, and `persistsMemories=false` engages the client-side memory
bank as usual.

## Discovery

`Ft991Discovery` polls `QSerialPortInfo::availablePorts()` on a timer and
lists ports whose description/manufacturer matches the radio's CP210x
bridge (skipping the CP2105's *Standard* port — CAT lives on *Enhanced*).
Each match emits the same `RadioInfo` shape as the Colibri sweep
(`family="ft991"`, synthetic LocalHost address, serial `ft991-<port>`).
The match is a heuristic — any CP210x device is listed — and that is fine:
listing is an offer, not an action, and connect verifies `ID;` before
anything else happens.

## Radio-side DSP (second cut)

The radio's own DSP is driven through the seam:

- **Filter width** — the slice passband handles act TWICE: the host biquad
  cascade applies the exact edges instantly, and the radio's DSP width
  follows via `NA` (narrow bank — chosen FIRST, the manual's order) + `SH`
  (nearest index in the per-mode tables: SSB 200–3200 Hz in 21 steps,
  CW/RTTY/DATA 50–3000 Hz in 17; AM/FM are fixed-width and take no SH).
  The confirm-poll reflects what the radio took back into the displayed
  edges (anchored at 1500 Hz for SSB/DATA, 700 Hz for CW, signed per
  sideband) — the display converges on radio truth. Turning the radio's
  own WIDTH knob updates the slice the same way (~5 s reflection cadence).
- **NB / NR / ANF** — the VFO DSP buttons route through the new seam verbs
  `setSliceNoiseBlanker/NoiseReduction/AutoNotch` (see below) onto CAT
  `NB`+`NL` (level 0–10), `NR`+`RL` (DNR level 1–15) and `BC` (the DNF
  auto notch). Levels stay 0–100 in the slice model and are mapped at the
  boundary; an echo only rewrites the UI value when it lands on a
  DIFFERENT radio step, so the coarse radio scale cannot yank a
  fine-grained slider.
- **Manual IF notch** (`BP`) — `setSliceManualNotch`. The seam's position
  is 0..100 ACROSS THE PASSBAND rather than a frequency, so the notch
  tracks the filter instead of being re-derived by every caller; this
  radio places it in audio Hz, and the backend converts in both
  directions, where it knows its own passband.

All of this rides the seam's EXISTING receive-DSP verbs
(`setSliceNoiseReduction`, `setSliceNoiseBlanker`, `setSliceAutoNotch`,
`setSliceManualNotch`) — this backend adds none of its own. The capability
tiers say exactly what this radio is: `hasRadioSideDsp` true (it runs its
own NR/NB/ANF), `hasLmsNoiseFilters` false (nothing resembling
NRL/ANFL/ANFT), `hasManualNotch` true. That is the same shape an Icom
declares, and it is what keeps three Flex-only buttons from lighting up
over registers this radio does not have.

## Clarifier (RIT / XIT)

`setRitEnabled`, `setXitEnabled` and `setRitOffset`. This radio has **one
shared clarifier register with two independent enables** — precisely the
shape the seam already models, so `setXitOffset` is deliberately left to
its default (which routes to `setRitOffset`) rather than overridden: on
this radio the aliasing is the truth, not an approximation.

The wire has no absolute-offset command, so setting one is "clear then
step": `RC;` zeroes the shared offset and `RU`/`RD` walk it to the
target, written as one frame pair so nothing observes the intermediate
zero. Readback is `IF;` — the only command that reports the clarifier at
all — parsed at fixed field offsets (frequency 5..13, clarifier 14..18,
RIT 19, XIT 20, mode 21) and polled about once a second.

## Pan centre: drag versus zoom

The lens is slaved to the VFO, so `setPanCenter`'s `PanCenterIntent` is
load-bearing here rather than ignorable: a `Drag` is the operator asking
to listen elsewhere and retunes the dial, while a `Range` centre riding
along with a zoom is answered with the geometry we already have. Treating
them alike would walk the radio across the band one zoom click at a time.

## Split — blocked, and on what

Not implemented, and not implementable behind the current seam. The
FT-991's split is VFO-A receives / VFO-B transmits, but AetherSDR models
split as *a second slice marked TX* — a Flex shape. With `maxSlices = 1`
the SPLIT button's handler returns early, and the `slice create` it would
otherwise send is Flex wire text that no seam backend receives. Wiring it
up needs a slice-lifecycle verb on `IRadioBackend` (there is
`createPanadapter`, but no `createSlice`), which is a family-neutral
addition well beyond this backend. Until then the SPLIT badge is a silent
no-op here — the same ungated-control shape as `+TNF`, and worth fixing
in the same family-neutral pass.

## Losing the link

A serial `ResourceError`/`ReadError` (USB cable pulled), ten consecutive
unanswered CAT queries (~5 s — the radio switched off), or a capture
`IOError` all funnel into `failLink()`: the device tears itself down and
emits `linkLost(reason)`, and the backend answers with `disconnected()`
followed by `connectionError(reason)` — that order, so the session state
is consistent before the reason is displayed. Without this the session
was a zombie: no `readyRead` ever again, nothing surfaced, and only the
heartbeat quietly going amber.

## TX meters

While `txState != 0` the S-meter poll is replaced by alternating `RM5`
(PO) and `RM6` (SWR), published as `TX:FWDPWR` (watts) and `TX:SWR`.
Both scalings are UNCALIBRATED linear estimates of the 0–255 raw count —
the CAT manual publishes no calibration — so the SWR reading is a
*trend*, not a measurement. Treat 1.0 as "meter at rest", not as a
matched antenna.

## Not in this cut (documented so nobody debugs their absence)

- RF gain (`RG`), the radio's CONTOUR/APF: not driven. The AGC threshold
  is display-only; AGC *mode* does map (`GT0x`). Host filtering narrower
  than the radio's filter cannot recover AGC pumping from strong signals
  the radio still passes to its own AGC — set the radio width down (the
  filter handles now do) when a neighbour pumps.
- TX meters (PO/SWR/ALC via `RM`), split/VFO-B, memory channels, clarifier.
- `AI1;` push mode (polling is the contract; see above).
- The `+TNF` button and TNF context-menu entries are Flex-only and today
  UNGATED repo-wide — dead on every seam backend (HL2/Colibri too), the
  known HERMES §17 shape. Gating them behind a capability is a separate,
  family-neutral fix.
