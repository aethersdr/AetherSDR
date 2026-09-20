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

# ---- Statement boundaries that are not `;` (#5860) ----
# direct_bool_fields() splits on `;` and then requires each fragment to BEGIN
# with `bool`. So anything that ends a statement WITHOUT a semicolon gets glued
# onto the front of the next fragment, where `^\s*bool\s+` rejects it for the
# leading junk and the field is DELETED FROM THE COUNT. Exactly the #5727
# accessor failure, reached through `public:` and `#endif` instead — and just
# as silent, because nothing raises the depth and nothing throws.
#
# Both directions are live, and the growth one is the dangerous half: a bool
# added under an access label leaves the count EXACTLY at the freeze, so the
# ratchet prints "ok (shrink only)" while the population has actually grown.
# MAX_PLAUSIBLE_DROP cannot catch that, because nothing dropped.
#
# Handled by REMOVAL before the depth scan rather than by widening the field
# match, so a directive form nobody anticipated still cannot smuggle a field
# past — and so a stray brace inside `#define X {` can never collapse the scan.
#
# Comments are stripped over the WHOLE text: the previous per-physical-line
# `/\*.*?\*/` never matched a comment spanning lines, leaving its interior —
# a stray `{` included — visible to the depth tracking, and costing the next
# field even when the comment was perfectly balanced.
_COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# A whole logical directive line, continuations included. Indented and
# `#  ifdef`-spaced forms are legal C++ and appear in the wild.
_DIRECTIVE_RE = re.compile(r"^[ \t]*#(?:.*?\\\n)*.*$", re.M)


def _blanked(m: "re.Match[str]") -> str:
    """Replace a match with its own newlines, so line numbering survives."""
    return "\n" * m.group(0).count("\n")


def direct_bool_fields(text: str) -> list[str]:
    """Bool members declared directly in RadioCapabilities, nested structs excluded."""
    # Comments and preprocessor directives are not declarations. Blank them
    # before anything else looks at the text, so neither the depth scan nor the
    # `;` split can ever see one. See _COMMENT_RE / _DIRECTIVE_RE (#5860).
    text = _COMMENT_RE.sub(_blanked, text)
    text = _DIRECTIVE_RE.sub(_blanked, text)

    lines = text.splitlines()
    start = None
    for i, line in enumerate(lines):
        if re.match(r"\s*struct\s+RadioCapabilities\b", line):
            start = i
            break
    if start is None:
        raise SystemExit("check_capability_records: struct RadioCapabilities not found")

    # Scan the struct's OWN body (depth 1) CHARACTER by character, then split
    # it into logical declarations on `;`. Matching per physical line missed a
    # clang-format-wrapped declaration — `bool\n    x = false;` — which needs no
    # intent to evade, and treated `bool a = false; bool b = false;` as one
    # field (#5619 re-review, ten9876), so the split-on-`;` stayed.
    #
    # WHY CHARACTERS AND NOT LINES (#5727). The previous revision accumulated
    # whole LINES whenever the depth entered or returned to 1, which meant a
    # member function's body came along: its interior `;` ended the declaration
    # early, and its closing `}` was carried into the NEXT fragment, where the
    # `^\s*bool\s+` match below rejected it for the leading brace. The field
    # after an accessor was therefore DELETED FROM THE COUNT — silently, since
    # a one-line accessor never leaves depth 1 at all and the multi-line case
    # returns to it. Both failure directions are live: a bool declared after an
    # accessor vanishes and the ratchet reports a drop it then asks you to make
    # permanent, and a bool ADDED after one is never seen, so the population can
    # grow past the freeze with the count flat and MAX_PLAUSIBLE_DROP blind to
    # it because nothing dropped. The `std::optional<Record>` + accessor shape
    # this ratchet exists to ENCOURAGE is the shape that broke it.
    #
    # Two rules do it. Characters at depth >= 2 are dropped, so nothing inside a
    # member body, a brace initializer or a nested type can reach the field
    # match. And entering a nested block EMITS A SYNTHETIC `;`, which closes off
    # the declarator that opened it so the following field starts clean. The
    # synthetic terminator is what makes the multi-line initializer safe
    # structurally rather than by special case: `agcModes = {…};` becomes
    # `agcModes = ; ;`, both halves fail the bool match, and the field after it
    # survives — the hazard that cost hasModeIndependentSquelch exactly once.
    code = "\n".join(lines[start:])
    open_brace = code.find("{")
    if open_brace < 0:
        raise SystemExit("check_capability_records: RadioCapabilities body not found")

    chars: list[str] = []
    depth = 1
    for ch in code[open_brace + 1:]:
        if ch == "{":
            depth += 1
            if depth == 2:
                chars.append(";")
            continue
        if ch == "}":
            depth -= 1
            if depth <= 0:
                break
            continue
        if depth == 1:
            chars.append(ch)

    fields: list[str] = []
    for statement in "".join(chars).split(";"):
        # `(` still excludes member functions and operator==. It also excludes a
        # parenthesised initialiser (`bool x(false);`) — the most vexing parse,
        # genuinely undecidable here, and a stated limitation rather than a
        # heuristic that would misfire on real declarations.
        if "(" in statement:
            continue
        # Leading attributes and qualifiers: `[[deprecated]] bool x`,
        # `mutable bool x`. Stripped rather than enumerated, so a future
        # qualifier does not silently create another evasion.
        head = re.sub(
            r"^\s*(?:\[\[[^\]]*\]\]\s*"
            r"|(?:public|private|protected)\s*:\s*"
            r"|mutable\s+|static\s+|inline\s+)+",
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
    #
    # BUT ONLY THE LOUD VERSION OF "FELL OVER" (#5727). This floor sees a drop,
    # so it is blind to a parse failure that loses ONE field — and blind by
    # construction to one that loses nothing and stops SEEING new ones, where
    # the count does not move at all. Widening it would not help; it would only
    # fail real one-bool conversions. What guards the quiet direction is the
    # parser being right, which is what tools/test_check_capability_records.py
    # is for. Keep this sized for the brace collapse it was written for.
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
