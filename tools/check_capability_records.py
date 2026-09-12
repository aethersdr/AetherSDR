#!/usr/bin/env python3
"""RadioCapabilities boolean ratchet — #5262 M2.

WHY THIS EXISTS. M2's convention is that a new capability lands as a per-feature
record (`std::optional<FeatureRecord>` — engaged = present, fields = shape), the
`cwText*` pattern generalized, rather than as another loose boolean. Two failure
modes drove that:

  * BOOLEAN FISSION. A bool encodes a yes/no that turns out to have shape. When
    the second radio family arrives the bool splits — `hasRadioSideDsp` became
    four tiers, `hostModulates` became two fields — and every consumer of the
    old name has to be found and re-reasoned.
  * THE ALL-DEFAULTS-FALSE TRAP. A bool has a default, so a backend that simply
    forgets to set it reports a definite "no" indistinguishable from a
    considered one. An absent optional says "not declared".

The convention was written down and did not hold: #5299 alone added seven new
bools after M2 was recorded. So it is a ratchet now, not a convention.

WHY A COUNT AND NOT A NAME SET. The point is to stop the population growing, not
to freeze which capabilities exist — renaming or reordering a bool is fine, and
a name set would make ordinary churn fail the check for no benefit. A count also
states the goal plainly: this number goes down.

ALSO: THE COUNT IS NOT WHAT GREP SAYS. `grep -c '^\\s*bool '` over this header
reports 74, and that figure reached #5262 and its planning comments. Three of
those are `operator==` declarations inside the nested helper structs
(DeclaredBandRange, RxFilterPreset, RxFilterControl), which are not capability
fields at all. This parser counts DIRECT bool members of RadioCapabilities only,
by tracking brace depth — 71 at the freeze.

Usage:
    python tools/check_capability_records.py            # report
    python tools/check_capability_records.py --strict   # exit 1 on growth
"""

import argparse
import re
import sys
from pathlib import Path

HEADER = Path("src/core/backends/RadioCapabilities.h")

# The population at the freeze (#5262 M2, 2026-09-12). SHRINK ONLY.
#
# Lowering this is the migration working: a bool that became an
# std::optional<...Control> record, or one that turned out to have no consumer.
# When you convert one, drop this number in the same commit — that is the whole
# ratchet. Raising it needs a maintainer ruling on #5262, not a quiet edit.
FROZEN_BOOL_COUNT = 71


def direct_bool_fields(text: str) -> list[str]:
    """Bool members declared directly in RadioCapabilities, nested structs excluded."""
    lines = text.splitlines()
    start = None
    for i, line in enumerate(lines):
        if re.match(r"\s*struct\s+RadioCapabilities\b", line):
            start = i
            break
    if start is None:
        raise SystemExit("check_capability_records: struct RadioCapabilities not found")

    fields: list[str] = []
    depth = 0
    inside = False
    for line in lines[start:]:
        if not inside:
            if "{" in line:
                inside = True
                depth += line.count("{") - line.count("}")
            continue
        depth_before = depth
        depth += line.count("{") - line.count("}")
        # Depth 1 is the struct's own body; anything deeper is a nested type.
        #
        # The trailing comment is stripped FIRST. Guarding on a bare "(" in the
        # raw line looked right and silently dropped hasExtendedDsp, whose
        # comment reads "(NRS/RNN/NRF)" — an off-by-one in the frozen count that
        # would have banked a capability nobody could see.
        code = re.sub(r"//.*$", "", line)
        code = re.sub(r"/\*.*?\*/", "", code)
        if depth_before == 1 and "(" not in code:
            m = re.match(r"\s*bool\s+([A-Za-z_]\w*)\s*(=|;)", code)
            if m:
                fields.append(m.group(1))
        if depth <= 0:
            break
    return fields


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true",
                    help="exit 1 when the boolean population has grown")
    args = ap.parse_args()

    if not HEADER.exists():
        print(f"check_capability_records: {HEADER} not found", file=sys.stderr)
        return 1

    fields = direct_bool_fields(HEADER.read_text(encoding="utf-8"))
    count = len(fields)

    if count > FROZEN_BOOL_COUNT:
        added = count - FROZEN_BOOL_COUNT
        print(f"::error file={HEADER},title=capability-bool-ratchet::"
              f"RadioCapabilities gained {added} boolean(s) — {count} against a frozen "
              f"{FROZEN_BOOL_COUNT}. A new capability lands as a per-feature record "
              f"(std::optional<FeatureRecord>: engaged = present, fields = shape), not "
              f"another bool — see #5262 M2. Booleans fission when the second radio "
              f"family arrives, and an unset one reports a definite 'no'. If this is a "
              f"deliberate exception, raise FROZEN_BOOL_COUNT with a maintainer ruling "
              f"on #5262.")
        print(f"capability-records: {count} boolean(s), frozen at {FROZEN_BOOL_COUNT} "
              f"— GREW by {added}")
        return 1 if args.strict else 0

    if count < FROZEN_BOOL_COUNT:
        print(f"capability-records: {count} boolean(s), below the frozen "
              f"{FROZEN_BOOL_COUNT} — the migration is working. Lower "
              f"FROZEN_BOOL_COUNT in tools/check_capability_records.py to {count} "
              f"so the gain cannot be given back.")
        return 0

    print(f"capability-records: {count} boolean(s), at the frozen "
          f"{FROZEN_BOOL_COUNT} — ok (shrink only)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
