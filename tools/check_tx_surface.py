#!/usr/bin/env python3
"""
AetherSDR transmit-surface gate (Principle VI).

CODEOWNERS routes review by path, but the code that keys the transmitter is not
in one place: it runs through RadioModel.cpp, the MainWindow_*.cpp siblings,
TciServer.cpp and every backend, all Tier 3. A change to keying there reaches
the same reviewers as a tooltip fix. This checker routes it by what the diff
does instead: when a PR's diff reaches the transmit surface listed in
.github/tx-surface.txt, the PR must also add .github/tx-acks/<PR number>.md
stating what changes on the transmit path and how it was verified. A code
owner's approval (@aethersdr/reviewers owns that directory) then covers an
explicit TX disclosure rather than a keying change buried in a large diff, and
the existing stale-approval dismissal makes it cover the head that merges.

What it reads: every added or removed line under src/ that is not
comment-only. A `path` entry fires when such a line lands in a listed file; a
`token` entry fires when such a line anywhere under src/ matches it. A reworded
comment, even in TxController.cpp, does not trigger the gate.

What it cannot see: keying reached through a name the list does not carry. The
list is Tier 1 for that reason; growing it is the remedy, and shrinking it is a
maintainer-reviewed diff.

Usage:
    python tools/check_tx_surface.py                     # report against origin/main
    python tools/check_tx_surface.py --base HEAD^1 --pr 6500   # CI: enforce
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SURFACE = ".github/tx-surface.txt"
ACK_DIR = ".github/tx-acks"

# `*` only when it reads as a block-comment continuation (`* text`, `*/`), so
# a statement such as `*m_keyed = true;` is still code. No `#`: in C++ that is
# the preprocessor, and `#define KEY "xmit 1"` keys as surely as a call.
COMMENT_START = re.compile(r"^\s*(//|/\*|\*(\s|/|$))")


def comment_only(body: str) -> bool:
    if not COMMENT_START.match(body):
        return False
    # `/* note */ sendCmd("xmit 1");` carries code after the comment closes.
    head, sep, tail = body.partition("*/")
    return not (sep and tail.strip() and not tail.strip().startswith("//"))


@dataclass
class Surface:
    paths: list[re.Pattern[str]]
    tokens: list[re.Pattern[str]]


def load_surface(text: str) -> Surface:
    paths: list[re.Pattern[str]] = []
    tokens: list[re.Pattern[str]] = []
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        kind, _, rest = line.partition(" ")
        pattern = re.sub(r"\s+#.*$", "", rest).strip()
        if kind not in ("path", "token") or not pattern:
            raise ValueError(f"{SURFACE}:{lineno}: expected `path <regex>` or `token <regex>`")
        (paths if kind == "path" else tokens).append(re.compile(pattern))
    if not paths or not tokens:
        raise ValueError(f"{SURFACE}: needs at least one path and one token entry")
    return Surface(paths, tokens)


def git(repo: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(repo), *args], check=True,
                          capture_output=True, text=True, errors="replace").stdout


def default_base(repo: Path) -> str:
    return git(repo, "merge-base", "origin/main", "HEAD").strip()


def triggers(diff: str, surface: Surface) -> list[str]:
    """Return a human-readable reason for every hit in a `git diff -U0` text."""
    found: list[str] = []
    current: str | None = None
    for line in diff.splitlines():
        if line.startswith("diff --git "):
            current = None
            continue
        if line.startswith(("--- ", "+++ ")):
            name = line[4:]
            if name != "/dev/null":
                current = name[2:]
            continue
        if current is None or not current.startswith("src/"):
            continue
        if line[:1] not in "+-":
            continue
        body = line[1:]
        if comment_only(body):
            continue
        for p in surface.paths:
            if p.search(current):
                reason = f"{current}: file is on the TX surface ({p.pattern})"
                if reason not in found:
                    found.append(reason)
        for t in surface.tokens:
            m = t.search(body)
            if m:
                reason = f"{current}: {line[0]} line uses `{m.group(0)}`"
                if reason not in found:
                    found.append(reason)
    return found


def added_files(repo: Path, base: str) -> set[str]:
    out = git(repo, "diff", "--name-only", "--diff-filter=A", f"{base}", "HEAD")
    return set(out.split())


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--base", help="diff base (default: merge-base with origin/main)")
    ap.add_argument("--pr", type=int, help="PR number; when given, a hit without its ack fails")
    ap.add_argument("--repo", type=Path, default=REPO, help=argparse.SUPPRESS)
    args = ap.parse_args(argv)

    repo: Path = args.repo
    surface = load_surface((repo / SURFACE).read_text())
    base = args.base or default_base(repo)
    diff = git(repo, "diff", "-U0", "--no-color", "--no-ext-diff", base, "HEAD")
    hits = triggers(diff, surface)

    if not hits:
        print("TX surface: not touched.")
        return 0

    print("TX surface touched (Principle VI):")
    for h in hits:
        print(f"  {h}")

    if args.pr is None:
        print(f"\nA PR with this diff must add {ACK_DIR}/<PR number>.md.")
        return 0

    ack = f"{ACK_DIR}/{args.pr}.md"
    if ack not in added_files(repo, base):
        print(f"::error::This PR changes the transmit path. Add {ack} with one or two "
              f"lines saying what changes on the TX path and how it was verified. "
              f"Any code owner's approval covers it (GOVERNANCE.md "
              f"§Transmit-path changes).")
        return 1
    if not (repo / ack).read_text().strip():
        print(f"::error::{ack} is empty; say what changes on the TX path.")
        return 1
    print(f"\nAcknowledged in {ack}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
