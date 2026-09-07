# RTL slice persistence foundation (RFC #5468, F3a)

This change separates reported USB identity from discovery's connection locator.
`RadioModel::settingsScope()` supplies the same identity for its current
OperatingState save and preconnect/retry restore: trimmed reported RTL serial,
or the RTL family-wide row when no serial was reported. Genuine numeric serials
remain identities. Duplicate real serials still share a row; an index locator
does not distinguish their persistent state. Other families retain their existing
scope. RTL disconnect/flush completes under the old scope before a session swap.
Backend metadata no longer fabricates a serial from an enumeration index.

`RtlSliceSettings` compiles into the engine as the sole owner of feature
`RtlSlices`, schema 1. Its codec uses Hz and stable keyed IDs, nested squelch,
normalized modes/AGC, and model percentages. Representation bounds are inherited
from `SharedCapturePolicy`; they are not an advertised receiver count or RF range.
Whole-document validation finishes before exposing any parsed state. Explicit
patches read the exact row, retain unrelated members and omitted slices, and
remove only explicitly named IDs. Consumers use exact → family → absent;
corrupt/unsupported exact rows fail closed instead of choosing different state.
An empty row is settled and suppresses fallback/import. Existing generic settings
readers retain their previous fallback behavior; the new exact-read status also
distinguishes missing, corrupt, unavailable and present rows and exposes a raw
schema version even when JSON is corrupt.

There is deliberately no runtime call to this new owner or migration yet.
OperatingState remains the sole active RTL tuning/passband/rate owner. F3a does
not claim completed persistence cutover, multi-RX, new DSP controls or improved
hardware acceptance. Current pending/optimistic tuning behavior is unchanged.

## Subsequent cutover ordering

The accepted-capture adapter must first supply achieved center/rate, measured
usable interval, mode/BFO/guard metadata, capacity and accepted receiver state.
F4/M1 provides this; F3a does not invent those values. Then:

1. Obtain the canonical model scope and quiesce the legacy overlapping writer.
2. Call `migrateLegacy(identity)` once per attempted scope access. Only an absent
   effective RtlSlices document permits an exact matching legacy claim. An
   existing valid/empty exact or family document is settled. Newer/malformed or
   unavailable documents refuse and remain retryable. No legacy index-shaped
   `rtl:<n>` row is imported, even if a real reported serial now has that spelling.
   Numeric serials such as `0` remain eligible. Legacy reads remain inside
   `RadioStateMemory`, which returns the exact typed snapshot.
3. Migration writes the complete new document transactionally, never deletes
   the old row and never writes a separate completion marker. Failure must not
   enable a competing new writer or allow an RF-gain save to erase the source.
4. Complete the current-receiver replacement restore/save adapter before removing
   the legacy Tuning/Passband/SpanRate domains. Retain RF gain and memory-bank
   ownership. Later RF-gain saves use the explicit
   `RadioStateMemory::storeRtlRfGainPreservingLegacy()` helper to retain frozen
   old tuning fields for retry/downgrade. Ordinary non-RTL store semantics stay
   unchanged. This helper changes only the known gain value, preserves unknown
   nested gain/extension members, and refuses malformed or future extension
   versions. There must never be two active writers/restore paths for tuning.
5. Supply one prepared descriptor matching each saved ID/frequency/passband to
   `planRestore()`. The production F2 fixed-capture policy validates complete
   intervals and chooses capacity in ascending stable-ID order. Adapter metadata
   cannot alter saved tuning. The plan never moves hardware or changes storage;
   an empty accepted set means retain valid initial runtime state. Persist only
   successfully accepted state; partial restoration never implicitly deletes
   non-restored entries. Explicit user removals remove their stable IDs.

Stored capture center/rate provide context; they do not authorize a post-connect
recenter to accommodate otherwise rejected slices. AGC/squelch schema support is
not evidence that the experimental demodulator applies those controls. Live
stereo detection is not part of this document.

The headless catalogue remains observe-only and continues publishing its existing
serial locator field. This PR does not expand that public wire schema. A future
headless connection path must retain the same provenance before persistence.

`rtl_slice_settings_test` uses an isolated SQLite profile, direct owner calls,
the production F2 helper and a transport-free injected backend for real model
preconnect/retry/swap/flush paths. It opens no radio, listener or synthetic peer.
Physical USB reordering, GUI and RF behavior remain separate validation work.
