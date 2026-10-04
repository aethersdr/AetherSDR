#!/usr/bin/env python3
"""
AetherSDR build-options reference checker.

BUILD-OPTIONS.md is the one catalog of project-defined CMake options (#6097).
Options are added in ordinary feature and CI PRs that have no reason to open
that file, so without a check the catalog goes stale on its own.

Compares, in both directions:

* every option() in the project's tracked CMake files (vendored trees under
  third_party/ excluded) and every non-FORCE, non-INTERNAL cache set();
* every row of an "Option" or "Setting" table in BUILD-OPTIONS.md.

A finding is a definition with no row, a row with no definition, or an option
whose literal ON/OFF default differs from the row's default. Computed defaults
(`${VAR}`) are not compared; their row describes the computation.

Usage:
    python tools/check_build_options.py            # report, exit 0
    python tools/check_build_options.py --strict   # exit 1 on any finding
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DOC = "BUILD-OPTIONS.md"

COMMAND_RE = re.compile(r"(?<![A-Za-z0-9_])(cmake_dependent_option|option|set)\s*\(", re.IGNORECASE)
BRACKET_OPEN_RE = re.compile(r"\[(=*)\[")
TOKEN_RE = re.compile(r'"(?:\\.|[^"\\])*"|[^\s"]+')
ROW_RE = re.compile(r"^\|\s*`([A-Za-z0-9_]+)`\s*\|\s*([^|]*?)\s*\|")
DOC_TABLE_HEADERS = {"option", "setting"}


def cmake_files(repo: Path) -> list[Path]:
    try:
        out = subprocess.run(
            ["git", "-C", str(repo), "ls-files", "--", "*CMakeLists.txt", "*.cmake"],
            check=True, capture_output=True, text=True).stdout.split()
    except (OSError, subprocess.CalledProcessError):
        out = []
        for root, dirs, files in os.walk(repo):
            dirs[:] = [d for d in dirs if not d.startswith((".", "build"))]
            for name in files:
                if name == "CMakeLists.txt" or name.endswith(".cmake"):
                    out.append(os.path.relpath(os.path.join(root, name), repo))
    rels = [r.replace(os.sep, "/") for r in out]
    return [repo / r for r in sorted(rels)
            if not r.startswith("third_party/") and "/third_party/" not in r]


def strip_comments(text: str) -> str:
    """Remove line and bracket comments, keeping quoted and bracket arguments."""
    out, i, in_quote = [], 0, False
    while i < len(text):
        ch = text[i]
        if in_quote:
            out.append(ch)
            if ch == "\\" and i + 1 < len(text):
                out.append(text[i + 1])
                i += 1
            elif ch == '"':
                in_quote = False
        elif ch == '"':
            in_quote = True
            out.append(ch)
        elif ch == "#":
            bracket = BRACKET_OPEN_RE.match(text, i + 1)
            if bracket:
                close = text.find("]" + bracket.group(1) + "]", bracket.end())
                i = len(text) if close < 0 else close + len(bracket.group(1)) + 2
                continue
            newline = text.find("\n", i)
            i = len(text) if newline < 0 else newline
            continue
        elif BRACKET_OPEN_RE.match(text, i):
            bracket = BRACKET_OPEN_RE.match(text, i)
            close = text.find("]" + bracket.group(1) + "]", bracket.end())
            end = len(text) if close < 0 else close + len(bracket.group(1)) + 2
            out.append(text[i:end])
            i = end
            continue
        else:
            out.append(ch)
        i += 1
    return "".join(out)


def commands(text: str):
    """Yield (command, args) for each option()/set() call, parens balanced."""
    text = strip_comments(text)
    for m in COMMAND_RE.finditer(text):
        depth, i, in_quote = 1, m.end(), False
        while i < len(text) and depth:
            ch = text[i]
            if ch == '"' and text[i - 1] != "\\":
                in_quote = not in_quote
            elif not in_quote:
                depth += {"(": 1, ")": -1}.get(ch, 0)
            i += 1
        yield m.group(1).lower(), TOKEN_RE.findall(text[m.end():i - 1])


def collect_definitions(texts: dict[str, str]) -> tuple[dict[str, str], set[str]]:
    """Return ({option: default}, {cache setting}) across the given files."""
    options: dict[str, str] = {}
    settings: set[str] = set()
    for text in texts.values():
        for cmd, args in commands(text):
            if not args:
                continue
            if cmd == "option":
                options[args[0]] = args[2].strip('"') if len(args) >= 3 else "OFF"
            elif cmd == "cmake_dependent_option":
                options[args[0]] = args[2].strip('"') if len(args) >= 3 else ""
            elif "CACHE" in args and "FORCE" not in args:
                kind = args[args.index("CACHE") + 1] if args.index("CACHE") + 1 < len(args) else ""
                if kind != "INTERNAL":
                    settings.add(args[0])
    return options, settings


def collect_documented(text: str) -> dict[str, str]:
    """Return {name: default cell} from the doc's Option/Setting tables."""
    rows: dict[str, str] = {}
    lines = text.splitlines()
    active = False
    for i, line in enumerate(lines):
        if line.startswith("|") and i + 1 < len(lines) and lines[i + 1].startswith("|---"):
            active = line.strip("| ").split("|")[0].strip().lower() in DOC_TABLE_HEADERS
            continue
        if not line.startswith("|"):
            active = False
            continue
        m = ROW_RE.match(line)
        if active and m:
            rows[m.group(1)] = m.group(2)
    return rows


def compare(options: dict[str, str], settings: set[str],
            documented: dict[str, str]) -> list[str]:
    findings = []
    defined = set(options) | settings
    for name in sorted(defined - set(documented)):
        findings.append(f"{name}: defined in CMake but missing from {DOC}")
    for name in sorted(set(documented) - defined):
        findings.append(f"{name}: listed in {DOC} but not defined in any project CMake file")
    for name in sorted(set(options) & set(documented)):
        default = options[name].upper()
        if default in ("ON", "OFF") and documented[name].upper() != default:
            findings.append(f"{name}: CMake default is {default}, "
                            f"{DOC} says '{documented[name]}'")
    return findings


def run(repo: Path) -> list[str]:
    texts = {str(p.relative_to(repo)): p.read_text(encoding="utf-8", errors="replace")
             for p in cmake_files(repo)}
    options, settings = collect_definitions(texts)
    documented = collect_documented((repo / DOC).read_text(encoding="utf-8"))
    return compare(options, settings, documented)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--strict", action="store_true", help="exit 1 on any finding")
    args = parser.parse_args()
    findings = run(REPO)
    for finding in findings:
        print(f"BUILD-OPTIONS: {finding}")
    if not findings:
        print(f"BUILD-OPTIONS: {DOC} matches every project option and cache setting")
    return 1 if findings and args.strict else 0


if __name__ == "__main__":
    sys.exit(main())
