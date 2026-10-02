#!/usr/bin/env python3
"""
QSettings ratchet: application settings go through AppSettings, never QSettings
(docs/agents/settings.md).

A file under src/ may use the QSettings type only if it is listed in ALLOWED,
with the reason it needs Qt's own store rather than ours. Only src/ is
scanned: test fixtures that drive the legacy-store migration use QSettings on
purpose. The list only
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


# A numeric literal's digit separators (1'000, 0xFF'FF) are not quotes. Drop
# them before stripping, or the span between two of them reads as a char
# literal and hides the code on that line. A digit right after a quote or a
# word character ('1', u8'x') does not start a numeric literal.
_NUMBER = re.compile(r"(?<!['\w])\d[\w.']*")


def uses_qsettings(text: str) -> bool:
    text = _NUMBER.sub(lambda m: m.group(0).replace("'", ""), text)
    return bool(_TOKEN.search(_STRIP.sub(" ", text)))


_SELF_TEST = [
    ("QSettings s;", True),
    ("int a = 1'000; QSettings settings; int b = 2'000;", True),
    ("auto m = 0xFF'FF; QSettings s; auto n = 0b1010'1010;", True),
    ("char c = '1'; QSettings s; char d = '2';", True),
    ("auto c = u8'x'; QSettings s; auto d = u8'y';", True),
    ("// QSettings is banned\nconst char* x = \"QSettings\";", False),
    ("void migrateFromQSettings();", False),
    ("int a = 1'000; /* QSettings */ int b = 2'000;", False),
]


def self_test() -> list[str]:
    return [f"self-test: uses_qsettings({src!r}) should be {want}"
            for src, want in _SELF_TEST if uses_qsettings(src) != want]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    parser.add_argument("--strict", action="store_true", help="exit 1 on any finding")
    args = parser.parse_args()

    broken = self_test()
    if broken:
        for line in broken:
            print(f"QSETTINGS: {line}")
        return 1

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
