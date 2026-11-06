# Shared Icom MAIN/SUB Ethernet bring-up

The modern-scope IC-7851 profile uses the same receiver state, scope decoder,
spectrum/waterfall presentation, native tuning and RX audio routing as IC-7610
and IC-7760. Its model data supplies LAN menus 0056/0062–0066, codec negotiation,
the measured relative display slope of 0.72 dB/unit and REF range −20…+20 dB.
The absolute dBm floor remains estimated. No model-specific renderer or audio
processor is installed.

## IC-7851 lab learnings

Authority: the operator's Icom oracle §§13.15–13.16 and §14, the retained
IC-7850/7851 edition-3 manual and firmware 1.40 supplement, and Icomlab's
`docs/ic7851-support.md` and dated receive/tuning baselines. The lab evidence
qualifies the observed unit, not an inferred numeric firmware version.

| Concern | Shared AetherSDR component | Model data / evidence |
| --- | --- | --- |
| MAIN/SUB identity | `IcomCivBackend::ReceiverState`, addressed `25`/`26` | Stable receiver 00/01, DATA OFF/D1/D2/D3; focus is independent of TX routing |
| Scope and waterfall | `ScopeDecoder` → RadioModel → existing `SpectrumWidget` | 689 bins per receiver; common axes and ordered amplitudes; no IQ rotation |
| Native tuning | Existing spectrum click/drag/wheel → slice intent → backend | Addressed radio readback confirms the targeted receiver |
| Stereo receive | `IcomSession`, `RxAssembler`, persistent `Resampler` instances | RX codec 10 / TX 04; MAIN channel 0, SUB channel 1; arbitrary payload boundaries |
| Decoder/monitor | Existing per-slice taps and shared monitor mix | Receiver taps precede local gain/mute/balance |
| Freshness | Existing `civ profile` diagnostic | Per-receiver scope count/age, processed audio frames and pre-monitor RMS; audio frame counts include transport concealment |

The initial AetherSDR live baseline published both incoming IC-7851 scope
selectors through the single-pan fallback. The operator observed strobing.
A read-only Icomlab probe confirmed concurrent MAIN and SUB frames with distinct
windows (133 each, 689 bins), Dualwatch and dual scope ON, split/PTT OFF and AF
output. The shared dual-receiver profile routes each selector to its own pane.

The oracle's replay lessons distinguish received samples from concealment,
retain stereo frame alignment, and treat audio backlog separately from scope
history. Waterfall seams do not establish audio packet loss. A display FPS cap
does not establish fresh RF sweep rate. Keep radio/frame, renderer and audio
counters separate in reports. No transmit test is included in this RX run.

The receive transport requests missing outer sequences immediately, retains a
bounded RTT-aware replay budget, and ages each hole from its own arrival.
Stereo concealment uses the separate media sequence at offset 18, never the
number of lost outer packets: idles contain no PCM. Two consecutive observed
1364/1364/1112-byte cycles qualify the native 341/341/278-frame phases before
exact two-channel silence can fill an unrecovered media fragment. Sequence
wrap retains phase; a restart or changed geometry withdraws that inference.

The primary AudioEngine queue consumes a neutral `ReceiveAudioPlayout` record.
Stereo Icom transport requests 200 ms prefill and a 400 ms cap; an explicitly
larger user buffer is retained. This uses the existing shared PCM/DSP/mixer
path, independent of model identity. The prefill is consumed through delivery
gaps, refills both channels together after starvation, and composes with the
separate Receive Sync presentation delay. Audio already accepted by the output
device counts toward the cushion; an empty software queue alone cannot restart
prefill. Per-receiver decoder taps precede
the speaker queue. The monitor defaults to MAIN left / SUB right; centering
both receivers produces a bounded equal mix. `get audio` endpoint diagnostics
expose `jitter_prefill_ms`, `jitter_cap_ms` and `jitter_prebuffering`.

`civ session` exposes source-rate received and concealed frames separately under
`transport.media`. Outer gaps include idles and are not a source-loss rate.
Mono concealment is bounded to 64 estimated packets; stereo uses qualified
native media phases. Replay deadlines retain the first observation of each
hole even when a nearer packet arrives later.

Dualwatch is read using the common `07 C2` command. `civ profile` reports the
confirmed setting; `civ dualwatch on|off` is an explicit receive-only request
through the shared adapter and serialized scheduler. It never equalizes the
receivers or selects a band, and startup never rewrites this radio setting.

The implementation target is two fixed MAIN/SUB receivers with native display
traces, demodulated receive audio, and one shared transmitter over RS-BA1/CI-V.
It is experimental and requires target-radio qualification. Offline fixtures
prove parsing and routing, not firmware interoperability or RF performance.

## IC-7851 receive qualification — 2026-11-05

The real radio replied `19 00 8E`. The connected tab and status bar showed
IC-7851 with the experimental badge. This identity is shared with IC-7850; it
does not encode a firmware version. Both native scopes delivered 689 bins
through the common spectrum/waterfall path.

Twelve live gesture checks passed across MAIN and SUB: spectrum and waterfall
double-click and wheel tuning, spectrum passband drag, and Scroll-F paging
across each native window edge. Addressed frequency readbacks confirmed the
intended receiver while the other receiver stayed unchanged. The script
restored both test dial frequencies and observed PTT off.

A radio-read Dualwatch-off state explained the initially silent SUB feed. An
explicit `07 C2 01` request enabled it, confirmed by `07 C2 01` readback. Both
receivers then supplied audio. Live monitor captures confirmed MAIN-left and
SUB-right isolation; muting either channel left both TCI receiver feeds
running. The operator reported no popping or crackling with both receivers
open. A bounded sustained capture recorded no new playback-starvation events.
A normal disconnect/reconnect recovered both panes, native modes and audio
with unchanged dial frequencies and PTT off. The retained final trace contained
accepted LAN-output and modulation-menu replies, with no NG response in that
trace window. Received and concealed source frames remain separate diagnostics: that result
is not a claim of loss-free transport. No RF transmission was performed.

## Protocol authority

The implementation follows the operator's Icom oracle, sections 13.2–13.6 and
13.13, checked against these locally retained primary documents:

- [IC-7610 CI-V Reference Guide](https://www.icomjapan.com/support/manual/1745/),
  A7380-7EX-4, September 2025, pp. 13–15 and SET-menu tables.
- [IC-7760 CI-V Reference Guide](https://www.icomjapan.com/support/manual/4227/),
  A7788-8EX-2, May 2025, pp. 23–26 and SET-menu tables.
- The oracle's pinned wfview protocol research supplies the stereo PCM codec
  identifier. No source code is copied from wfview.

The CI-V identity reply selects the hardware profile, independently of a
customized command address. Names supplied by an operator do not select a
hardware profile. Profiles record official-guide evidence separately from live
hardware evidence. `verified` in the identity table means the documented facts
were checked, not that this app was tested on that hardware.

## Implemented boundary

| Area | Initial contract |
| --- | --- |
| Topology | Physical receiver 0 is MAIN; receiver 1 is SUB. Connect publishes MAIN only in Scroll-F. Native pan creation opens SUB after Dualwatch confirmation; closure retires its pane/audio and releases only app-owned Dualwatch. Pans are published before their slices. The IC-9700 retains its existing single selectable scope. |
| Frequency | `25 rr` reads/writes absolute MAIN/SUB. A selected-band transceive push prompts addressed reconciliation; it cannot overwrite an arbitrary receiver cache. |
| Mode | `26 rr` retains base mode, DATA OFF/D1/D2/D3 and FIL1/FIL2/FIL3. Writes carry all fields and use an addressed readback. The existing neutral mode vocabulary collapses the DATA variant in the displayed label; the backend retains the actual enum. |
| Other receiver controls | Only explicitly supported command families use `29 rr`. Shared PTT, split, modulation menus and scope commands are not wrapped. Generic ACK/NG is serialized by the existing scheduler. MAIN and SUB use separate semantic generations. |
| Scope | Two independent decoders, 689 amplitudes each, documented 0–200 range. Each normal Ethernet sweep has 704 data bytes after `27 00`, including 15 metadata bytes. Truncated/oversized traces and invalid selectors are rejected. Out-of-range clears only its receiver. |
| Frequency axes | Center mode is centre ± half-span. Fixed and scroll modes report actual lower/upper edges. Bytes already run from low to high RF; there is no IQ conjugation or FFT half-rotation. RadioModel derives spectrum and waterfall from the same bins and geometry. |
| Zoom | Center mode snaps to the eight native spans, 5 kHz–1 MHz total width. Fixed/Scroll-F drag and zoom use kHz-rounded edge windows. Each receiver borrows a slot not selected/reserved by the other scope, saves the range/slot preset before writing, and restores saved presets/selections during normal disconnect. |
| RX audio | Stereo PCM16 RX and mono PCM16 TX are negotiated independently at 48 kHz. Stereo packet fragments retain complete interleaved frames. The IC-7851 live capture confirms channel 0 MAIN/channel 1 SUB; the other dual profiles use the same documented mapping. Each receiver has its own persistent 48→24 kHz converter and decoder feed. Speakers get a bounded equal mix with local per-slice gain/mute/balance. |
| LAN AF/IF | Read-only inspection of the model's AF/IF menu. IF is not played as demodulated AF. No connect-time AF/IF menu rewrite. IC-7610 item 86; IC-7760 item 123; IC-7851 item 56. |
| TX routing | One global PTT and one mono uplink. Editing focus does not move TX. Radio split readback selects MAIN or SUB; `1C 03` reports actual TX frequency. Key-on waits for receiver frequency/mode and split readback. Existing operation admission, queued-audio cancellation, PTT readback and fail-safe unkey remain in place. |
| Modulation | IC-7610 LAN=05, DATA OFF item 91, D1/D2/D3 items 92/93/94. IC-7760 LAN=09, DATA OFF item 129, D1/D2/D3 items 130/131/132. Diagnostics follow the actual transmit receiver's DATA variant. IC-7851 LAN=09, DATA OFF item 63, D1/D2/D3 items 64/65/66. PC Audio continues to change DATA OFF only on operator request. |

Stereo negotiation uses the advertised factory CI-V destination as a transport
hint, not as authorization for model-specific controls. An explicit model choice
also requests stereo; `icom.rxChannels=1/2` can override the receive transport
for an automated connection using a customized address. An unknown identity
never gains TX or scope capability from that hint. Mono sessions publish no
invented SUB audio feed and warn that SUB audio is unavailable.

## Offline verification

`icom_dual_receiver_test` injects decoded frames into an unstarted production
session: no radio sockets, synthetic radio service, or physical transmitter.
It exercises all three profiles at a custom address, both scope IDs and full native
bin counts/positions, independent axes, out-of-range and malformed frames,
foreign/stale deliveries, reordered MAIN/SUB state, DATA enums, split/focus,
receiver-qualified confirmations, stereo decoder feeds and the speaker mix.
`icom_audio_test` checks byte/frame boundaries across stereo packets and channel
counts for loss concealment. Existing Icom tests protect the previous models.

## Minimum hardware acceptance

Before testing, check the radio owner/busy state and the local test lock. Do not
connect over another operator's session. Record model, firmware, connection
address, AF/IF menu, Dualwatch, scope display selection, codec and sample rate.

1. Connect in RX. Identify the correct model at default and customized CI-V
   addresses. Confirm command replies remain live during both scope streams.
2. Enable Dualwatch/dual scope on the radio if necessary. Observe distinct MAIN
   and SUB traces, 689 bins each, with separate frequency, span, REF and history.
3. Inject or tune identifiable signals at center and both sides. Confirm peak,
   waterfall column, click tuning and received tone agree on both receivers.
   Test Center, Fixed and both scroll metadata forms. Change spans independently.
4. Send different audio markers to each receiver. Establish actual LAN channel
   ordering, rate, gain/squelch behavior and effects of Dualwatch/XFC. Verify
   both decoder feeds and the monitor mix; mute one without interrupting its
   decoder feed. Confirm AF selection resolves any IF-like listening symptom.
5. With explicit TX authorization and an appropriate load, test low-power PTT
   and mono uplink in voice and data modes. Confirm RF/audio at the intended
   MAIN/SUB frequency for split off/on. Change editing focus without changing
   RF routing. Verify PTT readback and physical unkey on release/disconnect.
6. Reconnect and repeat after a transport interruption. Old scope/audio buffers,
   pending commands and TX authority must not cross the new session.

Archive the bridge snapshots, received CI-V trace, audio markers, screenshots
and the independent RF observation. Do not describe the devices as qualified or
close #6871/#6872 from offline tests alone.

## Deferred work and core transport risks

Receiver-specific DSP, true user-programmed filter widths/PBT, all antenna
combinations, meter calibration, PSK labels, CW text, memories, DPD/inhibit UI,
firmware-specific extensions and LAN IF demodulation are outside this initial
bring-up. This Ethernet path supplies amplitude traces and AF, not wideband IQ.
The radio's own TX inhibit/interlocks remain authoritative; the app does not
clear them.

The oracle also flags broader pre-existing RS-BA1 work: fixed-size single-record
capability parsing, advertised/configured media-port reconciliation, busy-owner
handling before stream acquisition, and model-specific open/restart behavior.
These are not proven causes of the current reports and should be investigated
with actual target captures before changing the shared transport.

Useful next automation verbs would report per-receiver scope freshness/bin
geometry, capture identified audio channels, and assert the global TX route
against actual TX-frequency readback. They should consume real observations,
not manufacture a radio session or elevate offline fixtures into hardware proof.

## Shared pane lifecycle and Scroll-F window navigation

The desktop exposes fixed MAIN/SUB hardware through native pan create/remove
intents. MAIN connects in Scroll-F. Opening SUB requests Dualwatch only if its
readback was off, initializes its first view to Scroll-F, and waits for radio
confirmation before publishing the pane. Opening/reopening never resets MAIN's
mode or tunes either receiver. Hidden SUB frames update diagnostics without
recreating a closed model or contributing local/decoder audio. Stereo transport
is negotiated once; each persistent converter continues consuming its channel.
MAIN alone defaults to centered monitor audio; two panes default to MAIN left
and SUB right. Explicit Dualwatch requests supersede pane ownership.

Scope window edits reuse the common CI-V edge builder, range tables, kHz
clamping and VFO-holding rules. Modern dual profiles declare the twelve HF/6 m
edge ranges. Because presets are global but selection is receiver-addressed,
the adapter reserves distinct slots and requires a matching preset readback
before a write. Coalesced gestures are paced at 200 ms. Paired center/width
changes share one target; tuning-related window changes wait for addressed
frequency readback. Stale-window sweeps are held while a bounded request settles.
`civ profile` reports pane visibility, current/borrowed slots, saved preset
contents and Dualwatch ownership/pending pane transitions. Normal disconnect
serializes saved presets and original selections, awaiting exact readbacks
before closing sockets; unexpected
transport loss cannot establish restoration. A newer observed operator preset
edit withdraws the old restoration claim.

Scope reads preserve their full receiver or range/slot address as the scheduler
reply prefix. A reply for another scope/preset cannot retire the pending read
or authorize a backup. Disconnect retires PCM and cancels view gestures, sends
fail-safe unkey first, then drains serialized restore writes/readbacks through
a bounded owner-thread event loop (12 s, excluding GUI input). Ordinary polls
and new user writes are suppressed during this phase. Missing confirmation is
reported rather than counted as restoration. `scopepreset` and `scopeslot`
bridge actions support typed RX-only preset inspection/recovery and exact
post-reconnect verification; raw CI-V injection remains TX-gated.

Connected radio tabs refresh on `RadioModel::infoChanged`, so late CI-V identity
updates both the tab model and status bar. The active session's canonical model
supersedes a stale discovery/connection label while retaining operator names.
Native pane creation waits for confirmed publication instead of reporting a
capacity failure at dispatch. The VFO TX selector writes split off for MAIN or
on for SUB, retains editing focus, and publishes its designation from readback.
Pending selection prevents PTT/TUNE from using the previous route; `civ profile`
exposes the pending target and actual `1C 03` transmit-frequency readback.
The restore drain serializes stop acknowledgments before setting transactions;
response-free teardown writes are reserved for the terminal close burst, where
their untagged FB/FA replies cannot retire a subsequent setting read.

## Live pane/Scroll-F acceptance — 2026-11-05

Modulation source menus are global; DATA OFF, D1, D2 and D3 each have their
own source. These are decimal menu numbers encoded as BCD after `1A 05`:

| Model | LAN level | DATA OFF | D1 / D2 / D3 | LAN source value |
|---|---|---|---|---|
| IC-7610 | 0090 | 0091 | 0092 / 0093 / 0094 | `05` |
| IC-7760 | 0128 | 0129 | 0130 / 0131 / 0132 | `09` |
| IC-7851 | 0062 | 0063 | 0064 / 0065 / 0066 | `09` |

Authority: IC-7610 CI-V A7380-7EX-4 p.5–6, IC-7760 CI-V A7788-8EX-2 p.9,
and IC-7850/7851 instruction manual edition 3 p.18-8. Complete source enums
are model-owned. All four readbacks are retained independently and exposed in
`civ profile`; only the TX receiver's active variant supplies the advisory.
PC Audio checks DATA OFF only while DATA is off. Selecting another DATA variant
uses that variant's cached readback, not the previous one's source. Diagnostics
never rewrite these settings. The lab IC-7851 reported D1 as USB (`0064 = 08`),
consistent with the oracle's earlier D2/LAN test; the two are distinct routes.

The IC-7851 bridge run passed MAIN-only startup in Scroll-F, centered MAIN
monitor audio, confirmed Dualwatch enable on SUB creation, app-owned release
on close, no hidden-pane resurrection, and reopen without retuning. Opening
SUB preserved a manually selected MAIN Fixed mode; reopening SUB preserved its
selected Center mode. Both were returned to Scroll-F. The live tab mismatch
was reproduced (tab Unknown Icom / status IC-7851) and the repaired info-change
refresh reported IC-7851 in both. The experimental badge was visible.

Fourteen window checks passed across both receivers on the same HF band:
spectrum/waterfall background drag, zoom in/out, native 5 kHz and 1 MHz widths,
and drag clamping before the VFO leaves Scroll-F. Each preserved both dial
frequencies and the other scope's window. Twelve additional checks passed for
spectrum/waterfall double-click and wheel tuning, passband drag, and edge
following on both receivers; test dial frequencies were restored. Coordinates
were selected outside the VFO control overlays.

Normal disconnect confirmed both saved range/slot values, both original
selectors and app-owned Dualwatch-off before transport closure (1.86 s in the
retained run). Fresh-session preset reads independently matched those originals.
A final monitor PCM capture had positive RMS on both channels; the operator's
master mute was preserved. All live actions ran with TX automation disabled
and PTT observed off. No RF TX result is claimed.

The Add Panadapter dialog also passed repeated close/reopen cycles without a
capacity warning. Both VFO TX buttons selected the expected split state and
actual `1C 03` dial frequency, preserving receive tuning with PTT off. Live
source reads were DATA OFF=MIC, D1=USB, D2=LAN and D3=LAN.
