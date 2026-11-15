#!/usr/bin/env python3
"""Where a release's PDF manual gets its signature, and when to re-sign.

From RFC #6964 the manual is listed in SHA256SUMS.txt, and SHA256SUMS.txt.asc
is its only signature. Releases whose Docs workflow still signs the manual
apart keep that shape. A dispatch of today's signing workflow for an old tag
keeps it too, so the producer (sign-release.yml) and the checker
(check_release_assets.py) answer the question from the same file: the tag's
own .github/workflows/docs.yml, which is the copy that ran for that tag.

    release_manual_signing.py shape --tree DIR
        Prints "sums" when the manual belongs in SHA256SUMS.txt, "apart" when
        the tag's Docs workflow signs it on its own, and "none" when the tag
        has no manual.

    release_manual_signing.py resign --name NAME [--pdf FILE] [--sums FILE]
        Run after Docs attaches the manual. Prints "resign" when the signed
        SHA256SUMS.txt doesn't cover the manual now attached, either because
        signing ran without it or because a Docs rerun replaced it. Prints
        "skip: <reason>" otherwise. Exit 0 either way.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path


def shape(tree: Path) -> str:
    if not ((tree / "tools/docs/build_pdf.py").is_file() and (tree / "docs/user/sidebars.js").is_file()):
        return "none"
    docs = tree / ".github/workflows/docs.yml"
    if docs.is_file() and "--detach-sign" in docs.read_text(encoding="utf-8", errors="replace"):
        return "apart"
    return "sums"


def sums_entry(sums_text: str, name: str) -> str | None:
    """The SHA-256 SHA256SUMS.txt lists for NAME, if any (sha256sum's text and
    binary forms)."""
    for line in sums_text.splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2 and parts[1].lstrip("*") == name:
            return parts[0].lower()
    return None


def resign(name: str, pdf_sha: str | None, sums_text: str | None) -> str:
    if pdf_sha is None:
        return "skip: the manual is not attached"
    if sums_text is None:
        # No signing run has written the sums yet. Whichever run does will
        # find the manual attached and list it.
        return "skip: nothing is signed yet"
    listed = sums_entry(sums_text, name)
    if listed is None:
        return "resign"
    if listed != pdf_sha.lower():
        return "resign"
    return "skip: SHA256SUMS.txt already covers this manual"


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("shape")
    s.add_argument("--tree", type=Path, default=Path("."))
    r = sub.add_parser("resign")
    r.add_argument("--name", required=True)
    r.add_argument("--pdf", type=Path)
    r.add_argument("--sums", type=Path)
    args = ap.parse_args(argv)
    if args.cmd == "shape":
        print(shape(args.tree))
        return 0
    pdf_sha = _sha256(args.pdf) if args.pdf and args.pdf.is_file() else None
    sums = args.sums.read_text(encoding="utf-8") if args.sums and args.sums.is_file() else None
    print(resign(args.name, pdf_sha, sums))
    return 0


if __name__ == "__main__":
    sys.exit(main())
