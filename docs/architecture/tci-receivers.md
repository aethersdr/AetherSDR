# TCI Receiver Index Policy

_Changed in [e49875b2](https://github.com/aethersdr/AetherSDR/commit/e49875b2) (#2140)._

## Overview

AetherSDR exposes Flex 6000-series slices to TCI clients (WSJT-X, JTDX,
etc.) as numbered **receivers** (`trx` indexes in the TCI protocol).
Since #2140 these indexes follow a contiguous numbering scheme rather
than passing through raw Flex slice IDs.

The receiver-index policy is only one half of the routing contract. Channel
0/1, split, TX ownership, and acknowledgement ordering are defined in
[TCI Routing and Ordering Contract](tci-routing-ordering.md).

## Rules

1. **Contiguous `0..N-1` indexing.**
   Receiver indexes are the position of each slice in the owned-slice
   list, starting at zero.  If you own slices with Flex IDs 1 and 3, TCI
   advertises `trx_count:2` and maps them to receivers 0 and 1.

2. **Indexes can shift at runtime.**
   If a lower-numbered owned slice is removed (e.g. another client
   deletes it), the remaining slices are re-indexed.  TCI clients receive
   updated notifications but should be prepared for index changes between
   sessions.

3. **Legacy-client fallback.**
   `TciProtocol::sliceForTrx()` includes a compatibility path: if the
   requested TRX index is out of the `0..N-1` range, it searches for a
   slice whose raw Flex `sliceId()` matches.  If that also fails it falls
   back to the first owned slice.  This keeps older clients that cached
   raw Flex IDs functional in the common single-slice case.

4. **Two channels per receiver.**
   AetherSDR advertises `channels_count:2`. Channel 0 is the receiver's RX
   slice. Channel 1 is the resolved radio-global TX slice for that RX route.
   The route uses stable Flex slice IDs internally even if public TRX indexes
   shift after topology changes.

## Spot Click Notifications

When a visible spot is clicked, AetherSDR broadcasts the click to every
connected TCI client using both protocol spellings:

- `clicked_on_spot:<callsign>,<frequency_hz>;`
- `rx_clicked_on_spot:<receiver>,0,<callsign>,<frequency_hz>;`

The receiver is the same contiguous `trx` index used by `vfo:` and
`modulation:` events.  The channel field is `0`, matching AetherSDR's
single-VFO path for a slice.  This mirrors Thetis behavior and keeps older
clients such as Log4OM working while giving TCI v2 clients receiver context.

Both spellings are emitted **unconditionally** for every spot click — there
is no client-capability handshake.  This is the v2 protocol baseline
introduced by #3145; third-party log clients writing TCI protocol parsers
should expect to see both messages back-to-back for every click, not just
the legacy `clicked_on_spot:` form.

## Why this changed

Flex slice IDs are radio-global and not necessarily contiguous within a
single client's owned set.  TCI's `trx_count` / receiver model assumes
`0..N-1` numbering.  Passing raw IDs caused WSJT-X to address
non-existent receivers when another client owned slice 0, breaking
multi-slice TCI operation (TCI1/TCI2).
