#!/usr/bin/env python3
"""
Self-test for tools/check_build_options.py.

The checker is only worth running if it fails on drift, so each case changes
the real BUILD-OPTIONS.md or CMakeLists.txt text in exactly one way and
requires a finding that names the change. The unmodified tree must pass, and
the parser cases pin the CMake shapes the real tree uses.

Usage:
    python tools/test_check_build_options.py
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_build_options as cbo  # noqa: E402

REPO = cbo.REPO
failures = 0


def check(ok: bool, message: str) -> None:
    global failures
    if not ok:
        failures += 1
        print(f"FAIL: {message}")


def real_texts() -> dict[str, str]:
    return {str(p.relative_to(REPO)): p.read_text(encoding="utf-8", errors="replace")
            for p in cbo.cmake_files(REPO)}


def findings_for(texts: dict[str, str], doc: str) -> list[str]:
    options, settings = cbo.collect_definitions(texts)
    return cbo.compare(options, settings, cbo.collect_documented(doc))


def main() -> int:
    texts = real_texts()
    doc = (REPO / cbo.DOC).read_text(encoding="utf-8")
    check(findings_for(texts, doc) == [], "the unmodified tree has no findings")

    options, settings = cbo.collect_definitions(texts)
    check(len(options) >= 30 and len(settings) >= 4,
          "the real tree yields options and cache settings (the parser is not vacuous)")
    check("third_party" not in "".join(texts), "vendored CMake files are excluded")

    # A row removed from the doc is reported as missing.
    row = next(line for line in doc.splitlines() if line.startswith("| `ENABLE_RTL` |"))
    found = findings_for(texts, doc.replace(row + "\n", ""))
    check(any(f.startswith("ENABLE_RTL:") and "missing" in f for f in found),
          "a deleted row is reported as missing from the doc")

    # A row whose option no longer exists is reported as stale.
    found = findings_for(texts, doc.replace(row, row + "\n| `ENABLE_GONE` | OFF | Removed. |"))
    check(any(f.startswith("ENABLE_GONE:") and "not defined" in f for f in found),
          "a row with no definition is reported as stale")

    # A changed literal default is reported.
    found = findings_for(texts, doc.replace("| `ENABLE_RTL` | ON |", "| `ENABLE_RTL` | OFF |"))
    check(any(f.startswith("ENABLE_RTL:") and "default is ON" in f for f in found),
          "a default mismatch is reported")

    # A new option in CMake with no row is reported.
    added = dict(texts)
    added["CMakeLists.txt"] += '\noption(AETHER_NEW_SWITCH\n    "multi-line declaration" ON)\n'
    found = findings_for(added, doc)
    check(any(f.startswith("AETHER_NEW_SWITCH:") for f in found),
          "a new multi-line option() with no row is reported")

    # Parser shapes.
    opts, sets = cbo.collect_definitions({"x": "\n".join([
        '# option(COMMENTED_OUT "no" ON)',
        'option(NO_DEFAULT "omitted default is OFF")',
        'option(WITH_PAREN "text (with parens)" ON)',
        'set(FORCED OFF CACHE BOOL "" FORCE)',
        'set(INTERNAL_ONE x CACHE INTERNAL "")',
        'set(A_PATH "${CMAKE_BINARY_DIR}/x"',
        '    CACHE PATH "multi-line cache set")',
    ])})
    check("COMMENTED_OUT" not in opts, "a commented-out option() is ignored")
    opts2, sets2 = cbo.collect_definitions({"x": "\n".join([
        '#[[ a bracket comment',
        'option(IN_BRACKET_COMMENT "no" ON)',
        ']]',
        'set(MULTI "a value spanning',
        'lines, # on the second" CACHE STRING "doc")',
        'cmake_dependent_option(DEPENDENT "dependent" ON "ENABLE_X" OFF)',
        'option(AFTER_ALL "still seen" ON)',
    ])})
    check("IN_BRACKET_COMMENT" not in opts2, "an option() inside a multi-line #[[ ]] comment is ignored")
    check(opts2.get("DEPENDENT") == "ON", "cmake_dependent_option() is collected with its default")
    check("MULTI" in sets2 and opts2.get("AFTER_ALL") == "ON",
          "a # on a later line of a quoted string is not a comment")
    check(opts.get("NO_DEFAULT") == "OFF", "an option() without a default reads OFF")
    check(opts.get("WITH_PAREN") == "ON", "parentheses inside the help string are balanced")
    check(sets == {"A_PATH"}, "FORCE and INTERNAL cache sets are ignored; multi-line CACHE PATH is kept")

    # Only Option/Setting tables count; the generated-definitions table does not.
    rows = cbo.collect_documented("\n".join([
        "| Definition | Defined when |", "|---|---|", "| `HAVE_X` | always |", "",
        "| Option | Default | Purpose |", "|---|---|---|", "| `ENABLE_X` | ON | x |",
    ]))
    check(rows == {"ENABLE_X": "ON"}, "rows outside Option/Setting tables are ignored")

    print(f"check_build_options self-test: {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
