#!/usr/bin/env python3
"""
AetherSDR per-PR test-gate freeze checker.

The per-PR workflow (.github/workflows/ci.yml) does not run the test suite. It
runs a hand-picked allow-list of tests, each added by a PR that had a reason at
the time, each behind its own `ctest -R` step. That list is FROZEN: no new test
joins it. The full suite runs unfiltered in .github/workflows/sanitizers.yml
(`ctest --test-dir build`, no -R), weekly, under ASan+UBSan and TSan — a test
declared in tests/tests.cmake is on that lane the moment it is declared, and
that is where a new test belongs.

Why freeze it: every test added to ci.yml is one more step, one more comment
justifying it, one more wall-clock minute on every PR, and one more thing that
reads as "CI runs the tests" when it does not. The gate grew from zero to ~40
tests by accretion, one "rides along for the same reason" at a time; this
script is the ratchet that stops the accretion.

What it checks: every `ctest ... -R <pattern>` in ci.yml — inline patterns and
patterns passed through `env:` variables such as ICOM_GATE — is applied as a
regex search over the names registered by add_test() in tests/tests.cmake,
exactly as ctest applies it. The resulting set of names must equal the frozen
list in .github/ci-test-gate.txt. Any name in ci.yml that the frozen list does
not carry fails the check with a message pointing at sanitizers.yml.

A PR that removes a test from ci.yml is welcome; run `--update` and commit the
shrunken list. A PR that GROWS the frozen list is the thing this exists to make
visible: the diff to .github/ci-test-gate.txt is the review signal, and that
file is maintainer-owned in CODEOWNERS for that reason.

Deliberately narrow: it does not judge the tests already on the gate, and it
does not check sanitizers.yml — that lane is unfiltered by construction, and a
`-R` appearing there would be its own review conversation.

Usage:
    python tools/check_ci_test_gate.py            # report, exit 0
    python tools/check_ci_test_gate.py --strict   # exit 1 on any drift
    python tools/check_ci_test_gate.py --update   # rewrite the frozen list
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

# `ctest <anything on the line> -R <pattern>`. The pattern is double-quoted,
# single-quoted, or a bare word (`-R asr_gpu_probe_test -V`).
CTEST_R = re.compile(
    r"""ctest\b[^\n]*?\s-R\s+(?:"([^"]+)"|'([^']+)'|(\S+))""")
# `  ICOM_GATE: "^(...)$"` under a step's env: block.
ENV_ASSIGN = re.compile(r'^\s*([A-Z][A-Z0-9_]*)\s*:\s*"([^"]+)"\s*$', re.M)
ENV_REF = re.compile(r"^\$\{?([A-Z][A-Z0-9_]*)\}?$")

# add_test(NAME foo ...) — the NAME may sit on the line after the paren.
ADD_TEST = re.compile(r"add_test\s*\(\s*NAME\s+([A-Za-z0-9_.-]+)", re.S)
BRACKET_COMMENT = re.compile(r"#\[(=*)\[.*?\]\1\]", re.S)
COMMENT = re.compile(r"#[^\n]*")

FROZEN_HEADER = """\
# Tests the per-PR gate (.github/workflows/ci.yml) is allowed to run. FROZEN.
#
# This list does not grow. New tests run on the weekly unfiltered lane
# (.github/workflows/sanitizers.yml) the moment they are declared in
# tests/tests.cmake; they do not get a `ctest -R` step in ci.yml.
# tools/check_ci_test_gate.py fails Static Checks when ci.yml's -R patterns
# resolve to a name that is not here. Removing a test from ci.yml: run
#     python tools/check_ci_test_gate.py --update
# and commit the shorter list. A diff that ADDS a line to this file is the
# review signal the freeze exists to produce — it needs a maintainer's reason.
#
# One name per line; generated and sorted by --update.
"""


def registered_tests(text: str) -> set[str]:
    code = COMMENT.sub("", BRACKET_COMMENT.sub("", text))
    return set(ADD_TEST.findall(code))


def gate_patterns(ci_text: str) -> list[tuple[str, int]]:
    """Every -R pattern in ci.yml with its line number, env vars resolved."""
    env = dict(ENV_ASSIGN.findall(ci_text))
    out: list[tuple[str, int]] = []
    for m in CTEST_R.finditer(ci_text):
        raw = next(g for g in m.groups() if g is not None)
        line = ci_text.count("\n", 0, m.start()) + 1
        ref = ENV_REF.match(raw)
        if ref:
            if ref.group(1) not in env:
                print(f"error: {CI_WORKFLOW}:{line}: -R uses ${ref.group(1)} "
                      f"but no `{ref.group(1)}: \"...\"` env assignment was found")
                sys.exit(2)
            raw = env[ref.group(1)]
        out.append((raw, line))
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
                  f"matches no add_test() name in {TESTS_CMAKE}")
            sys.exit(2)
        for n in hits:
            gated.setdefault(n, []).append(line)
    return gated


def read_frozen(path: Path) -> set[str]:
    if not path.is_file():
        return set()
    return {ln.strip() for ln in path.read_text().splitlines()
            if ln.strip() and not ln.lstrip().startswith("#")}


def write_frozen(path: Path, names: set[str]) -> None:
    path.write_text(FROZEN_HEADER + "".join(f"{n}\n" for n in sorted(names)))


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Fail when ci.yml gates a test the frozen list does not carry")
    parser.add_argument("--strict", action="store_true",
                        help="exit 1 when the gate and the frozen list differ")
    parser.add_argument("--update", action="store_true",
                        help=f"rewrite {FROZEN} from the current ci.yml")
    args = parser.parse_args()

    for rel in (CI_WORKFLOW, TESTS_CMAKE):
        if not (REPO / rel).is_file():
            print(f"error: {rel} is missing — has it moved? Update this script.")
            return 2

    names = registered_tests((REPO / TESTS_CMAKE).read_text())
    gated = resolve(gate_patterns((REPO / CI_WORKFLOW).read_text()), names)

    if args.update:
        write_frozen(REPO / FROZEN, set(gated))
        print(f"wrote {FROZEN}: {len(gated)} test(s) on the per-PR gate")
        return 0

    frozen = read_frozen(REPO / FROZEN)
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
        print("already runs on the weekly unfiltered lane (.github/workflows/")
        print("sanitizers.yml); it does not also get a `ctest -R` step in ci.yml.")
        print("Drop the ci.yml step. If a maintainer has decided this test is an")
        print("exception, they add it to the frozen list in the same PR, with the")
        print("reason in the PR body.")
    if removed:
        print()
        print(f"In {FROZEN} but no longer selected by ci.yml:")
        for n in removed:
            print(f"  {n}")
        print("Run `python tools/check_ci_test_gate.py --update` and commit the result.")

    if (added or removed) and args.strict:
        return 1
    if not added and not removed:
        print("ok: per-PR gate matches the frozen list")
    return 0


if __name__ == "__main__":
    sys.exit(main())
