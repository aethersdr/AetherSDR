# RTL analog multi-receiver evaluation

This checkpoint implements the analog multi-receiver path from RFC #5468.
It does not raise the qualified default capacity, enable multiple HD decoders,
or establish release qualification. The free-pan and HD RFC amendments remain
subject to maintainer review. Native qualification results belong to the exact
tested revision; component or injected-USB tests do not establish live RF proof.

## Evaluation admission

On Linux x86_64 only, start an explicitly isolated evaluation process with
`AETHER_AUTOMATION=1` and `AETHER_RTL_EVALUATION_RECEIVERS=2` or `4`.
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
