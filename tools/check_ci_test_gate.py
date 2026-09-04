#!/usr/bin/env python3
"""
AetherSDR per-PR test-gate freeze checker.

The per-PR workflow (.github/workflows/ci.yml) does not run the test suite. It
runs a hand-picked allow-list of tests, each behind its own `ctest -R` step on
the platform job whose toolchain the test's claim is about. That list is
FROZEN: no new test joins it. The full suite runs unfiltered in
.github/workflows/sanitizers.yml (`ctest --test-dir build`, no -R), weekly,
under ASan+UBSan and TSan — a test declared in tests/tests.cmake and built by
the default configure is on that lane the moment it is declared, and that is
where a new test belongs.

Why freeze it: every test added to ci.yml is one more step, one more comment
justifying it, one more wall-clock minute on every PR, and one more thing that
reads as "CI runs the tests" when it does not. The gate grew from zero to ~40
tests by accretion, one "rides along for the same reason" at a time; this
script is the ratchet that stops the accretion.

What it checks: every `ctest ... -R <pattern>` (or `--tests-regex`) in ci.yml
is applied as a regex search over the names registered by add_test() in
tests/tests.cmake, exactly as ctest applies it. Backslash-continued lines are
joined and YAML comment lines are ignored first, so a wrapped command counts
and a comment mentioning one does not. The resulting set of names must equal
the frozen list in .github/ci-test-gate.txt. Any name in ci.yml that the frozen
list does not carry fails the check with a message pointing at sanitizers.yml.

The ratchet is one-directional. `--update` only ever SHRINKS the frozen list:
it drops names ci.yml no longer selects, and refuses to run when ci.yml selects
a name the list does not carry. Growing the list is a hand edit to
.github/ci-test-gate.txt, which is maintainer-owned in CODEOWNERS so that the
diff is the review signal.

Patterns must be written inline. `-R "$SOME_VAR"` is rejected: an opaque
variable is exactly the thing a reviewer of the frozen list cannot read.

Deliberately narrow: it does not judge the tests already on the gate, and it
does not check sanitizers.yml — that lane is unfiltered by construction, and a
`-R` appearing there would be its own review conversation. It cannot see a
test run by any means other than `ctest -R` (a `-L`/`-E` selection, or a test
binary invoked directly); those are for review to catch.

Usage:
    python tools/check_ci_test_gate.py            # report, exit 0
    python tools/check_ci_test_gate.py --strict   # exit 1 on any drift
    python tools/check_ci_test_gate.py --update   # drop stale names from the list
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CI_WORKFLOW = Path(".github/workflows/ci.yml")
FROZEN = Path(".github/ci-test-gate.txt")
TESTS_CMAKE = Path("tests/tests.cmake")

# `ctest <anything on the logical line> -R <pattern>`. The pattern is
# double-quoted, single-quoted, or a bare word (`-R asr_gpu_probe_test -V`).
# `--tests-regex` is ctest's long form of the same flag.
CTEST_R = re.compile(
    r"""ctest\b.*?\s(?:-R|--tests-regex)\s+(?:"([^"]+)"|'([^']+)'|(\S+))""")
YAML_COMMENT_LINE = re.compile(r"^\s*#")
# `$VAR`, `${VAR}`, `$env:VAR` (pwsh), `${{ env.VAR }}` — a `$` regex anchor is
# none of these.
VARIABLE_REF = re.compile(r"\$(?:\{|env:|[A-Za-z_])")

# add_test(NAME foo ...) — the NAME may sit on the line after the paren.
ADD_TEST = re.compile(r"add_test\s*\(\s*NAME\s+(\S+)", re.S)
# Keep in sync with tools/check_test_registration.py: bracket comments are
# stripped FIRST (re.S — retired fixtures live inside `#[=[ ... ]=]` blocks in
# tests.cmake), then line comments.
BRACKET_COMMENT = re.compile(r"#\[(=*)\[.*?\]\1\]", re.S)
COMMENT = re.compile(r"#[^\n]*")

# Written only when the frozen list does not exist yet. An existing file keeps
# its own header verbatim across --update, so the file is the single owner of
# that prose.
FROZEN_HEADER = """\
# Tests the per-PR gate (.github/workflows/ci.yml) is allowed to run. FROZEN.
#
# One name per line. tools/check_ci_test_gate.py --update only removes names;
# adding one is a hand edit to this file, reviewed by its CODEOWNERS.
"""


VARIABLE_NAMES: list[str] = []


def registered_tests(text: str) -> set[str]:
    """Literal add_test() names. A name built from a variable (a foreach
    registering app_settings_safety_${SCENARIO}, say) cannot be resolved here
    and is left out; it is reported if a -R pattern then matches nothing."""
    code = COMMENT.sub("", BRACKET_COMMENT.sub("", text))
    names: set[str] = set()
    for raw in ADD_TEST.findall(code):
        if "$" in raw:
            VARIABLE_NAMES.append(raw)
        else:
            names.add(raw)
    return names


def logical_lines(text: str) -> list[tuple[str, int]]:
    """Physical lines joined across trailing backslashes, YAML comment lines
    dropped. Each entry carries the number of its FIRST physical line."""
    out: list[tuple[str, int]] = []
    buf: list[str] = []
    start = 0
    for i, phys in enumerate(text.split("\n"), 1):
        if not buf:
            if YAML_COMMENT_LINE.match(phys):
                continue
            start = i
        stripped = phys.rstrip()
        if stripped.endswith("\\"):
            buf.append(stripped[:-1])
            continue
        buf.append(phys)
        out.append((" ".join(buf), start))
        buf = []
    if buf:
        out.append((" ".join(buf), start))
    return out


def gate_patterns(ci_text: str) -> list[tuple[str, int]]:
    """Every -R pattern in ci.yml with the line number it starts on."""
    out: list[tuple[str, int]] = []
    for line, lineno in logical_lines(ci_text):
        for m in CTEST_R.finditer(line):
            raw = next(g for g in m.groups() if g is not None)
            if VARIABLE_REF.search(raw):
                print(f"error: {CI_WORKFLOW}:{lineno}: -R pattern {raw!r} "
                      "references a variable. Write the pattern inline so the "
                      "frozen list is reviewable without resolving env: blocks.")
                sys.exit(2)
            out.append((raw, lineno))
    return out


def resolve(patterns: list[tuple[str, int]], names: set[str]) -> dict[str, list[int]]:
    """Name -> ci.yml lines that select it, by regex search like ctest -R."""
    gated: dict[str, list[int]] = {}
    for pattern, line in patterns:
        try:
            rx = re.compile(pattern)
        except re.error as exc:
            print(f"error: {CI_WORKFLOW}:{line}: cannot parse -R pattern "
                  f"{pattern!r}: {exc}")
            sys.exit(2)
        hits = sorted(n for n in names if rx.search(n))
        if not hits:
            print(f"error: {CI_WORKFLOW}:{line}: -R pattern {pattern!r} "
                  f"matches no literal add_test() name in {TESTS_CMAKE}")
            if VARIABLE_NAMES:
                print("  (names built from variables are not resolved: "
                      + ", ".join(VARIABLE_NAMES) + ")")
            sys.exit(2)
        for n in hits:
            gated.setdefault(n, []).append(line)
    return gated


def read_frozen(path: Path) -> tuple[list[str], set[str]]:
    """(header comment lines, names)."""
    if not path.is_file():
        return FROZEN_HEADER.splitlines(), set()
    header: list[str] = []
    names: set[str] = set()
    for ln in path.read_text().splitlines():
        s = ln.strip()
        if not s:
            continue
        if s.startswith("#"):
            header.append(ln.rstrip())
        else:
            names.add(s)
    return header, names


def write_frozen(path: Path, header: list[str], names: set[str]) -> None:
    body = "".join(f"{n}\n" for n in sorted(names))
    path.write_text("\n".join(header) + "\n" + body)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Fail when ci.yml gates a test the frozen list does not carry")
    parser.add_argument("--strict", action="store_true",
                        help="exit 1 when the gate and the frozen list differ")
    parser.add_argument("--update", action="store_true",
                        help=f"drop names from {FROZEN} that ci.yml no longer "
                             "selects (never adds)")
    args = parser.parse_args()

    for rel in (CI_WORKFLOW, TESTS_CMAKE):
        if not (REPO / rel).is_file():
            print(f"error: {rel} is missing — has it moved? Update this script.")
            return 2

    names = registered_tests((REPO / TESTS_CMAKE).read_text())
    gated = resolve(gate_patterns((REPO / CI_WORKFLOW).read_text()), names)
    header, frozen = read_frozen(REPO / FROZEN)
    added = sorted(set(gated) - frozen)
    removed = sorted(frozen - set(gated))

    print(f"{CI_WORKFLOW}: {len(gated)} test(s) selected by -R; "
          f"{FROZEN}: {len(frozen)} frozen; "
          f"{len(names)} registered in {TESTS_CMAKE}")

    if added:
        print()
        print(f"NEW on the per-PR gate but not in {FROZEN}:")
        for n in added:
            where = ", ".join(f"{CI_WORKFLOW}:{ln}" for ln in gated[n])
            print(f"  {n}  ({where})")
        print()
        print("The per-PR test gate is frozen. A test declared in tests/tests.cmake")
        print("and built by the default configure already runs on the weekly")
        print("unfiltered lane (.github/workflows/sanitizers.yml); it does not")
        print("also get a `ctest -R` step in ci.yml. Drop the ci.yml step. If a")
        print("maintainer has decided this test is an exception, add its name to")
        print(f"{FROZEN} by hand in the same PR, with the reason in the PR body.")
        print("--update will not do that for you.")
    if removed:
        print()
        print(f"In {FROZEN} but no longer selected by ci.yml:")
        for n in removed:
            print(f"  {n}")
        if not args.update:
            print("Run `python tools/check_ci_test_gate.py --update` and commit the result.")

    if args.update:
        if added:
            return 1
        if removed:
            write_frozen(REPO / FROZEN, header, frozen - set(removed))
            print(f"wrote {FROZEN}: {len(frozen) - len(removed)} test(s) on the per-PR gate")
        else:
            print("ok: nothing to remove")
        return 0

    if (added or removed) and args.strict:
        return 1
    if not added and not removed:
        print("ok: per-PR gate matches the frozen list")
    return 0


if __name__ == "__main__":
    sys.exit(main())
