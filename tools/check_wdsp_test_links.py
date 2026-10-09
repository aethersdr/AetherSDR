#!/usr/bin/env python3
"""
AetherSDR WDSP test-boundary checker.

A test reaches WDSP only through core/dsp/WdspChannel.h. aethercore links WDSP
PRIVATE and WDSP is built with hidden visibility, so under AETHER_SHARED_CORE
(the sanitizer lanes) libaethercore.so exports none of WDSP's symbols: a test
that calls WDSP's C API while linking only aethercore links in the static build
every PR uses and fails only in the weekly sanitizer run, at link (#6283,
#6284). Linking aether_wdsp as well makes it build, but then the test drives a
SECOND copy of WDSP, with its own channel table, allocation counters and
thread-locals, which the core never touches: every check it makes through that
copy is vacuous. So the fix is a WdspChannel *ForTest hook, never a link.

Rules, over tests/tests.cmake and tests/:
  1. No source under tests/, and no source a target in tests/tests.cmake lists
     literally, includes a WDSP header (aether_wdsp.h, wdsp_port.h, or anything
     under wdsp/upstream/ or wdsp/port/), directly or through quoted repo
     headers followed transitively.
  2. No target in tests/tests.cmake links aether_wdsp / aether::wdsp.
Both exempt the targets in ALLOWLIST, each of which must exist, must not link
aethercore (that is the double copy again), and must still reach WDSP, or the
entry is stale and is itself a finding.

The signal is the #include, not the call: a test cannot call WDSP without its
declarations, so a hand-written extern "C" declaration gets past rule 1 (rule 2
still catches it unless the symbol arrives through aethercore's static
archive). A source reached only through a ${variable} is not seen by the
target attribution, which is why every file under tests/ is scanned whatever
lists it.

Known limit, by design: this is the cheap static form of the invariant. The
complete form is a shared-core link of every test on each PR; the maintainer
weighed its per-PR cost and chose this check (#6283). The other static
libraries aethercore links PRIVATE (zlibstatic, mspack_static) have the same
shape and are not covered here.

Stdlib only, under a second. Usage:
    python tools/check_wdsp_test_links.py            # report, exit 0
    python tools/check_wdsp_test_links.py --strict   # exit 1 on a finding
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TESTS_CMAKE = Path("tests/tests.cmake")

# target -> why it may reach WDSP directly. Each one is a pure WDSP unit test
# that never links aethercore, so there is exactly one WDSP in the process.
ALLOWLIST: dict[str, str] = {
    "wdsp_allocation_scope_test":
        "unit test of wdsp_port's allocation counters themselves; links "
        "aether_wdsp and no aethercore, so the WDSP it drives is the only one",
}

BRACKET_COMMENT = re.compile(r"#\[(=*)\[.*?\]\1\]", re.S)
LINE_COMMENT = re.compile(r"#[^\n]*")
C_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"])([^>"]+)[>"]', re.M)
DECL = re.compile(r"\b(add_executable|add_library|target_link_libraries)\s*\(")
SOURCE_EXT = (".c", ".cc", ".cpp", ".cxx", ".mm", ".h", ".hpp", ".inc")
WDSP_LINK = {"aether_wdsp", "aether::wdsp"}
CORE_LINK = {"aethercore"}
WDSP_HEADERS = {"aether_wdsp.h", "wdsp_port.h"}
ADVICE = ("Go through core/dsp/WdspChannel.h (add a *ForTest hook beside "
          "allocationSequenceForTest() if one is missing). Do not link aether_wdsp: "
          "under AETHER_SHARED_CORE that gives the test its own WDSP copy, which "
          "the core never touches.")


def is_wdsp_header(spelled: str) -> bool:
    path = spelled.replace("\\", "/")
    return (path.rsplit("/", 1)[-1] in WDSP_HEADERS
            or "wdsp/upstream/" in path or "wdsp/port/" in path)


def calls(text: str) -> list[tuple[str, list[str]]]:
    """(command, argument tokens) for each declaration command in the text."""
    found = []
    for match in DECL.finditer(text):
        depth, pos = 1, match.end()
        while depth and pos < len(text):
            depth += {"(": 1, ")": -1}.get(text[pos], 0)
            pos += 1
        found.append((match.group(1), text[match.end():pos - 1].split()))
    return found


class Includes:
    """Which repo files reach a WDSP header, following quoted repo includes."""

    def __init__(self, repo: Path) -> None:
        self.repo = repo
        self.roots = (repo / "src", repo / "tests", repo)
        self.memo: dict[Path, str | None] = {}
        self.visiting: dict[Path, int] = {}  # file -> its depth on the stack
        self.low = 1 << 30  # shallowest stack depth a cycle cut returned to

    def wdsp_header(self, path: Path) -> str | None:
        """The include chain to a WDSP header, or None.

        A file met again while it is still being scanned (an include cycle)
        contributes nothing to that scan. A "no" reached across such a cut is
        provisional, because the file the cycle returned to has not finished:
        memoising it would answer "no WDSP" for good for a file that reaches
        WDSP through that ancestor. So only final answers are memoised: any
        "yes", and a "no" whose scan cut no cycle above itself."""
        if path in self.memo:
            return self.memo[path]
        if path in self.visiting:
            self.low = min(self.low, self.visiting[path])
            return None
        depth = len(self.visiting)
        self.visiting[path] = depth
        outer, self.low = self.low, 1 << 30
        try:
            result = self._scan(path)
        finally:
            del self.visiting[path]
            low, self.low = self.low, outer
        if result is not None or low >= depth:
            self.memo[path] = result
        else:
            self.low = min(self.low, low)
        return result

    def _scan(self, path: Path) -> str | None:
        try:
            text = C_BLOCK_COMMENT.sub("", path.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            return None
        for kind, spelled in INCLUDE.findall(text):
            if is_wdsp_header(spelled):
                return spelled
            if kind != '"':
                continue
            for root in (path.parent,) + self.roots:
                candidate = (root / spelled).resolve()
                if not candidate.is_file() or not candidate.is_relative_to(self.repo):
                    continue
                if "third_party" not in candidate.relative_to(self.repo).parts:
                    via = self.wdsp_header(candidate)
                    if via:
                        return f"{spelled} -> {via}"
                break
        return None


def check(repo: Path = REPO,
          allowlist: dict[str, str] | None = None) -> tuple[list[str], int]:
    allowlist = ALLOWLIST if allowlist is None else allowlist
    text = (repo / TESTS_CMAKE).read_text(encoding="utf-8")
    text = LINE_COMMENT.sub("", BRACKET_COMMENT.sub("", text))
    sources: dict[str, list[str]] = {}
    links: dict[str, set[str]] = {}
    for command, args in calls(text):
        if not args or "$" in args[0]:
            continue
        if command == "target_link_libraries":
            links.setdefault(args[0], set()).update(args[1:])
        else:
            sources.setdefault(args[0], []).extend(
                a for a in args[1:] if a.endswith(SOURCE_EXT) and "$" not in a)

    includes = Includes(repo)
    errors: list[str] = []
    exempt: set[Path] = set()
    for target, _why in sorted(allowlist.items()):
        if target not in sources:
            errors.append(f"ALLOWLIST: {target} is not a target in {TESTS_CMAKE}; "
                          "drop the stale entry.")
            continue
        if links.get(target, set()) & CORE_LINK:
            errors.append(f"ALLOWLIST: {target} links aethercore as well as reaching WDSP "
                          "directly, so it holds two WDSP copies. " + ADVICE)
        paths = [(repo / name).resolve() for name in sources[target]]
        exempt.update(paths)
        if not any(includes.wdsp_header(p) for p in paths):
            errors.append(f"ALLOWLIST: {target} no longer includes a WDSP header; "
                          "drop the stale entry.")

    for target, linked in sorted(links.items()):
        if target not in allowlist and linked & WDSP_LINK:
            errors.append(f"{TESTS_CMAKE}: {target} links aether_wdsp. " + ADVICE)

    attributed: set[Path] = set()
    for target, files in sorted(sources.items()):
        if target in allowlist:
            continue
        for name in files:
            path = (repo / name).resolve()
            attributed.add(path)
            via = includes.wdsp_header(path)
            if via:
                errors.append(f"{TESTS_CMAKE}: {target}: {name} includes {via}. " + ADVICE)

    for path in sorted((repo / "tests").rglob("*")):
        resolved = path.resolve()
        if (path.suffix in SOURCE_EXT and path.is_file()
                and resolved not in attributed and resolved not in exempt):
            via = includes.wdsp_header(resolved)
            if via:
                errors.append(f"{path.relative_to(repo)} includes {via}. " + ADVICE)
    return errors, len(sources)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--strict", action="store_true", help="exit 1 on a finding")
    args = parser.parse_args()
    errors, targets = check()
    if targets < 100:
        print(f"error: parsed only {targets} targets from {TESTS_CMAKE}; "
              "the scanner no longer understands the file.")
        return 1
    for error in errors:
        print(f"error: {error}")
    if errors:
        print(f"{len(errors)} WDSP test-boundary finding(s).")
        return 1 if args.strict else 0
    print(f"WDSP test boundary OK ({targets} targets in {TESTS_CMAKE}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
