# TCI Receiver Index Policy

_Changed in [e49875b2](https://github.com/ten9876/AetherSDR/commit/e49875b2) (#2140)._

## Overview

AetherSDR exposes Flex 6000-series slices to TCI clients (WSJT-X, JTDX,
etc.) as numbered **receivers** (`trx` indexes in the TCI protocol).
Since #2140 these indexes follow a contiguous numbering scheme rather
than passing through raw Flex slice IDs.

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

## Why this changed

Flex slice IDs are radio-global and not necessarily contiguous within a
single client's owned set.  TCI's `trx_count` / receiver model assumes
`0..N-1` numbering.  Passing raw IDs caused WSJT-X to address
non-existent receivers when another client owned slice 0, breaking
multi-slice TCI operation (TCI1/TCI2).
