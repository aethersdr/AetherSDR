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
fields at all. This parser counts DIRECT bool members of RadioCapabilities only
— 71 at the freeze.

A precision about HOW, because the obvious explanation is wrong (#5619 review):
those three are excluded by the START OFFSET, not by the depth tracking. They
are declared ABOVE `struct RadioCapabilities`, so the scan never reaches them.
The depth guard exists for a nested type declared INSIDE the struct, of which
there are none today — so it is currently unexercised on this header, and should
be read as a guard against a future nested struct rather than as logic this
count has validated.

WHAT A COUNT CANNOT SEE. Converting one bool to a record while adding another in
the same commit leaves the number flat and passes. That is inherent to counting
rather than a defect — a name set would catch the swap but fail on ordinary
renames and reordering, which is the churn this deliberately tolerates. The
review that catches the swap is a human one.

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
        # Braces are counted on COMMENT-STRIPPED text. A Doxygen member group
        # (`/** @{ */` … `/** @} */`) puts braces in a comment, which the first
        # version counted as real nesting: depth went to 2 and every bool inside
        # the group became invisible to the ratchet (#5619 review, Ozy).
        #
        # Per-line stripping is enough for this header, which uses `//` and
        # single-line `/* */` only. A MULTI-LINE /* */ containing an unbalanced
        # brace would still fool it — noted rather than solved, because solving
        # it properly means tracking comment state across lines and the header
        # has never used that form.
        code = re.sub(r"/\*.*?\*/", "", re.sub(r"//.*$", "", line))
        if not inside:
            if "{" in code:
                inside = True
                depth += code.count("{") - code.count("}")
            continue
        depth_before = depth
        depth += code.count("{") - code.count("}")
        # Depth 1 is the struct's own body; anything deeper is a nested type.
        #
        # The trailing comment is stripped FIRST. Guarding on a bare "(" in the
        # raw line looked right and silently dropped hasExtendedDsp, whose
        # comment reads "(NRS/RNN/NRF)" — an off-by-one in the frozen count that
        # would have banked a capability nobody could see.
        if depth_before == 1 and "(" not in code:
            # EVERY declarator on the line, and every initialiser form. The
            # first version matched only `bool x = false;` — which is what the
            # header happens to use today, so it passed — and a brace init
            # (`bool x{false};`), a second declarator (`bool a = false, b;`) and
            # a bitfield (`bool x : 1;`) all walked straight past it (#5619
            # review). Style-dependent evasions are exactly what a ratchet is
            # for: the one bool that slips in will not match house style.
            m = re.match(r"\s*bool\s+(?P<rest>[^;]*);", code)
            if m:
                for decl in m.group("rest").split(","):
                    name = re.match(r"\s*([A-Za-z_]\w*)", decl)
                    if name:
                        fields.append(name.group(1))
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
