#!/usr/bin/env python3
"""
AetherSDR per-PR test-gate freeze checker.

The per-PR workflow (.github/workflows/ci.yml) does not run the test suite. It
runs a hand-picked allow-list of tests, each behind its own `ctest -R` step on
the platform job whose toolchain the test's claim is about. That list is
FROZEN: no new test joins it. The full suite runs unfiltered in
.github/workflows/full-suite.yml on every push to main (minutes after a merge,
and the fastest signal a contributor will get) and again in
.github/workflows/sanitizers.yml weekly under ASan+UBSan and TSan. A test
declared in tests/tests.cmake and built by the default configure is on BOTH the
moment it is declared, and that is where a new test belongs.

Why freeze it: every test added to ci.yml is one more step, one more comment
justifying it, one more wall-clock minute on every PR, and one more thing that
reads as "CI runs the tests" when it does not. The gate grew from zero to ~40
tests by accretion, one "rides along for the same reason" at a time; this
script is the ratchet that stops the accretion.

What it checks: every `ctest` invocation in every workflow that runs on
pull_request (any .github/workflows/*.yml with a `pull_request:` trigger;
sanitizers.yml and the push/schedule lanes are not PR gates) must carry `-R <pattern>` (or `--tests-regex`); the pattern is
applied as a regex search over the names registered by add_test() in
tests/tests.cmake, exactly as ctest applies it. A `ctest` line with no
recognised selector — an unfiltered run, `-E`, `-L`, `--tests-from-file` —
fails closed: the script cannot tell what it runs, so it refuses it.
Backslash-continued lines are joined and YAML comment lines are ignored first,
so a wrapped command counts and a comment mentioning one does not. The
resulting set of names must equal the frozen list in .github/ci-test-gate.txt.
Any name a workflow selects that the frozen list does not carry fails the
check with a message pointing at sanitizers.yml.

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
test binary invoked directly (`./build/foo_test`); that is for review to catch.

Usage:
    python tools/check_ci_test_gate.py            # exit 1 on any drift
    python tools/check_ci_test_gate.py --strict   # identical; accepted for symmetry
    python tools/check_ci_test_gate.py --update   # drop stale names from the list
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
WORKFLOWS_DIR = Path(".github/workflows")
WEEKLY_LANE = "sanitizers.yml"
# The `on:` key, and the rest of its line. YAML allows the trigger list as an
# inline scalar (`on: pull_request`), an inline flow sequence
# (`on: [push, pull_request]`) or an indented block — and a workflow file may
# be .yml OR .yaml. Missing any of those shapes silently drops a workflow from
# the scan (#5405 second-opinion review).
ON_KEY = re.compile(r"^on\s*:(.*)$", re.M)
PR_WORD = re.compile(r"\bpull_request(?:_target)?\b")
FROZEN = Path(".github/ci-test-gate.txt")
TESTS_CMAKE = Path("tests/tests.cmake")

# `ctest <anything on the logical line> -R <pattern>`. The pattern is
# double-quoted, single-quoted, or a bare word (`-R asr_gpu_probe_test -V`).
# `--tests-regex` is ctest's long form of the same flag.
CTEST_R = re.compile(
    r"""ctest\b.*?\s(?:-R|--tests-regex)\s+(?:"([^"]+)"|'([^']+)'|(\S+))""")
# Any ctest invocation at all, INCLUDING a path-qualified one: `ctest`,
# `/usr/bin/ctest`, `./build/ctest`. It must sit at a command position — start
# of line, or after whitespace or a shell operator — so `ctest_helper` and
# `--ctest-foo` do not match, but `/usr/bin/ctest` does. An earlier form
# excluded anything preceded by `/`, which let a full path through the
# fail-closed counter entirely (#5405 second-opinion review).
CTEST_ANY = re.compile(r"(?:^|[\s;&|(`])(?:[\w.@+-]*/)*ctest(?=\s|$)", re.M)
YAML_COMMENT_LINE = re.compile(r"^\s*#")
# `$VAR`, `${VAR}`, `$env:VAR` (pwsh), `${{ env.VAR }}` — a `$` regex anchor is
# none of these.
VARIABLE_REF = re.compile(r"\$(?:\{|env:|[A-Za-z_])")

# add_test(NAME foo ...) — the NAME may sit on the line after the paren.
ADD_TEST = re.compile(r"add_test\s*\(\s*NAME\s+(\S+)", re.S)
# foreach(VAR a b c) — the one-level list form tests.cmake uses to register a
# scenario family (app_settings_safety_${APP_SETTINGS_SCENARIO}). RANGE/IN
# LISTS forms are not expanded; a name that still carries a ${…} after
# expansion is reported rather than resolved.
FOREACH = re.compile(r"foreach\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s+([^)]*)\)", re.S)
VAR_IN_NAME = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}")
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
    """add_test() names, with `${VAR}` expanded from the plain-list foreach()
    that declares VAR. A name this cannot expand is left out and reported if a
    -R pattern then matches nothing."""
    code = COMMENT.sub("", BRACKET_COMMENT.sub("", text))
    lists: dict[str, list[str]] = {}
    for var, items in FOREACH.findall(code):
        words = [w for w in items.split() if w not in ("IN", "LISTS", "ITEMS", "RANGE")]
        if words and "$" not in "".join(words):
            lists[var] = words
    names: set[str] = set()
    for raw in ADD_TEST.findall(code):
        m = VAR_IN_NAME.search(raw)
        if m is None and "$" not in raw:
            names.add(raw)
        elif m is not None and m.group(1) in lists and VAR_IN_NAME.sub("", raw).count("$") == 0:
            names.update(raw.replace(m.group(0), item) for item in lists[m.group(1)])
        else:
            VARIABLE_NAMES.append(raw)
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


def gate_patterns(workflow: Path, text: str) -> list[tuple[str, str, int]]:
    """Every -R pattern in one workflow as (pattern, file, line). A ctest
    invocation without a recognised selector is an error: the script cannot
    say what it runs, so it refuses it rather than assuming nothing."""
    out: list[tuple[str, str, int]] = []
    for line, lineno in logical_lines(text):
        selectors = list(CTEST_R.finditer(line))
        if len(CTEST_ANY.findall(line)) > len(selectors):
            print(f"error: {workflow}:{lineno}: a ctest invocation with no "
                  "-R/--tests-regex selector. The per-PR gate is frozen; a "
                  "ctest run this script cannot classify (unfiltered, -E, -L, "
                  "--tests-from-file) is refused. The full suite runs on "
                  f"{WORKFLOWS_DIR / WEEKLY_LANE}.")
            sys.exit(2)
        for m in selectors:
            raw = next(g for g in m.groups() if g is not None)
            if VARIABLE_REF.search(raw):
                print(f"error: {workflow}:{lineno}: -R pattern {raw!r} "
                      "references a variable. Write the pattern inline so the "
                      "frozen list is reviewable without resolving env: blocks.")
                sys.exit(2)
            out.append((raw, str(workflow), lineno))
    return out


def resolve(patterns: list[tuple[str, str, int]], names: set[str]) -> dict[str, list[str]]:
    """Name -> workflow:line sites that select it, by regex search like
    ctest -R."""
    gated: dict[str, list[str]] = {}
    for pattern, wf, line in patterns:
        try:
            rx = re.compile(pattern)
        except re.error as exc:
            print(f"error: {wf}:{line}: cannot parse -R pattern "
                  f"{pattern!r}: {exc}")
            sys.exit(2)
        hits = sorted(n for n in names if rx.search(n))
        if not hits:
            print(f"error: {wf}:{line}: -R pattern {pattern!r} "
                  f"matches no literal add_test() name in {TESTS_CMAKE}")
            if VARIABLE_NAMES:
                print("  (names built from variables are not resolved: "
                      + ", ".join(VARIABLE_NAMES) + ")")
            sys.exit(2)
        for n in hits:
            gated.setdefault(n, []).append(f"{wf}:{line}")
    return gated


def has_pr_trigger(text: str) -> bool:
    """True if the workflow's `on:` names pull_request in any valid shape."""
    m = ON_KEY.search(text)
    if not m:
        return False
    if PR_WORD.search(m.group(1).split("#", 1)[0]):
        return True  # `on: pull_request` / `on: [push, pull_request]`
    for line in text[m.end():].split("\n")[1:]:
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if not line[:1].isspace():
            break  # dedented to the next top-level key; the on: block ended
        if PR_WORD.search(line.split("#", 1)[0]):
            return True
    return False


def pr_workflows() -> list[Path]:
    """Every workflow with a pull_request trigger, sorted. A new workflow
    file is scanned the moment it exists, so a `ctest` step cannot hide in
    one this script was never told about — either extension, any `on:` shape."""
    candidates = sorted(set((REPO / WORKFLOWS_DIR).glob("*.yml"))
                        | set((REPO / WORKFLOWS_DIR).glob("*.yaml")))
    found = [p for p in candidates
             if p.name != WEEKLY_LANE and has_pr_trigger(p.read_text())]
    if not found:
        print(f"error: no workflows found under {WORKFLOWS_DIR} — has it moved? "
              "Update this script.")
        sys.exit(2)
    return [p.relative_to(REPO) for p in found]


def read_frozen(path: Path) -> tuple[list[str], dict[str, str]]:
    """(header comment lines, {name: the whole line it came from}).

    A name may carry a trailing `# why` comment saying which platform claim
    keeps it on the gate — the file is maintainer-owned precisely so that a
    one-line diff is reviewable, and a bare name tells a reviewer nothing. Only
    the first whitespace-separated field is the name; the rest is prose, and
    --update preserves it verbatim for every name it keeps."""
    if not path.is_file():
        return FROZEN_HEADER.splitlines(), {}
    header: list[str] = []
    names: dict[str, str] = {}
    for ln in path.read_text().splitlines():
        s = ln.strip()
        if not s:
            continue
        if s.startswith("#"):
            header.append(ln.rstrip())
        else:
            names[s.split("#", 1)[0].split()[0]] = ln.rstrip()
    return header, names


def write_frozen(path: Path, header: list[str], lines: dict[str, str]) -> None:
    body = "".join(f"{lines[n]}\n" for n in sorted(lines))
    path.write_text("\n".join(header) + "\n" + body)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Fail when ci.yml gates a test the frozen list does not carry")
    parser.add_argument("--strict", action="store_true",
                        help="accepted for symmetry with the sibling checkers; "
                             "drift exits non-zero either way")
    parser.add_argument("--update", action="store_true",
                        help=f"drop names from {FROZEN} that no workflow "
                             "selects any more (never adds)")
    args = parser.parse_args()

    if not (REPO / TESTS_CMAKE).is_file():
        print(f"error: {TESTS_CMAKE} is missing — has it moved? Update this script.")
        return 2

    names = registered_tests((REPO / TESTS_CMAKE).read_text())
    workflows = pr_workflows()
    patterns: list[tuple[str, str, int]] = []
    for wf in workflows:
        patterns += gate_patterns(wf, (REPO / wf).read_text())
    gated = resolve(patterns, names)
    header, frozen_lines = read_frozen(REPO / FROZEN)
    frozen = set(frozen_lines)
    added = sorted(set(gated) - frozen)
    removed = sorted(frozen - set(gated))

    print(f"{len(workflows)} workflow(s) scanned: {len(gated)} test(s) selected "
          f"by -R; {FROZEN}: {len(frozen)} frozen; "
          f"{len(names)} registered in {TESTS_CMAKE}")

    if added:
        print()
        print(f"NEW on the per-PR gate but not in {FROZEN}:")
        for n in added:
            where = ", ".join(gated[n])
            print(f"  {n}  ({where})")
        print()
        print("The per-PR test gate is frozen. A test declared in tests/tests.cmake")
        print("and built by the default configure already runs unfiltered on every")
        print("push to main (.github/workflows/full-suite.yml) and again weekly")
        print("(.github/workflows/sanitizers.yml); it does not")
        print("also get a `ctest -R` step on a PR workflow. Drop the step. If a")
        print("maintainer has decided this test is an exception, add its name to")
        print(f"{FROZEN} by hand in the same PR, with the reason in the PR body.")
        print("--update will not do that for you.")
    if removed:
        print()
        print(f"In {FROZEN} but no longer selected by any workflow:")
        for n in removed:
            print(f"  {n}")
        if not args.update:
            print("Run `python tools/check_ci_test_gate.py --update` and commit the result.")

    if args.update:
        if added:
            return 1
        if removed:
            kept = {n: l for n, l in frozen_lines.items() if n not in set(removed)}
            write_frozen(REPO / FROZEN, header, kept)
            print(f"wrote {FROZEN}: {len(frozen) - len(removed)} test(s) on the per-PR gate")
        else:
            print("ok: nothing to remove")
        return 0

    if added or removed:
        # Non-zero WITHOUT --strict too. The bare mode used to print all of the
        # above and then exit 0, so `python tools/check_ci_test_gate.py` by hand
        # reported a violation and passed — the one thing a ratchet must never
        # do (#5405 review). --strict is kept as an accepted no-op so
        # static-checks.yml and the sibling checkers' muscle memory keep working.
        return 1
    print("ok: per-PR gate matches the frozen list")
    return 0


if __name__ == "__main__":
    sys.exit(main())
