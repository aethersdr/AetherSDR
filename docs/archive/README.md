# docs/archive/

One-off planning docs, session notes, and historical fix reports that
were load-bearing during their originating PR but aren't actively
referenced by ongoing work. Kept here (rather than deleted) so they
remain greppable for context on why a piece of code was written the
way it was.

If you find yourself reaching for one of these to understand current
code, consider whether the surfaced context belongs in a more
discoverable place (the originating issue, a comment near the relevant
code, or `docs/architecture/`).

- [`hl2-phase0-spike.md`](hl2-phase0-spike.md) — the Python Phase‑0 spike that
  preceded the in-tree `Hl2Backend`, superseded by it. Still load-bearing for two
  reasons: it is the clean-room provenance record `THIRD_PARTY_LICENSES` cites
  for HPSDR Protocol 1, and it carries the corrected `CONFIG_MERCURY` diagnosis
  (the bit is a no-op on HL2; the real cure is C&C-before-`metis-start`
  ordering). The scripts it documents live in `tools/hl2/`.