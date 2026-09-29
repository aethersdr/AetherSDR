# RTL analog multi-receiver evaluation

This checkpoint implements the analog multi-receiver path from RFC #5468.
It does not raise the qualified default capacity, enable multiple HD decoders,
or establish release qualification. The free-pan and HD RFC amendments remain
subject to maintainer review. Native qualification results belong to the exact
tested revision; component or injected-USB tests do not establish live RF proof.

## Evaluation admission

On Linux x86_64 only, start an explicitly isolated evaluation process with
`AETHER_AUTOMATION=1` and `AETHER_RTL_EVALUATION_RECEIVERS=2`, `4` or `8`.
This chooses the same capacity for the transaction and private worker at
construction. It cannot resize a running session, is never persisted, and
does not change `kQualifiedReceiverCapacity` (one). Every other value or
platform retains one. The automation bridge retains its independent receive
authorization and TX exclusion. Use a separate `AETHER_SETTINGS_DIR` and
preserve the normal launcher/profile before hardware testing.

FM, FM-N and native analog WFM can coexist. Each receiver owns its exact RF
filter, Mono/Stereo choice, deemphasis and monitor gain/pan/mute. AM/SAM/SSB/CW
and HD remain singleton configurations, including when a configured sibling
is parked. A refusal reports the unsupported combination without adopting a
partial recipe. Changing back from a remembered HD recipe to analog remains
an explicit operator action.

## Selection and presentation

RTL owns the selected stable ID separately from receiving membership. Focus
does not prepare DSP, retune capture or change any audio setting. An accepted
capture update retains a surviving selected receiver, including when parked.
Removal chooses the first surviving configured receiver; slot reuse does not
inherit the retired object's selection. Confirmed-control models do not emit
optimistic focus before the owner accepts their live-object identity.

One WFM applet shows the selected receiver's letter and accepted frequency.
Queued status callbacks and controls retain their originating model binding;
an old popup or held button cannot command a new selection or reused slot.
Parking clears reception observations while retaining configured controls.
Passive broadcast overlays retain each originating receiver's station/service
identity independently of focus and display visibility.

## Measurements and interpretation

The existing health snapshot exposes the qualified default and evaluation
capacities separately. Bounded lifetime counters add USB read starts/cancels,
valid/malformed callback counts, IQ sample counts, callback duration histogram
and exact maximum, queue occupancy/high-water, and per-slot processing-fault
withdrawals. Duration covers callback adoption/DDC/FFT/DSP work, excluding its
own counter update and USB transit time. A callback exceeding its actual
sample duration increments a separate deadline counter. Histogram percentile
bounds are conservative; the final bucket is unbounded.

`rtlReceiverRecipes` reports each last-published accepted receiver recipe,
including its `stableId`, current `selected` identity, `receiving`/`parked`
membership, `carrierHz`, `mode`, `filterLowHz`/`filterHighHz`, `wfmForceMono`,
`wfmHdStereo`, `wfmDeemphasisUs`, and `audioGain`/`audioPan`/`audioMute`.
An observer can freeze every receiver's accepted settings without moving
selection to inspect an applet. These are accepted control recipes, not an
independent measurement of the demodulated signal or observed stereo lock.

`rtlPcmStreams` contains eight stable receiver slots followed by the speaker
mix. Frames, nonzero samples, nonfinite input samples, delivered discontinuities,
left/right energy, peak, queue residence and delivery gaps are cumulative
owner-thread observations. Queue residence uses a separate enqueue clock and
does not replace the HD decoder's original production-age fence. Counts and
energies can be differenced across a frozen workload. Lifetime maxima and
delivery gaps can include intentional parking/reconfiguration; they must not
be called steady-state dropouts without checking that interval. Finite,
nonzero PCM establishes processing, not intelligibility or acoustic fidelity.

Counters are sampled independently, not one atomic instant. An empty
application audio queue is not a hardware XRUN counter. The RTL driver supplies
no sequence counter proving that every RF sample reached the host. Distinct
FM/WFM paths align their sample labels for mixing but do not claim phase-coherent
inter-station acoustic group delay.

## Qualification boundary

Use socket-free injected device/model tests plus real generated-IQ demodulation
for selection, stable-ID reuse, restore, stereo/deemphasis/filter independence,
monitor isolation, parking, shared capture refusal/rollback and stale delivery.
Retain the existing mixer deadlines and bounded queues. Before claiming live
capacity, freeze representative one-, two- and four-receiver workloads and
measure actual USB, FFT/waterfall and nonzero audio for at least 30 minutes
each, including CPU/thermal headroom, callback latency, queue growth, saturated
history RSS, planning and teardown. Report failures without changing their
thresholds after observing the result. Linux x86_64 evidence does not qualify
Linux aarch64, macOS, Windows, mixed legacy/native rates or multiple HD workers.

## Startup delivery headroom

The packet ring is allocated once from the immutable process receiver capacity.
Its usable slots are 127, 170, 254 and 424 for one, two, four and eight receivers;
two, four and eight are exposed by the process evaluation setting. Native WFM
produces one 256-frame tap per receiver plus a shared 128-frame speaker stream
at 48 kHz. Scaling by that packet rate preserves at least the original one-RX
nominal 225.8 ms buffering time. The qualified one-RX queue stays at 128 allocated
slots, including its empty sentinel. No allocation occurs during acquisition.

A four-WFM startup trace showed 151 ms spent synchronously publishing the bank,
with all 53 observed packet drops inside that span. The former shared 127-slot
usable ring covered only 112.9 ms at four WFM receivers. The scaled ring retains
publication order and the existing 128-packet per-service drain bound; it does
not shorten UI work or tolerate sustained overload. A process admitted for four
receivers retains its larger ring even with fewer receivers currently active,
so it can hold more queued latency in that state. `rtlPacketQueueCapacity`
reports actual usable slots. Occupancy is sampled current backlog; drops and
high-water remain lifetime observations.
Old 127-packet high-water screening results remain historical comparisons and
must not be described as the current ring's full condition.

## Eight-receiver requirement and SQL correction

The operator requested eight on Nobara after observing approximately 3% laptop
CPU with four. That observation is not an eight-receiver capacity result.
This is an isolated evaluation amendment for maintainer review under RFC #5468,
not approval to raise the qualified one-receiver default or other radio limits.
The existing eight slots, PCM routes, mixer membership, queue sizing and stable
IDs are reused; no thread or dependency is added. Evaluate the representative
operator workload and report actual continuity faults and timing/thermal data
separately; a historical zero-deadline-miss screen is not an evaluation admission
rule. Public release qualification remains separate.

RTL FM/FM-N manual SQL retains its absolute -120 + 1.2 * level scale, in dBFS
per 2048-point Blackman-Harris detector bin. For example, levels 51 and 54 mean
-58.8 and -55.2 dBFS/detector-bin. It is neither calibrated antenna dBm nor a
noise-relative percentage. Tuner gain, bandwidth, in-band interference and
peak/hang behavior influence the threshold required to close audio. There is
no guessed RF offset or changed default/manual threshold in this correction.
The temporary SQL overlay is suppressed when detector/display bandwidths differ;
the visible SQL controls describe the detector units instead of plotting a
misleading absolute threshold on a different scale.

The display changed to 65536 bins while SQL detection stayed at 2048. Both
normalize by FFT length; white-noise bin power therefore differs by about
15.05 dB, so display Auto thresholds are not detector thresholds. Additionally,
a trimmed logarithmic floor is biased low relative to noise peaks. RTL Auto now
runs inside each acquisition-owned gate on the same detector spectrum as its
peak measurement. It excludes the selected receiver passband plus window leakage,
uses the median of neighboring bin powers (with the exponential-noise median to
mean correction), and smooths locally. It retains the existing ramp, hang and
stale-data closure. It remains a coarse signal-level gate, not a discriminator
noise squelch or calibrated channel-power measurement.

The backend accepts a typed Auto intent and publishes its adopted per-slice
state. The engine owns Auto/margin alongside the manual threshold in RtlSlices;
manual/Off intent exits Auto. Selection, display zoom, FFT averaging and a hidden
applet do not control adaptation. WFM retains its separate decoder behavior;
the FM/FM-N manual/Auto gate is unavailable there. Schema-1 SQL objects now have
optional `automatic` (default false) and `marginDb` (default 10, range 5–20).
Legacy display-Auto intent rows are preserved but do not silently enable the
new engine policy; explicitly choose Auto after upgrading. The prior manual
threshold is never replaced with a computed Auto threshold by the new engine.

TCI already consumes the typed per-slice 48 kHz PCM route for RTL. This is a
source-level route audit, not eight-client throughput or RadioReference service
qualification. RTL does not advertise Flex DAX streams; its virtual DAX and
DAX-IQ paths are not supplied by this change. RadioReference/Broadcastify
publishing still requires a separately validated external streaming workflow.
