# Icom CI-V Backend — Design Note

Bring-up plan for `IcomCivBackend`, an `IRadioBackend` implementor for Icom
networked radios. First target: **IC-705 over WiFi**.

The protocol reference is the oracle at `~/oracles/icom/icom-oracle.md`, with
primary sources under `~/oracles/icom/sources/`. This note does not restate the
wire format; it covers what AetherSDR has to build and in what order.

Companion to `aetherd-hl2-backend-design.md`, and deliberately shaped like it.

---

## 1. Why Icom is a different kind of backend

The two backends we have bracket the design space, and Icom sits between them in
a way neither anticipated:

| | Flex | HL2 | **Icom** |
|---|---|---|---|
| Demodulation | radio | host | **radio** |
| FFT / spectrum | radio | host | **radio** |
| Raw IQ available | yes (DAX-IQ) | yes (it's all we get) | **no** |
| Command plane | text over TCP | registers in the IQ stream | **CI-V over UDP** |
| State push | full status subscription | telemetry in-band | **CI-V Transceive, partial** |
| Meters | VITA-49 stream | in-band telemetry | **polled, one at a time** |

Flex is *radio-authoritative and rich*: it tells you everything, unprompted.
HL2 is *host-authoritative and raw*: it tells you almost nothing and hands you
samples. Icom is **radio-authoritative and poor** — the radio owns the DSP, the
demodulation and the FFT, but its only way to tell you anything is a 1980s
request/response bus that also carries every command you send.

That combination produces the two structural facts this whole design turns on:

**(a) There is no IQ.** Not over WiFi, not over USB, not at all — confirmed
against Icom's own CI-V Reference Guide, wfview's type system, and the transport
itself (oracle §8.1). The panadapter is a cooked 475-bin, 0–160 display array at
one of eight fixed spans. Everything downstream of raw IQ is unavailable:
host-side FFT sizing, arbitrary zoom, DAX-IQ consumers, CW Skimmer.

The goal as written asks for "control/voice/data/IQ/panadapter". **Four of those
five are achievable and IQ is not.** The panadapter is real and good; the IQ line
item should be struck rather than shipped as a disabled control.

**(b) Meters are polled and share a stream with tuning.** There is no meter
plane. Every S-meter reading is a round trip on the same UDP stream carrying
frequency changes, and on WiFi that is 5–30 ms. This is the first backend where
*metering policy* is a real engineering constraint rather than a subscription.

---

## 2. What `IcomCivBackend` owns

```
src/core/backends/icom/
  IcomCivBackend.{h,cpp}      IRadioBackend implementor; owns the others
  IcomSession.{h,cpp}         RS-BA1 session: handshake, login, token renewal
  IcomStream.{h,cpp}          one UDP stream: seq, ARQ, keepalives (×3 instances)
  IcomSeqBuf.{h,cpp}          reorder + replay buffers (the ARQ layer)
  CivCodec.{h,cpp}            CI-V framing, BCD codecs, frame reassembly
  CivCommands.h               the command table (§4.3 of the oracle)
  IcomScope.{h,cpp}           0x27 waveform decode → spectrum frames
  IcomAudio.{h,cpp}           codec 4 LPCM, the 1364+556 split, resampling
  IcomMeters.{h,cpp}          calibration curves + the poll scheduler
  IcomModels.h                per-model capability table (IC-705 first)
```

`IcomStream` existing three times — control, serial, audio — with independent
sequence spaces and ARQ state is the shape the protocol dictates, and it is worth
resisting the urge to collapse it. kappanhang's `streamCommon` is exactly this
and it is the cleanest part of that codebase.

---

## 3. Seam mapping

### Intents DOWN

| `IRadioBackend` | Icom mechanism |
|---|---|
| `setSliceFrequency` | CI-V `05` (set operating frequency), 5-byte LE BCD |
| `setSliceMode` | CI-V `06` (mode + filter) |
| `setSliceFilter` | CI-V `1A 03` (IF filter width) — **discrete FIL1/2/3, not continuous** |
| `setSliceAgc` | CI-V `16 12` — FAST/MID/SLOW only; **no threshold**, ignore `thresholdDb` |
| `setPanCenter` | CI-V `05` in Center mode; `27 1E` edges in Fixed mode |
| `setPanBandwidth` | CI-V `27 15` — **snaps to one of eight spans**; report what was taken |
| `setPanRfGain` | CI-V `16 02` preamp — **three positions**, see §5 |
| `setPanFrameRate` | CI-V `27 1A` sweep speed 0/1/2 — mapping to Hz is unmeasured |
| `setKeying` | CI-V `1C 00` (00=RX, 01=TX) |
| `setTune` | **no direct command** — see below |
| `setTxPower` | CI-V `14 0A`, 0000–0255 BCD |
| `setTxFilter` | CI-V `1A 05 0020/0021/0022` — **discrete WIDE/MID/NAR**, not Hz |
| `submitTxAudio` | audio stream, codec 4 — **requires DATA MOD = WLAN** |
| `setSliceAudioGain` | CI-V `14 02` (AF level) |
| `createPanadapter` | `false` — one receiver, one scope |

Three of these do not fit the seam cleanly, and all three fit the same pattern:
**the verb is continuous and the radio is discrete.** `setSliceFilter`,
`setPanBandwidth` and `setTxFilter` all take Hz and can only snap. The seam
already anticipates this — `setPanBandwidth`'s own comment says "hz is a REQUEST"
and the result comes back via `panCenterBandwidthChanged`. Follow that contract
exactly: take the request, snap, report what happened. Do not clamp silently.

`setSliceAgc`'s `thresholdDb` has nowhere to go. Ignoring a parameter is the
right call over inventing a mapping, but it should be a documented no-op with a
comment, not a silent drop.

**`setTune` has no Icom command, and the obvious candidate is a different
feature.** `1C 01` is the *antenna tuner* status (`00`=OFF, `01`=ON, `02`=Tune) —
it starts an ATU matching cycle, not a tune carrier. AetherSDR's `setTune(on,
tunePowerPercent)` means "raise a steady carrier at the operator's tune power",
which on an Icom is composed rather than commanded: save the mode, set RTTY or
CW, apply the tune power via `14 0A`, key with `1C 00`, and restore on release.

Those are two genuinely different operations and they must not be conflated —
`1C 01 02` belongs on the tuner extension path (`TunerModel`'s autotune intent),
not on `setTune`. A backend that wires the ATU cycle to the TUNE button gives the
operator a button that does nothing on a radio with no ATU attached, and does
something unexpected on one with an AH-705.

### State UP

| Signal | Source |
|---|---|
| `sliceChanged` | CI-V Transceive pushes + polled `03`/`04` |
| `panCenterBandwidthChanged` | the `27 00` waveform header carries centre+span or edges |
| `spectrumFrameReady` | `27 00` waveform data, 475 bins → float (see §6) |
| `waterfallRowReady` | derived from the same frame |
| `audioFrameReady` | audio stream, decoded to the engine's PCM format |
| `sliceAudioFrameReady` | same buffer — one slice, so they are the same stream |
| `meterUpdate` | polled `15 xx` through the calibration curves |
| `transmitChanged` | `1C 00` echo + TX meters |
| `radioChanged` | `19 00` model, firmware, connection state |
| `linkStatsUpdated` | per-stream counters + `0x07` ping RTT |
| `healthSnapshot` | OVF (`15 07`), Vd, Id, retransmit and loss counters |

`sliceAudioFrameReady` and `audioFrameReady` carrying the same buffer is correct
here and worth a comment in the code — with one receiver there is nothing to
un-mix, and a future reader will wonder if it is a bug.

---

## 4. Capabilities `IcomCivBackend` advertises (IC-705)

```cpp
caps.family                 = "icom";
caps.model                  = "IC-705";        // from CI-V 19 00, never hardcoded
caps.maxSlices              = 1;
caps.maxPanadapters         = 1;
caps.tuningMinHz            = 30e3;
caps.tuningMaxHz            = 470e6;           // with gaps; see note
caps.canTransmit            = true;
caps.txPowerMaxWatts        = 10.0;
caps.hostModulates          = false;           // the radio modulates
caps.hasRadioSideDsp        = true;            // NR/NB/notch are 16 xx, in firmware
caps.hasTuner               = false;           // no INTERNAL ATU; see note
caps.hasSupplyVoltageTelemetry = true;         // 15 15 Vd
caps.hasDaxStreams          = false;           // NO IQ — see oracle §8.1
caps.hasGpsLocation         = false;           // GPS exists, protocol won't carry it
caps.hasProfiles            = false;
caps.hasWaveforms           = false;
caps.hasMultiClientSessions = false;
caps.hasRadioSideWaterfallAutoBlack = false;
caps.persistsMemories       = false;           // radio has 99; not phase 1 — see §8
caps.canReboot              = false;           // see note
caps.clientSettingsDomains  = {};              // radio remembers its own state
```

**`hasTuner = false` is a judgement call, not a fact.** The IC-705 has no
internal ATU, but `1C 01` controls an *external* AH-705 — and there is no command
to detect whether one is attached. So the capability is unanswerable from the
radio. False is the safer default (no tuner UI on a radio that probably has
none); an operator with an AH-705 is better served by an explicit setting than by
a control that appears unconditionally and silently fails.

**`canReboot = false` despite `18 00` / `18 01` existing.** Those turn the
transceiver off and on — but over WiFi, powering off drops the WLAN interface,
so the `18 01` that would bring it back has no path to reach the radio. The pair
is usable on a wired CI-V bus and is a one-way trip over the network. Advertising
a reboot the operator cannot recover from is worse than not offering it.

**`clientSettingsDomains` empty is the load-bearing line.** Unlike the HL2 — where
the radio reports no VFO and the app must be authoritative — an Icom remembers
its own frequency, mode and filter across power cycles and reports them on
request. Constitution II/III then says the client must not re-assert them. This
backend reads state at connect; it does not push a restored state.

`tuningMaxHz` is a simplification: the IC-705 covers 0.03–470 MHz with gaps
(no 148–430 receive on some regional variants). The seam has no gap
representation, so the honest thing is the outer envelope plus a rejected-tune
path that reports what the radio actually did.

---

## 5. The structural gaps Icom forces

### Gap A — RF gain has no continuous range

`setPanRfGain(panId, gainDb)` assumes a dB register. The IC-705 has a
three-position preamp (`16 02`: OFF / P.AMP1 / P.AMP2, and only OFF/ON on
144/430) and no continuous gain.

The seam already has the escape hatch: `panRfGainInfoChanged(panId, low, high,
step)`. Emit `(0, 2, 1)` and let the existing slider snap to three detents. That
is precisely what that signal was added for — the HL2 case in its comment is the
same shape (a Flex-derived range that did not match the hardware), and reusing it
avoids a per-family special case in the UI.

**Do not advertise a fabricated dB range.** A slider that moves smoothly over a
control with three positions is the "the control moves, the audio is unchanged"
failure the capability comments keep warning about.

### Gap B — metering is a scheduler, not a subscription

This is new. Flex streams meters; the HL2 embeds them. Icom needs a **poll
scheduler** that:

- polls only meters currently visible in the UI;
- runs S-meter at ~10 Hz and everything else at ~5 Hz;
- stops TX meters entirely while receiving, and RX meters while transmitting;
- yields to user-initiated commands, so tuning never queues behind metering;
- filters its own request/response traffic out of anything re-exported (CAT
  pass-through, TCI) — kappanhang does exactly this and it matters.

wfview's per-rig `Periodic\N\Command` list with priorities is the proven shape.
This should be a named component (`IcomMeters`) with its own test, not a timer
sprinkled through the backend.

### Gap C — the seam's audio contract is 24 kHz stereo, and the radio is 48 kHz mono

Not an Icom problem — a seam fact that is nowhere written down, and that this
backend is the third to have to rediscover.

Everything downstream of `sliceAudioFrameReady` consumes **interleaved stereo
float32 at 24 kHz**. The evidence is spread across three files and no single one
states it:

- `Hl2RxDsp::audioReady(const std::vector<float>& stereoPcm)` — the parameter
  name is the only declaration of channel order.
- `Hl2RxDsp::Config::audioSampleRateHz = 24000` — the only declaration of rate.
- `TciServer::onDaxAudioReady` divides by `2 * sizeof(float)` for the frame
  count and constructs `Resampler(24000.0, cs.audioSampleRate, …)` — the only
  place the two facts appear together, and it is in the consumer.

The Icom delivers **48 kHz mono**, because that is what the RS-BA1 stream
negotiates. So the backend owns a conversion, and **both halves of it are
load-bearing**:

| Skipped | Symptom |
|---|---|
| Rate conversion | Playback runs an octave low. WSJT-X sees every tone at twice its frequency and decodes nothing. |
| Channel duplication | `TciServer` divides by `2 * sizeof(float)` and sees half the frames it has. |

Both failures are **silent** — audio flows, meters move, the session is healthy.
That is why `icom_backend_test` asserts the ratio (4800 mono samples in at 48 kHz
→ ~2400 stereo frames out at 24 kHz) rather than merely asserting that audio
arrived, and why it also asserts the *negative*: a passthrough would emit ~4800
frames, so the test fails a backend that skipped the conversion.

`Resampler::processMonoToStereo` does both halves in one call. It is stateful
(r8brain), so the instance is built once at connect — a fresh one per callback
restarts the filter history every block, which is audible as a periodic tick.

**One TCI channel, and that is the whole requirement for WSJT-X.** Slice 0 →
DAX channel 1 → TRX 0, via the existing `MainWindow_Session` wiring
(`onDaxAudioReady(sliceId + 1, pcm)`). Nothing Icom-specific is needed in
`TciServer`; the backend only has to emit the right bytes and publish a slice
for the routing to resolve against.

### Gap D — the scope is not calibrated

`spectrumFrameReady` carries float dBm on the HL2 path. The Icom scope is 0–160
display units relative to the `27 19` reference level, and Icom publishes no
calibration.

Follow the `Hl2DbReference` precedent: a named per-model offset, documented as an
estimate, anchored to the reference level read back from the radio, and
cross-checked against the **S-meter**, which *is* calibrated (0 = S0, 120 = S9,
241 = S9+60 dB). Put a known signal in the passband and compare.

Until that cross-check exists, the UI should not present the scope's Y axis as
absolute dBm. An honest relative scale beats a number that looks like a
measurement and is not.

---

## 6. Transport and codec commonality across models

Recorded because it is the question that decides whether "add a model" means a
table row or a second backend, and because the answer rests on inference rather
than on any specification.

### The UDP transport is the same protocol on every networked Icom

Four independent checks, all agreeing:

- **wfview has zero per-model branching in its UDP path.** `icomudpbase`,
  `icomudphandler`, `icomudpaudio` and `icomudpcivdata` contain no `modelID`
  test, no CI-V-address special case, and no mention of any model name. One
  implementation drives the IC-705, IC-9700, IC-7610, IC-785x and IC-7300MK2.
- **kappanhang** lists IC-705, IC-9700, IC-7610 and IC-785x as compatible with a
  single codebase.
- **The IC-7300MK2's own CI-V guide exposes the same three-port structure** —
  `1A 05 01 10 / 11 / 12` are Control Port (UDP), Serial Port (UDP) and Audio
  Port (UDP), alongside Network Control (`01 08`) and an Internet Access Line
  setting (`01 13`, FTTH / ADSL-CATV). That FTTH value is the same string the
  IC-705 returns in its login reply at offset `0x40`.
- **The scope division split is identical**: `01` over LAN, `11` over USB.

So `IcomStream` and `IcomSession` are expected to work against any of them
unchanged, and per-model variation is confined to `IcomModels`. **This is an
inference from convergent implementations plus a matching feature surface, not
a documented guarantee** — Icom documents the transport nowhere, for any model.
Treat a new model's first connection as a test of this claim.

### The codec negotiation is shared; codec ACCEPTANCE is unverified per model

The mechanism is unambiguously transport-level, not model-level: the client
chooses the codec in the conninfo packet (`0x72` / `0x73`, sample rate at `0x74`
/ `0x78`). Nothing about that is per-radio.

What is **not** established is which codecs a given radio accepts. Neither the
IC-705 nor the IC-7300MK2 CI-V guide mentions "codec" even once, and wfview
offers its full nine-entry codec list to every radio unconditionally — which
tells us wfview does not model per-radio codec support, not that every radio
supports all nine.

LPCM 1ch 16-bit at 48 kHz is what we negotiate, what kappanhang uses
exclusively, and what wfview defaults to. It is the safe common denominator and
should stay the default for any newly added model until someone proves
otherwise on that radio.

### Nothing here is an "air" protocol

Worth stating once because the phrasing recurs: all of the above is the LAN /
WiFi link. The over-the-air side — SSB, CW, FM modulation — happens entirely
inside the radio. With no IQ on any networked Icom, this backend never handles
anything airborne; it ships demodulated audio and receives a cooked spectrum.

---

## 7. Phasing

Each phase is independently shippable and independently provable.

**Phase 0 — the socket.** `IcomStream` + `IcomSession`: handshake, login, token
renewal, keepalives, ARQ. No CI-V, no audio. Proof: connects to an IC-705, stays
connected for an hour, `linkStats` shows RTT and zero loss. This is the phase
where the protocol is either right or wrong, and it is testable against a
recorded packet trace with no radio attached.

**Phase 1 — control.** `CivCodec` + the command table: frequency, mode, filter,
PTT, power. Proof: the automation bridge tunes the radio and reads it back; the
front panel follows. This is the phase that makes the backend *useful*.

**Phase 2 — panadapter.** `IcomScope`: enable `27 10` **and** `27 11`, decode the
single-packet WLAN waveform, emit spectrum and waterfall. Proof: a screenshot
with a real signal at a known frequency landing in the right bin.

**Phase 3 — audio.** `IcomAudio`: codec 4 LPCM 48 k mono, RX first. Then TX,
which needs DATA MOD = WLAN on the radio and **verification outside the system** —
a second receiver or a WebSDR, per `feedback-verify-outside-the-system`. A TX
path that looks perfect from inside AetherSDR and is silent on the air is the
exact failure mode this project has already been bitten by.

**Phase 4 — meters and health.** `IcomMeters`: the published calibration curves
plus a poll scheduler with four rules (visible-only, TX/RX split, one request in
flight, yield to user commands). The scheduler takes an injected clock so the
policy is provable in microseconds rather than by watching a radio. Proof on
hardware later: S-meter against a signal generator, Po against a wattmeter into
a dummy load.

**Phase 5 — breadth.** Model discovery via `19 00` and a per-model capability
table keyed by CI-V address. Only the IC-705 row is `verified`; every other row
says so, and the unknown-model fallback is deliberately conservative (no scope,
no transmit) because an unrecognised radio advertised as scope-capable wires a
panadapter to a command it may not implement.

**CI-V over a local serial port is DEFERRED, not cancelled.** It brings in every
non-networked Icom (IC-7300 and up) and is the strongest argument for the
`IcomCIV` name, which is why `CivCodec` is already transport-free — the increment
is a transport class, not a rewrite.

---

## 8. Clean-room provenance

**wfview is GPL-3.0 and AetherSDR cannot take code from it.** It is the best
reference available and it must be treated as a *specification*: read it, cite
it, do not paste it. This is the same rule the project already applies to
`pihpsdr` for the HL2 test fixture.

**kappanhang is MIT** and is the reference to port *from*, with attribution.

**Hamlib is LGPL-2.1.** Its meter calibration tables are data, but most of the
same curves are in Icom's own published guide — derive them from tier 1 and the
question does not arise.

Allowed inputs, in order: Icom's CI-V Reference Guides (facts, not text);
kappanhang (MIT, portable); wfview and Hamlib (read-only reference); packet
captures from our own radio.

---

## 9. Explicitly out of scope for phase 1

- **IQ.** It does not exist on this radio. Not deferred — absent.
- **Memory channels.** The radio stores 99 in 100 groups (`1A 00`) and the decode
  is large and fiddly. Ship `persistsMemories = false` (client-side bank) and
  revisit.
- **D-STAR / DV.** A large command surface (`22 xx`, `23 xx`) and a separate
  feature.
- **Bluetooth transport.** Unknown whether it carries all three streams.
- **Opus and ADPCM codecs.** LPCM first. They matter for WAN use, so this is a
  deferral rather than a dismissal.
- **USB transport.** Needs the 11-chunk scope reassembly the WLAN path avoids
  (implemented in `ScopeDecoder` already; the transport is what is missing).
- **Local serial CI-V.** Deferred, not cancelled — `CivCodec` is transport-free
  precisely so this stays a transport class rather than a rewrite. It brings in
  every non-networked Icom, the original IC-7300 included.
- **Reading the radio's UDP ports over CI-V**, and **remote power-on**. Both are
  IC-7300MK2 capabilities the IC-705 does not have. See §11.

Multi-model support is no longer on this list: `19 00` discovery and the
capability table are built, with the IC-705 and IC-7300MK2 both verified against
their own Icom CI-V guides.

---

## 10. Open questions needing a radio on the bench

Carried from oracle §12, because they gate specific phases:

1. **Scope frame rate over WLAN** — gates whether `setPanFrameRate` does anything.
2. **Does the scope update during TX?** — gates the phase-2/3 interaction.
3. **The real dBm offset**, per band and preamp setting — gates Gap C.
4. **Are `27 15` span changes echoed** when set on the front panel, or must they
   be polled? — gates whether the pan follows the operator's own zoom.
5. **Second-client behaviour.** The protocol has `busy` and `computer` fields;
   the IC-705's single-session response to contention is untested.
6. **Does an IC-7300MK2 answer on the LAN while in Standby?** This single
   question gates the remote power-on feature in §11 — and it is the one whose
   wrong answer is expensive, because a radio that shuts its interface down
   cannot be woken and has to be reached physically.

Answering 1–4 needs perhaps an hour with the radio and a packet capture, and
would remove most of the guesswork from phases 2 and 3. Question 6 needs an
MK2, which is a different radio from the one the rest of this targets.

---

## 11. Roadmap candidates

Not built, deliberately. Each is recorded here with what it needs so the
decision is not re-litigated from scratch.

### Read the radio's UDP ports over CI-V (IC-7300MK2 and later)

Today the backend assumes 50001 / 50002 / 50003 and, when the operator has
changed them, fails with a timeout that names the wrong cause — "no answer from
the radio" is indistinguishable from Network Control being off.

The MK2 exposes them: `1A 05 01 10` (Control), `01 11` (Serial), `01 12` (Audio),
each a three-byte BCD value covering 1–65535. `01 08` reads Network Control
itself, so a connected client could also report *definitively* that it is
disabled rather than guessing.

The catch is ordering: those are CI-V commands, and CI-V arrives over the serial
stream, which cannot open until the control stream's request has already
announced the ports. So this cannot bootstrap a first connection. What it can do
is **confirm and cache** them once connected, so a later reconnect uses the real
values and a mismatch is reported precisely. That is worth having and is a
smaller feature than it first looks.

**The IC-705 does not expose these at all** — they are menu-only there. So this
is per-model, gated on `IcomModel`, and another reason the capability table
earns its place.

### Remote power-on / reboot (IC-7300MK2)

`capabilities().canReboot` is currently **false for every model**, on the
reasoning that `18 00` powers the radio off, which drops the network interface,
so the `18 01` that would bring it back has no path. That reasoning is sound for
the IC-705 on WiFi and **may be too conservative for the MK2**.

The MK2 has `1A 05 01 09` — "Power OFF Setting (for Remote Control)": `00` = Only
Shutdown, `01` = Standby/Shutdown. And the IC-705's guide already documents that
`18 01` "turns ON the transceiver when the transceiver is OFF
(Standby/Shutdown)". A mains-powered radio with an Ethernet port plausibly keeps
its LAN interface alive in Standby, which is exactly the condition that makes
remote power-on work.

**Unverified, and the failure mode is bad**: a reboot the operator cannot
recover from strands the radio until someone walks to it. So this needs a bench
answer to one question — *does the MK2 answer on the LAN while in Standby?* —
before `canReboot` becomes true for it. If it does, the feature is
`setPowerOffMode(Standby)` plus a guarded `18 00` / `18 01` pair, and the
capability stays per-model.

---

## Appendix C — CI-V coverage audit

Every meter and switch the IC-705's CI-V guide documents, against what this
backend maps and what the UI actually consumes. Written after live testing found
five "broken" meters that were all publishing correctly at the seam.

**The dominant defect is not a missing mapping — it is a UNIT CONTRACT.**
`MeterModel` interprets a meter by NAME, with a unit it assumes rather than
reads. `TX:FWDPWR` is unconditionally treated as dBm and converted with
`10^(v/10)/1000`; `TX:ALC` is routed to `swAlcChanged(float dbfs)` and rendered
on a −20…0 dBFS gauge. A backend that publishes the honest unit from its own
radio — watts, percent — is silently mis-rendered. The `unit` field in
`MeterDef` is carried, displayed, and then ignored by the consumers that matter.

### C.1 Meters (`15 xx`)

| CI-V | Guide semantics | Mapped | Published as | Verdict |
|---|---|---|---|---|
| `15 01` | Noise/S-meter squelch open | `kSquelchStatus` | — | constant defined, never polled |
| `15 02` | S-meter, 0=S0 / 120=S9 / 241=S9+60 | ✅ | `SLC:LEVEL` dBm | **working** |
| `15 05` | Various squelch (tone etc.) open | ✗ | — | unmapped |
| `15 07` | ADC OVF indicator | `kOverflow` | `RAD:OVF` Percent | published, no consumer |
| `15 11` | Po, 0=0% / 143=50% / 213=100% | ✅ | `TX:FWDPWR` **Watts** | **BROKEN — seam wants dBm** |
| `15 12` | SWR, 0=1.0 / 48=1.5 / 80=2.0 / 120=3.0 | ✅ | `TX:SWR` SWR | **BROKEN — gated behind FWDPWR** |
| `15 13` | ALC, 0=min / 120=max | ✅ | `TX:ALC` **Percent** | **BROKEN — seam wants dBFS, so it pegs** |
| `15 14` | COMP, 0=0 dB / 130=15 dB / 210=25.5 dB | ✅ | `TX:COMPPEAK` dB | contract correct; reads 0 while PROC is unmapped |
| `15 15` | Vd, 0=0 V / 75=5 V / 241=16 V | ✅ | `RAD:+13.8A` Volts | **working** |
| `15 16` | Id, 0=0 A / 121=2 A / 241=4 A | ✅ | `RAD:PACURRENT` Amps | published, no consumer |

**There is no mic-level meter and no temperature meter in the CI-V set.** The
list above is complete. So the Phone/CW **Level** gauge (mic peak, dBFS) and
`TX:MICPEAK` can never move on this radio, and neither can `RAD:PATEMP`. Both
want hiding on a backend that owns its own microphone, not fixing.

### C.2 Functions (`16 xx`) — switches

| CI-V | Function | Mapped | Reaches a control |
|---|---|---|---|
| `16 02` | Preamp OFF/P.AMP1/P.AMP2 | ✅ | ✅ via `setPanRfGain` |
| `16 12` | AGC FAST/MID/SLOW | ✅ | ✅ via `setSliceAgc` |
| `16 22` | Noise blanker | ✅ | ✗ constant only |
| `16 40` | Noise reduction | ✅ | ✗ constant only |
| `16 41` | Auto notch | ✅ | ✗ constant only |
| `16 43` | Tone squelch | ✗ | ✗ |
| `16 44` | **Speech compressor (PROC)** | ✅ | ✗ **not wired — the PROC state disagrees with the radio** |
| `16 45` | Monitor | ✅ | ✗ constant only |
| `16 46` | VOX | ✅ | ✗ constant only |
| `16 47` | BK-IN OFF/SEMI/FULL | ✗ | ✗ **CW break-in unreachable** |
| `16 48` | Manual notch | ✅ | ✗ constant only |
| `16 4F` | Twin peak filter (RTTY) | ✗ | ✗ |
| `16 50` | Dial lock | ✅ | ✗ constant only |
| `16 56` | DSP IF filter SHARP/SOFT | ✗ | ✗ |
| `16 57` | Manual notch width W/M/N | ✗ | ✗ |
| `16 58` | SSB TX bandwidth W/M/N | ✗ | ✗ |

### C.3 Levels (`14 xx`)

Mapped: AF `01`, RF `02`, squelch `03`, NR `06`, CW pitch `09`, RF power `0A`,
mic gain `0B`, key speed `0C`, COMP level `0E`, NB level `12`, monitor `15`.

Unmapped: notch position `0D`, break-in delay `0F`, VOX gain `16`, anti-VOX
gain `17`.

**`14 0E` is the missing half of PROC.** AetherSDR's processor control is a Flex
shape — OFF / NOR / DX / DX+ — and on an Icom that is two commands, not one:
`16 44` for the on/off and `14 0E` (0000–0255 ⇒ 0–10) for which of the three.

### C.4 RIT / XIT (`21 xx`) — entirely unmapped

| CI-V | Function | Mapped |
|---|---|---|
| `21 00` | RIT frequency | ✗ |
| `21 01` | RIT ON/OFF | ✗ |
| `21 02` | ∂TX (XIT) ON/OFF | ✗ |

No constant, no builder, no call site. RIT and XIT are not wired in at all.

### C.5 IF filter — three, not more

The radio has exactly **FIL1 / FIL2 / FIL3**, selected in the mode command's
third byte. `filterForWidthHz()` snaps a width request onto them correctly, but
the UI still offers the full Flex step list, so most of its steps land on the
same three filters. The reachable set is a capability the backend should
publish, the same way the RF-gain control was narrowed to three preamp detents.

---

## Appendix D — Applet control inventory

Which controls on the surfaces the operator uses actually reach this radio.

**Method.** Two passes. The first traced wiring from source — a control is
"linked" when its intent reaches an `IRadioBackend` verb this backend overrides.
The second DROVE the controls against a live IC-705 and read the resulting CI-V
frames back through `civ trace`, checking each encoded value against arithmetic
rather than against an observed capture.

Rows marked ✅ **verified** were driven and their bytes checked. Rows marked
✅ implemented were traced but never driven — treat those exactly as the first
pass intended: the map of what CAN work, not evidence that it does.

### D.1 The structural finding — FIXED

**Most receive-DSP controls did not use the seam at all.** `SliceModel::setNr`,
`setAnf`, `setNb` and `setSquelch` emitted FlexRadio wire text:

```cpp
void SliceModel::setNr(bool on) {
    m_nr = on;
    sendCommand(QString("slice set %1 nr=%2").arg(m_id).arg(on ? 1 : 0));
}
```

On a Flex that string is the command. On every other backend it is discarded —
there is no `IRadioBackend` verb for any of them, so no backend can implement
one however much it wants to. The control moves, the model updates, the UI
agrees with itself, and the radio never hears about it.

This is the same shape as lesson 1.5 (the bridge is not the UI) one layer down,
and it was why `hasRadioSideDsp = true` bought nothing: the capability said the
radio's own firmware runs NR/NB/notch while the intents to drive them had
nowhere to go.

**Resolved.** `setSliceNoiseReduction` / `NoiseBlanker` / `AutoNotch` / `Squelch`
now exist on the seam, `SliceModel` emits operator-intent signals alongside the
Flex wire text (so Flex is unchanged), and `RadioModel` routes them for any
non-Flex backend. The audio setters (`setSliceAudioGain` / `Mute` / `Pan`) have
verbs and this backend still does not implement them.

Two enabling changes came out of trying to TEST this, and both are lessons in
their own right (CERTIFICATION.md §1.29):

* the RX applet's DSP toggles have no `objectName` and no `accessibleName`, so
  `invoke()` cannot address them — hence the `slice dsp` bridge verb;
* `nr2_toggle` cycles off → NR → NR2 → NR4 through the HOST chain and can fail
  to reach the slice at all, so the one shortcut that names NR is not a reliable
  way to drive it.

### D.2 By surface

| Surface | Control | State |
|---|---|---|
| **VFO / slice flag** | frequency | ✅ `setSliceFrequency` |
| | mode + IF filter | ✅ **verified** — `06 03 01` (CW, FIL1) |
| | S-meter flag | ✅ `SLC:LEVEL` |
| | RIT | ✅ **verified** — `21 01 01` then `21 00 00 00 00` |
| | XIT | ✅ implemented (`21 02`); shares ONE offset register with RIT |
| **S-meter applet** | level display | ✅ |
| **RX Controls** | AGC mode | ✅ `setSliceAgc` (FAST/MID/SLOW) |
| | AGC threshold | ❌ accepted and discarded — the radio has no threshold register |
| | preamp / RF gain | ✅ `setPanRfGain` → 3-position preamp |
| | filter width | ✅ snaps to FIL1/2/3, and the three are now published as `rxFilterWidthsHz` so the applet stops offering widths that all land on the same filter. Capability wiring is code-verified; the three buttons have NOT been confirmed on screen |
| | NR | ✅ **verified** — `16 40 01` + `14 06 01 53` (60 % = 153) |
| | NB | ✅ **verified** — `16 22 01` + `14 12 01 40` (55 % = 140) |
| | ANF | ✅ **verified** — `16 41 01` |
| | squelch | ✅ **verified** — `14 03 01 02` (40 % = 102). No enable exists: the threshold IS the control and off is zero |
| | manual notch | ❌ `16 48` mapped, no seam verb |
| | AF gain / mute / pan | ❌ seam verbs exist, backend does not implement |
| **TX Controls** | MOX / PTT | ✅ `setKeying` |
| | TUNE | ✅ `setTune` |
| | RF power | ✅ `setTxPower` |
| | power / SWR gauges | ✅ (units fixed; unverified on hardware) |
| | TX filter | ❌ `setTxFilter` not implemented (`16 58` unmapped) |
| **Phone / CW** | PROC enable + NOR/DX/DX+ | ✅ `setSpeechProcessor` (`16 44` + `14 0E`) |
| | ALC / Compression gauges | ✅ (ALC scale fixed; unverified) |
| | Level gauge | ⛔ hidden — this radio publishes no mic meter |
| | mic source | ✅ collapsed to PC by capability |
| | mic gain | ✅ implemented (`14 0B`) — **not driven live**, no bridge verb reaches it |
| | monitor | ✅ implemented (`16 45`) — **not driven live**. Function only: the radio's monitor LEVEL is a separate register and no verb carries it, so writing one would overwrite what the operator set on the radio |
| | VOX | ❌ no seam verb (`16 46` + `14 16` mapped) |
| | CW speed / pitch / break-in | ❌ no seam verb (`14 0C`, `14 09` mapped; `16 47` unmapped) |
| **Status bar** | voltage | ✅ `RAD:+13.8A` |
| | temperature | ⛔ no temperature meter exists in CI-V |
| | radio name / model | ✅ from the handshake + `19 00` |
| | hostname / alias | ⚠️ shows the connect address; the radio's own name is in the capabilities packet and unused |

### D.3 One control, two registers

NR and NB are single switches here and two registers on the radio, so the intent
carries enable and level together. Driving "NR on at level 60" first produced
`16 40 00` immediately before `16 40 01` — a brief disable of the operator's
noise reduction, from two individually-correct commands in the wrong order.

The backend now sends a function command only when the state actually changes,
and forgets what it believes on disconnect: the radio keeps its own DSP state
across our sessions and we never read it back, so carrying the previous
session's belief would suppress the first command that matters. Recorded as
CERTIFICATION.md §1.31 because it generalises to any fanned-out control.

### D.4 What is left

**Not implemented**

1. **Audio gain / mute / pan** — seam verbs exist, no override here. The radio's
   AF level IS now read at connect, so the control opens in the right place and
   then cannot move it, which is arguably worse than not reading it.
2. **Manual notch, VOX, CW speed / pitch / break-in** — CI-V mapped or trivially
   mappable; no seam verb yet.
3. **TX filter** (`16 58` SSB TX bandwidth) — `setTxFilter` exists, unimplemented.
4. **AGC threshold** is accepted and discarded; the radio has no threshold
   register. Better to advertise it as unavailable than keep a live slider that
   does nothing.
5. **The radio's own name** arrives in the capabilities packet and is unused;
   the status bar shows the connect address instead.

**Implemented and NOT proven on hardware** — the distinction this appendix
exists to keep visible:

6. **The TUNE carrier.** Synthesised, built, never keyed into a tuner.
7. **Mic gain and TX monitor.** No bridge verb reaches either.
8. **The three filter buttons**, on screen with the applet open.
9. **Connect-time state adoption**, beyond confirming the values arrive: whether
   each one lands on the control an operator is looking at is a separate
   question, and it is the §1.27 gap in a different costume.
10. **XIT.** RIT was driven and observed on the wire; XIT shares the offset
    register and was not.

**Open defects**

11. **Transmit cuts out roughly once a second on FT8** — see CERTIFICATION.md
    §2.6. The radio stays keyed and ALC stays active, so this is not a keying
    or an audio-delivery fault; what remains is real RF pulsing or a low-drive
    meter artefact, and those want a higher-power run to separate.
12. **A revoked session still looks healthy.** The backend swallows a post-grant
    auth failure as "the previous session's teardown" — right for a reconnect,
    wrong when the radio really has withdrawn this one. It should disconnect.

---

## Appendix E — CI-V capability sweep, observed on the wire

A black-box read-only sweep of an IC-705 (firmware E1.40), driven through
`civ send` / `civ trace`. Raw results: [`docs/data/icom-ic705-civ-sweep.json`];
the sweeper is `tools/icom_civ_sweep.py`.

**Provenance.** Every fact below is a response this radio gave to a command we
sent. Nothing here is derived from a firmware image, and it must stay that way —
Principle IV is explicit that decompiled protocol *knowledge* contaminates
everything written from it, and equally explicit that "capturing and studying
the protocol as it actually behaves on the wire" is clean. This appendix is the
second thing.

**Safety.** Read forms only, against a hard exclusion list. Unknown CI-V space
is not inert: it contains `18 00` (power off — a one-way trip over WiFi, since
the WLAN interface goes with it), `1C 00` (keying), `1C 01` (the tuner cycle),
plus scan and memory writes. A read is `cmd + sub` with NO payload; adding a
payload is what makes it a set.

### E.1 Method, and the two ways it lied first

Worth recording, because both failures produced confident wrong answers rather
than errors:

1. **Scanning the trace ring newest-first** matched the app's OWN metering — an
   S-meter poll every 100 ms, a transmit-state poll every 250 — so probes were
   answered by somebody else's reply. Commands we *know* work came back
   "silent" while the sweep looked plausible. §1.9's shape exactly: a
   measurement that looks in the wrong place reads as absence.
2. **Diffing the ring by index** then returned nothing at all, because the ring
   is capped at 200 frames: once full its length stops growing. Every probe
   still reported success and the whole sweep came back blank.

Correlating by `ageMs` works, because age survives eviction. And a single-pass
negative is NOT evidence of absence — re-probing the misses three times each
recovered seven commands that had simply answered slower than the window. Any
future sweep must keep that retry pass.

**A command the app itself polls cannot be cleanly attributed** by this method,
since a matching reply may be the app's rather than ours. For `15 02`, `15 15`
and `1C 00` we already have ground truth from the implementation, so the sweep
is a discovery tool for everything ELSE.

### E.2 What the radio answers that we do not map

**Levels (`14 xx`)** — the radio answers 14, we map 10:

| CI-V | Read back | What it is | Note |
|---|---|---|---|
| `14 07` | 128 | **TWIN PBT (PBT1)** position | 0 = full CCW, 128 = centre, 255 = full CW |
| `14 08` | 128 | **TWIN PBT (PBT2)** position | the pair shifts and narrows the IF passband |
| `14 16` | 128 | VOX gain | |
| `14 19` | 128 | LCD backlight | not ours to drive |

**PBT is the interesting one.** We report the IF filter as three fixed widths
because that is all `filterForWidthHz` can reach — but the radio has a
continuous passband-tuning pair underneath it. A client that drove `14 07` /
`14 08` could offer real passband control on a radio we currently describe as
having three filters. That is a feature, not a defect, and it is the single
largest capability this sweep found.

**Functions (`16 xx`)** — the radio answers 21, we map 10:

| CI-V | Read back | What it is |
|---|---|---|
| `16 42` | 0 | Repeater tone |
| `16 43` | 0 | Tone squelch |
| `16 47` | 0 | **BK-IN** — 00 off / 01 semi / 02 full |
| `16 4B` | 0 | DTCS |
| `16 4F` | 0 | Twin peak filter (RTTY) |
| `16 56` | **1** | DSP IF filter type — 00 SHARP / 01 SOFT |
| `16 57` | **1** | Manual notch width — W/M/N |
| `16 58` | 0 | SSB TX bandwidth — W/M/N |
| `16 5B` | 0 | DSQL / CSQL (DV) |
| `16 5C` | 0 | GPS TX mode |
| `16 5D` | 0 | Tone squelch type |

`16 47` is the one that matters for operators: CW break-in is unreachable today
and the radio plainly supports it.

### E.3 The questions this was run to answer

**Battery.** There is no battery command, and there does not need to be: on an
IC-705 `15 15` (Vd) IS the battery gauge. Read **7.98 V** on this radio — a
BP-272 pack at roughly 60–70 %, not a 13.8 V supply, which reads ~13.8 on the
same meter. One meter, two meanings, distinguished only by the value. We already
publish it to the status bar; what we do NOT do is say which of the two it is.

**Temperature.** Confirmed absent for the third time, now empirically as well as
from the guide. The `15 xx` space answers at `01, 02, 05, 07, 11, 12, 13, 14,
15, 16` and nothing else. There is no PA-temperature meter on this radio.

**WiFi signal strength and network health.** Nothing in CI-V. Network health is
an RS-BA1 property, not a CI-V one, and we already measure it ourselves —
per-stream RTT, jitter, gap and packet loss, surfaced through `liveness`. Signal
strength is a radio-display value with no command behind it.

**Counters.** None. CI-V has no packet, error or uptime counters of any kind;
every counter AetherSDR shows for this radio is one it computes from the
transport.

### E.4 Scope geometry, confirmed

`27 12` and `27 13` both answer `00` and nothing else — one receiver, one scope,
exactly as `IcomModels` already assumes for the IC-705. `27 10` and `27 11`
answer `01`, confirming both switches are on: the scope is running AND its data
is being sent to us, which is the pair whose asymmetry is the number-one cause
of a black panadapter.

### E.5 What to do with this

1. **`16 47` BK-IN** — real operator feature, radio supports it, no seam verb.
2. **`14 07` / `14 08` TWIN PBT** — would turn our three-filter story into real
   passband tuning. Needs a UI decision, not just a verb.
3. **`16 56` / `16 57` / `16 58`** — SHARP/SOFT, notch width, TX bandwidth. All
   trivial once the DSP verbs exist.
4. **Say which Vd means.** A voltage that is a battery gauge below ~9 V and a
   supply rail above it should be labelled as such, not left as a bare number.
5. **Re-run this sweep against the IC-7300MK2** when one is available. The
   sweeper is model-agnostic and the JSON diffs cleanly, which is the cheapest
   possible way to establish a second model's capability set.
