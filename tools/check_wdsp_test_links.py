#!/usr/bin/env python3
"""
AetherSDR WDSP test-link checker.

A test that calls WDSP's C API must link aether_wdsp itself. aethercore links
WDSP PRIVATE and WDSP is built with hidden visibility, so under
AETHER_SHARED_CORE (the sanitizer lanes) libaethercore.so exports none of
WDSP's symbols. The default static-core build still links such a test, because
the archive arrives transitively, so every per-PR build is green and only the
weekly sanitizer run fails, at link (#6283, #6284). Tests normally go through
core/dsp/WdspChannel.h, which needs no WDSP link.

Rule: every target declared in tests/tests.cmake whose literal sources include
a WDSP header (aether_wdsp.h, wdsp_port.h, or anything under third_party/wdsp/
upstream or port), directly or through a quoted repo header, names aether_wdsp
or aether::wdsp in a target_link_libraries() call of its own. A WDSP-including
file under tests/ that no target lists literally is an error too, so a source
moved behind a variable fails closed rather than going unchecked.

The signal is the #include, not the call: a test cannot call WDSP without its
declarations. A hand-written extern "C" declaration would get past it.

Stdlib only, seconds. Usage:
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

BRACKET_COMMENT = re.compile(r"#\[(=*)\[.*?\]\1\]", re.S)
LINE_COMMENT = re.compile(r"#[^\n]*")
C_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"])([^>"]+)[>"]', re.M)
DECL = re.compile(r"\b(add_executable|add_library|target_link_libraries)\s*\(")
SOURCE_EXT = (".c", ".cc", ".cpp", ".cxx", ".mm", ".h", ".hpp")
WDSP_LINK = {"aether_wdsp", "aether::wdsp"}
WDSP_HEADERS = {"aether_wdsp.h", "wdsp_port.h"}
INCLUDE_ROOTS = (REPO / "src", REPO / "tests", REPO)


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

    def __init__(self) -> None:
        self.memo: dict[Path, str | None] = {}

    def wdsp_header(self, path: Path) -> str | None:
        if path in self.memo:
            return self.memo[path]
        self.memo[path] = None  # cycle guard
        try:
            text = C_BLOCK_COMMENT.sub("", path.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            return None
        result = None
        for kind, spelled in INCLUDE.findall(text):
            if is_wdsp_header(spelled):
                result = spelled
                break
            if kind != '"':
                continue
            for root in (path.parent,) + INCLUDE_ROOTS:
                candidate = (root / spelled).resolve()
                if not candidate.is_file() or not candidate.is_relative_to(REPO):
                    continue
                if "third_party" not in candidate.relative_to(REPO).parts:
                    if self.wdsp_header(candidate):
                        result = f"{spelled} -> {self.wdsp_header(candidate)}"
                    break
            if result:
                break
        self.memo[path] = result
        return result


def check() -> tuple[list[str], int]:
    text = (REPO / TESTS_CMAKE).read_text(encoding="utf-8")
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

    includes = Includes()
    errors: list[str] = []
    attributed: set[Path] = set()
    for target, files in sorted(sources.items()):
        for name in files:
            path = (REPO / name).resolve()
            attributed.add(path)
            via = includes.wdsp_header(path)
            if via and not (links.get(target, set()) & WDSP_LINK):
                errors.append(
                    f"{TESTS_CMAKE}: {target}: {name} includes {via}, but {target} "
                    "does not link aether_wdsp. Go through core/dsp/WdspChannel.h, "
                    "or add aether_wdsp to its target_link_libraries().")

    for path in sorted((REPO / "tests").rglob("*")):
        if path.suffix in SOURCE_EXT and path.resolve() not in attributed:
            text = C_BLOCK_COMMENT.sub("", path.read_text(encoding="utf-8", errors="replace"))
            spelled = [s for _, s in INCLUDE.findall(text) if is_wdsp_header(s)]
            if spelled and path.suffix not in (".h", ".hpp"):
                errors.append(
                    f"{path.relative_to(REPO)} includes {spelled[0]} but no target in "
                    f"{TESTS_CMAKE} lists it literally, so its link cannot be checked.")
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
        print(f"{len(errors)} WDSP test-link finding(s).")
        return 1 if args.strict else 0
    print(f"WDSP test links OK ({targets} targets in {TESTS_CMAKE}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
