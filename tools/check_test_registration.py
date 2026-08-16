#!/usr/bin/env python3
"""
AetherSDR test-registration location checker.

Every test target is declared in tests/tests.cmake. The root CMakeLists.txt used
to hold all 300+ of them and was 6,357 lines because of it; the split cut it to
~2,780. The risk after a move like that is drift back: someone — a contributor
following a stale doc, or an agent pattern-matching on the neighbourhood where
the tests used to live — appends a new target to the root file, and it works.
Nothing breaks. It just quietly rebuilds the pile.

tests/tests.cmake already fails the CONFIGURE step if that happens, which is the
primary guard and gives the fastest feedback. This is the second line: it catches
the cases a configure-time check cannot, notably a reviewer reading a diff on
GitHub without building it, and a stale build tree that never re-runs cmake.

Deliberately narrow: it checks WHERE test targets are declared, nothing about
whether they are any good.

Usage:
    python tools/check_test_registration.py            # report, exit 0
    python tools/check_test_registration.py --strict   # exit 1 on a stray
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CANONICAL = Path("tests/tests.cmake")

# Strip # comments before scanning: the root file's tombstone comment names the
# very patterns below, on purpose, so that a grep for them lands on the sign that
# says where they went.
COMMENT = re.compile(r"#[^\n]*")

TEST_TARGET = re.compile(r"^\s*add_executable\s*\(\s*([A-Za-z0-9_-]+_test)\b", re.M)
TEST_REGISTER = re.compile(r"^\s*add_test\s*\(", re.M)


def listfiles() -> list[Path]:
    """Every CMake listfile that is ours — vendored trees bring their own."""
    found = [REPO / "CMakeLists.txt"]
    found += sorted(REPO.glob("*.cmake"))
    for sub in ("src", "tests", "plugins", "hal-plugin", "packaging", "tools"):
        base = REPO / sub
        if base.is_dir():
            found += sorted(base.rglob("CMakeLists.txt"))
            found += sorted(base.rglob("*.cmake"))
    return [p for p in found if "third_party" not in p.parts]


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Fail when a test target is declared outside tests/tests.cmake")
    parser.add_argument("--strict", action="store_true",
                        help="exit 1 when a stray declaration is found")
    args = parser.parse_args()

    canonical = REPO / CANONICAL
    if not canonical.is_file():
        print(f"error: {CANONICAL} is missing — has test registration moved again?")
        print("       If so, update CANONICAL in this script and tests/README.md.")
        return 1

    strays: list[tuple[Path, int, str]] = []
    for path in listfiles():
        rel = path.relative_to(REPO)
        if rel == CANONICAL:
            continue
        # run_*.cmake / verify_*.cmake under tests/ are ctest DRIVER scripts —
        # they are invoked BY a test, they do not declare one.
        #
        # Matched by name, not by directory. A directory-wide skip would exempt
        # everything under tests/, including a future tests/CMakeLists.txt —
        # which is precisely the file .github/CODEOWNERS warns about someone
        # adding, and which the configure-time guard in tests/tests.cmake does
        # not cover either, since that one scans only the root listfile.
        if rel.parent == Path("tests") and rel.name.startswith(("run_", "verify_")):
            continue
        code = COMMENT.sub("", path.read_text(encoding="utf-8", errors="replace"))
        for match in TEST_TARGET.finditer(code):
            line = code[:match.start()].count("\n") + 1
            strays.append((rel, line, f"add_executable({match.group(1)} ...)"))
        for match in TEST_REGISTER.finditer(code):
            line = code[:match.start()].count("\n") + 1
            strays.append((rel, line, "add_test(...)"))

    if not strays:
        print(f"test registration: OK — all test targets declared in {CANONICAL}")
        return 0

    print(f"test registration: {len(strays)} declaration(s) outside {CANONICAL}\n")
    for rel, line, what in sorted(strays):
        print(f"  {rel}:{line}: {what}")
    print(f"\nEvery test target belongs in {CANONICAL}. Move the block there")
    print("verbatim — that file is include()d from the root CMakeLists.txt, so its")
    print("relative paths still resolve against the repository root and nothing")
    print("needs rewriting. See tests/README.md.")
    return 1 if args.strict else 0


if __name__ == "__main__":
    sys.exit(main())
