# Shared capture policy foundation

`src/core/SharedCapturePolicy.{h,cpp}` implements the pure RF admission and
restore policy from approved RTL RFC #5468. It is compiled into `aethercore`
and exercised directly by `shared_capture_policy_test`. No backend invokes it
yet. It does not change USB, DSP, audio, viewport, UI, settings or slice lifecycle.

## Descriptor contract

All frequency and rate fields use finite `double` Hz with magnitude at most
2^40 Hz. This numerical ceiling is not a tuner range or usable-bandwidth claim.
Occupied RF must be nonnegative and remain below that ceiling. Empty/inverted
filters, negative guards, zero achieved rate and unresolved numerical intervals
are refused. No mode defaults, filter presets or measured margins are inferred.

A slice's complete occupied interval is:

```
[carrier + translation + filterLow - guardLow,
 carrier + translation + filterHigh + guardHigh]
```

The adapter supplies the signed carrier/BFO translation so both filter edges
refer to RF in the same coordinate system. Filter edges may be asymmetric or
on one side of the carrier. `occupiedInterval` validates geometry and a
nonnegative stable ID; the set operations also validate the caller's slot bound.

A capture descriptor carries one endpoint ID, a target generation, its center,
achieved sample rate and independently supplied left/right usable extents.
Extents are nonnegative, not both zero, and each must fit within half the achieved
rate. One-sided usable intervals are supported. Usable bandwidth is separate
from nominal rate and viewport. A legal capture may include unused negative RF;
that does not disqualify positive occupied RF inside it.

`ReceiverLimits` separates addressable slots from concurrent capacity. The caller
must supply `slotCount` matching its published `maxSlices`/addressable range;
valid IDs are `[0, slotCount)`. `capacity` may be lower because fewer resources
are available, but cannot exceed `slotCount` or bypass a smaller architectural
limit. Slot count is positive; zero capacity is valid. The 4096-entry/slot and
256-domain ceilings only bound validation work and allocations; they do not
advertise supported receiver counts. Architecture limits require integrated
measurement in later RFC phases.

Hardware center domains are an explicit union of inclusive intervals with
independent grids. They may be disjoint, overlapping or supplied in any order.
The representable hardware request at grid index `n` is
`std::fma(n, stepHz, gridOriginHz)`. The adapter must describe its actual tuning
quantization this way; the helper does not infer it from a nominal sample rate.
Indices are exact integers bounded to +/-2^52. A domain must contain a realizable
point and its step must be at least the greatest double spacing at its upper
edge/origin. Unresolvable grids fail validation instead of hanging or rounding
onto unrelated centers.

Arithmetic uses IEEE round-to-nearest double operations. TwoSum residuals round
occupied edges outward and usable capture edges inward. There is no tolerance
that widens acceptance and no fixed fractional-Hz lattice. Conservative rounding
can refuse a mathematically fitting fringe that cannot be represented safely.
The helper must not be compiled with unsafe floating-point reassociation or
fast-math. Ordinary decimal rates, guards and measured margins are accepted.

## Selection and readback

`selectCenter` receives the complete desired receiver set. It validates every
entry and the caller's limits before returning a proposal. From occupied
intervals `[L_i,H_i]` and usable extents `U_L,U_R`, the continuous center bound is:

```
max(H_i - U_R) <= center <= min(L_i + U_L)
```

It intersects that bound with each legal hardware domain, examines neighboring
grid points around the current center and feasible endpoints, then checks full
containment at the actual returned double center. It preserves a legal current
center, otherwise chooses the nearest realizable center; exact ties choose lower
Hz regardless of domain order. Distance residuals distinguish close candidates
whose large distances would otherwise round to the same double.

All add/tune/filter/guard/mode-translation/rate proposals use this same operation
with their full desired set and proposed achieved capture descriptor. No input
is mutated; one invalid or impossible entry refuses the whole proposal. Removing
entries preserves the center when still legal.

A selection is a proposal, not confirmed hardware state. `validateReadback`
requires coherent proposed and actual endpoint IDs and target generations, checks
both descriptors and the entire desired set, and validates actual center/rate/
usable margins. Actual values may differ if the full set still fits. The caller
assigns the target generation for a reconfiguration and assembles one coherent
readback with that generation. Hardware transactions, rollback, asynchronous
ownership and publication remain the integration layer's responsibility.

Center domains and returned centers must use the same adapter-declared
representation. The adapter must document whether readback is logical library
state or a measured hardware quantity. In
[Osmocom librtlsdr v2.0.2](https://github.com/osmocom/rtl-sdr/blob/v2.0.2/src/librtlsdr.c#L884-L913)
and [RTL-SDR Blog commit aed0ea19](https://github.com/rtlsdrblog/rtl-sdr-blog/blob/aed0ea19f3a273370a13c9009b96313c75d54c7b/src/librtlsdr.c#L887-L936),
`rtlsdr_get_center_freq()` returns the cached requested frequency stored after
successful tuning; it does not measure the achieved PLL frequency. Verify other
library versions or forks before relying on that behavior. This policy's
validation does not establish physical RF accuracy, which needs hardware evidence.

## Fixed-capture restore

`restoreFixedCapture` validates the actual established capture and all input
entries before returning a complete plan. It never recenters or changes capture
bandwidth. Among valid individually fitting entries it accepts ascending stable
IDs up to capacity, preserving every field and ID. All occurrences of duplicated
nonnegative IDs are rejected, including otherwise-invalid occurrences, so input
order cannot choose a winner. Other per-entry reasons distinguish invalid IDs,
numeric/interval failures, outside-capture intervals and capacity refusal.
Rejections carry the original input index and ID in original input order.
A global descriptor/limits/collection failure returns no partial plan.

Saved slice descriptors contain only RF configuration and stable slot IDs, no
session capture identity or generation. Consequently reconnecting into a new
capture generation does not invalidate remembered configuration. Future lifecycle
code owns slot occupant generations; future persistence code owns parsing,
logging each refusal and atomically applying/persisting accepted state. If no
slice is accepted, the caller retains the session's valid initial state.

The focused test has no sockets, settings, hardware, Qt event loop or DSP. It
uses explicit RF edges, exhaustive small integer hardware-center enumeration,
nextafter boundary cases and malformed bounded input. Live RTL integration,
measured margins, architecture capacity, RF reception and GUI convergence remain
later gates.
