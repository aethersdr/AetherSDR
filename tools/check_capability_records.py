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

The depth tracking earns its keep separately, on a nested type declared INSIDE
the struct: `enum class ClientSettingsDomain : quint32 {` at RadioCapabilities.h
:356. Its enumerators are not bools so nothing would be miscounted today, but
the guard is exercised rather than dormant.

(An earlier revision of this docstring claimed there were no nested types and
called the guard unexercised. That was wrong, and it is the third time in this
file's short history that a comment credited a mechanism other than the one
running — worth stating, because the parser's correctness is the only thing
standing behind the frozen number.)

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

REPO = Path(__file__).resolve().parent.parent
# Anchored on the script, not the cwd — every sibling checker in tools/ does
# this. The first version used a bare relative path; it failed CLOSED rather
# than passing vacuously, so it was never a hole, but "run it from anywhere"
# should be true of all of them (#5619 review, K5PTB).
HEADER = REPO / "src" / "core" / "backends" / "RadioCapabilities.h"

# The population at the freeze (#5262 M2, 2026-09-12). SHRINK ONLY.
#
# Lowering this is the migration working: a bool that became an
# std::optional<...Control> record, or one that turned out to have no consumer.
# When you convert one, drop this number in the same commit — that is the whole
# ratchet. Raising it needs a maintainer ruling on #5262, not a quiet edit.
FROZEN_BOOL_COUNT = 71

# The largest one-commit drop that is plausibly a real conversion rather than the
# parser falling over. See the vacuity check in main().
MAX_PLAUSIBLE_DROP = 15


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

    def code_of(raw: str) -> str:
        """Comment-stripped text. Braces in comments are not nesting."""
        return re.sub(r"/\*.*?\*/", "", re.sub(r"//.*$", "", raw))

    # Accumulate the struct's OWN body (depth 1) as text, then split it into
    # logical declarations on `;`. Matching per physical line missed a
    # clang-format-wrapped declaration — `bool\n    x = false;` — which needs no
    # intent to evade, and treated `bool a = false; bool b = false;` as one
    # field (#5619 re-review, ten9876). Braces are still counted per line,
    # which is what the depth tracking needs.
    body: list[str] = []
    depth = 0
    inside = False
    for line in lines[start:]:
        code = code_of(line)
        if not inside:
            if "{" in code:
                inside = True
                depth += code.count("{") - code.count("}")
            continue
        depth_before = depth
        depth += code.count("{") - code.count("}")
        # Depth 1 is the struct's own body. The CLOSING line of a multi-line
        # initializer (`agcModes = {\n  …\n};`) is at depth 2 on entry but
        # returns to 1, and it carries the terminating `;` — without it the
        # unterminated fragment merges with the next declaration and swallows
        # it. That cost hasModeIndependentSquelch exactly once, caught by
        # diffing the parser against an independent reference rather than by
        # the count looking wrong.
        if depth_before == 1 or depth == 1:
            body.append(code)
        if depth <= 0:
            break

    fields: list[str] = []
    for statement in " ".join(body).split(";"):
        # `(` still excludes member functions and operator==. It also excludes a
        # parenthesised initialiser (`bool x(false);`) — the most vexing parse,
        # genuinely undecidable here, and a stated limitation rather than a
        # heuristic that would misfire on real declarations.
        if "(" in statement:
            continue
        # Leading attributes and qualifiers: `[[deprecated]] bool x`,
        # `mutable bool x`. Stripped rather than enumerated, so a future
        # qualifier does not silently create another evasion.
        head = re.sub(r"^\s*(?:\[\[[^\]]*\]\]\s*|mutable\s+|static\s+|inline\s+)+",
                      "", statement)
        m = re.match(r"\s*bool\s+(?P<rest>.+)$", head, re.S)
        if not m:
            continue
        for declarator in m.group("rest").split(","):
            name = re.match(r"\s*([A-Za-z_]\w*)", declarator)
            if name:
                fields.append(name.group(1))
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

    # ANTI-VACUITY FLOOR, the sibling's ABOVE_SEAM_DIR_FLOOR applied here (#5619
    # re-review, K5PTB). The multi-line /* */ blind spot documented above is not
    # a small under-count when it fires: the brace tracking collapses, the scan
    # finds almost nothing, and the "below the frozen count" branch below then
    # prints "the migration is working" and tells the contributor to lower
    # FROZEN_BOOL_COUNT to the collapsed number — which would disarm the ratchet
    # permanently. Anyone following that message in good faith destroys the gate.
    #
    # A conversion retires bools a few at a time, so a large drop is a parse
    # failure rather than progress. The threshold is deliberately generous: it
    # only has to separate "someone converted a handful" from "the parser fell
    # over".
    if count < FROZEN_BOOL_COUNT - MAX_PLAUSIBLE_DROP:
        print(f"::error file={HEADER},title=capability-bool-vacuity::"
              f"only {count} boolean(s) found against a frozen {FROZEN_BOOL_COUNT} — "
              f"that is too large a drop to be a conversion and is almost certainly a "
              f"PARSE FAILURE (an unbalanced brace inside a block comment collapses "
              f"the depth tracking). DO NOT lower FROZEN_BOOL_COUNT to match: that "
              f"would disarm the ratchet permanently. Fix the parser, or raise "
              f"MAX_PLAUSIBLE_DROP if a conversion really did retire this many.")
        print(f"capability-records: {count} boolean(s) against a frozen "
              f"{FROZEN_BOOL_COUNT} — implausible drop, treating as a parse failure")
        return 1

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
