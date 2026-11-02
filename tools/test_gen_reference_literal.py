#!/usr/bin/env python3
"""
Self-test for literal() in tools/docs/gen_reference.py.

The Generated Reference pages read button and action names out of C++ string
tables, so a table entry wrapped in translation markup has to read as the
text it marks. A wrapper literal() does not know falls through as raw source,
and `gen_reference.py --check` then reports the macro call itself as the new
table cell; regenerating would put that call into the user manual.

    python3 tools/test_gen_reference_literal.py     # exits non-zero on any failure
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "docs"))
import gen_reference  # noqa: E402

# (expression, the value literal() must return)
CASES = [
    ('"Top Left"', "Top Left"),
    ('"Top " "Left"', "Top Left"),
    ('QStringLiteral("Top Left")', "Top Left"),
    ('tr("Top Left")', "Top Left"),
    ('QT_TR_NOOP("Top Left")', "Top Left"),
    ('QT_TRANSLATE_NOOP("AetherSDR::UlanziDialMapperDialog", "Top Left")',
     "Top Left"),
    ('QT_TRANSLATE_NOOP( "Ctx" ,\n    "Right Bot" )', "Right Bot"),
    ('QT_TRANSLATE_NOOP("Ctx, with comma", "Dial Press")', "Dial Press"),
    # Not string literals: literal() says so instead of guessing.
    ('kPillLabel', None),
    ('QT_TRANSLATE_NOOP("Ctx", kPillLabel)', None),
    ('QT_TRANSLATE_NOOP(ctx, "Top Left")', None),
]


def main() -> int:
    failures = 0
    for expr, want in CASES:
        got = gen_reference.literal(expr, "test")
        ok = got == want
        failures += not ok
        print(f"[{'PASS' if ok else 'FAIL'}] literal({expr!r}) -> {got!r}"
              + ("" if ok else f", expected {want!r}"))
    print(f"{len(CASES) - failures}/{len(CASES)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
