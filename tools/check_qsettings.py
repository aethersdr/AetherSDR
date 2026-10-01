#!/usr/bin/env python3
"""
QSettings ratchet: application settings go through AppSettings, never QSettings
(docs/agents/settings.md).

A file under src/ may use the QSettings type only if it is listed in ALLOWED,
with the reason it needs Qt's own store rather than ours. The list only
shrinks: a listed file that no longer uses QSettings is also an error, so the
entry is removed in the same change that removes the use.

Comments and string literals are ignored, and so are identifiers that merely
contain the word (e.g. migrateFromQSettings).

Usage:
    python tools/check_qsettings.py           # report, exit 0
    python tools/check_qsettings.py --strict  # exit 1 on any finding
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "src"

ALLOWED = {
    "src/core/AppSettings.cpp": "one-time import of the legacy QSettings store",
    "src/core/TciPeerProcess.cpp": "reads a macOS app bundle's Info.plist",
}

_TOKEN = re.compile(r"\bQSettings\b")
_STRIP = re.compile(
    r'//[^\n]*|/\*.*?\*/|R"([^(\s]*)\(.*?\)\1"|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'',
    re.S,
)


def uses_qsettings(text: str) -> bool:
    return bool(_TOKEN.search(_STRIP.sub(" ", text)))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    parser.add_argument("--strict", action="store_true", help="exit 1 on any finding")
    args = parser.parse_args()

    users = set()
    for path in sorted(SRC.rglob("*")):
        if path.suffix not in {".cpp", ".h", ".hpp", ".mm"}:
            continue
        if uses_qsettings(path.read_text(encoding="utf-8", errors="replace")):
            users.add(path.relative_to(REPO).as_posix())

    findings = []
    for rel in sorted(users - ALLOWED.keys()):
        findings.append(f"{rel}: uses QSettings; store settings through AppSettings instead")
    for rel in sorted(ALLOWED.keys() - users):
        findings.append(f"{rel}: listed in ALLOWED but no longer uses QSettings; remove the entry")

    for line in findings:
        print(f"QSETTINGS: {line}")
    if not findings:
        print(f"QSettings ratchet: OK ({len(users)} allowed use(s))")
    return 1 if findings and args.strict else 0


if __name__ == "__main__":
    sys.exit(main())
