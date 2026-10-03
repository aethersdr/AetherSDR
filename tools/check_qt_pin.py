#!/usr/bin/env python3
"""
AetherSDR Qt-pin consistency checker.

cmake/qt-pin.env is the one statement of which Qt every artifact ships and
every CI leg builds (#5487). The workflows and the CI Dockerfile cannot read it
— a GitHub Actions `env:` block is evaluated before any step could source a
file, and `jurplel/install-qt-action` takes literals — so each one still spells
the values out. Before this check, "bump all seven together" was a comment;
the 6.12 migration needed a reviewer to grep for stragglers by hand.

Two passes:

1. KNOWN SITES. Every place a pin value is spelled is listed in SITES below
   with the pin key it must equal. Each site must match at least once — a
   regex that silently stops matching after an edit would otherwise turn this
   gate into a no-op — and every match must equal the pin.

2. UNKNOWN SITES. Any Qt-looking version literal (6.x.y) on a non-comment line
   of a workflow or Dockerfile that is neither the pin nor the floor is a
   finding. That is what catches the NEXT place someone spells a version,
   which pass 1 cannot know about.

Usage:
    python tools/check_qt_pin.py            # report, exit 0
    python tools/check_qt_pin.py --strict   # exit 1 on any drift
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PIN_FILE = REPO / "cmake" / "qt-pin.env"

CI = ".github/workflows/ci.yml"
APPIMAGE = ".github/workflows/appimage.yml"
MACOS_DMG = ".github/workflows/macos-dmg.yml"
WIN_INSTALLER = ".github/workflows/windows-installer.yml"
DOCKERFILE = ".github/docker/Dockerfile"

# (file, regex with one capture group, pin key, minimum match count). Regexes
# are applied per line, comments stripped.
SITES: list[tuple[str, str, str, int]] = [
    # ci.yml — workflow-level env shared by check-windows / check-macos.
    (CI, r"^\s{2}QT_VERSION:\s*'([^']+)'", "QT_VERSION", 1),
    (CI, r"^\s{2}QT_PACKAGE_REVISION:\s*'([^']+)'", "QT_PACKAGE_REVISION", 1),
    (CI, r"^\s+AQTINSTALL_VERSION:\s*'([^']+)'", "AQTINSTALL_VERSION", 1),
    (CI, r"^\s{2}AQTINSTALL_GIT_REF:\s*'([^']+)'", "AQTINSTALL_GIT_REF", 1),
    (CI, r"^\s{2}PY7ZR_VERSION:\s*'([^']+)'", "PY7ZR_VERSION", 1),
    # Release workflows.
    (APPIMAGE, r"^\s+QT_VERSION:\s*'([^']+)'", "QT_VERSION", 1),
    (APPIMAGE, r"^\s+AQTINSTALL_VERSION:\s*'([^']+)'", "AQTINSTALL_VERSION", 1),
    (MACOS_DMG, r"^\s+QT_VERSION:\s*'([^']+)'", "QT_VERSION", 1),
    (MACOS_DMG, r"^\s+QT_PACKAGE_REVISION:\s*'([^']+)'", "QT_PACKAGE_REVISION", 1),
    (MACOS_DMG, r"^\s+AQTINSTALL_VERSION:\s*'([^']+)'", "AQTINSTALL_VERSION", 1),
    (WIN_INSTALLER, r"^\s+version:\s*'([^']+)'", "QT_VERSION", 1),
    (WIN_INSTALLER, r"\\Qt\\(\d+\.\d+\.\d+)\\msvc", "QT_VERSION", 1),
    (WIN_INSTALLER, r"qtkeychain-.*-qt(\d+\.\d+\.\d+)-", "QT_VERSION", 1),
    (WIN_INSTALLER, r"qtkeychain-.*-qt[\d.]+-(\d+)-", "QT_PACKAGE_REVISION", 1),
    (WIN_INSTALLER, r"aqtinstall\.git@([0-9a-f]+)", "AQTINSTALL_GIT_REF", 1),
    (WIN_INSTALLER, r"py7zrversion:\s*'==([^']+)'", "PY7ZR_VERSION", 1),
    # The Linux CI image.
    (DOCKERFILE, r"^ENV QT_VERSION=(\S+)", "QT_VERSION", 1),
    (DOCKERFILE, r"^ENV QT_ROOT=/opt/Qt/([^/]+)/", "QT_VERSION", 1),
    (DOCKERFILE, r"^ENV QT_PACKAGE_REVISION=(\S+)", "QT_PACKAGE_REVISION", 1),
    (DOCKERFILE, r"^ENV AQTINSTALL_VERSION=(\S+)", "AQTINSTALL_VERSION", 1),
]

# Files whose `-m <modules>` aqt lists must equal QT_MODULES, with the minimum
# number of aqt invocations each carries.
MODULE_SITES = {CI: 2, APPIMAGE: 1, MACOS_DMG: 1, DOCKERFILE: 1}
MODULES_RE = re.compile(r"-m\s+((?:qt\w+\s*)+)")
# install-qt-action's `modules:` also lists debug-symbol modules; those are
# packaging, not the Qt API surface, so only the plain module names must match.
WIN_MODULES_RE = re.compile(r"^\s+modules:\s*'([^']+)'")

# CMakeLists.txt enforces the floor as MAJOR.MINOR.
CMAKE_FLOOR_RES = [
    re.compile(r"find_package\(Qt6\s+(\d+\.\d+)\s+REQUIRED"),
    re.compile(r"Qt6_VERSION\s+VERSION_LESS\s+(\d+\.\d+)\s*\)"),
]

UNKNOWN_SCAN = [CI, APPIMAGE, MACOS_DMG, WIN_INSTALLER, DOCKERFILE,
                ".github/docker/Dockerfile.tsan"]
QT_LITERAL_RE = re.compile(r"(?<![\w.])(6\.\d{1,2}\.\d{1,2})(?![\w.])")


def read_pin(path: Path) -> dict[str, str]:
    pin = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = re.match(r'^([A-Z0-9_]+)="?([^"]*)"?$', line.strip())
        if match:
            pin[match.group(1)] = match.group(2)
    return pin


def code_lines(path: Path):
    """(line number, text) with `#` comments removed (YAML and Dockerfile)."""
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        # Workflows embed PowerShell, whose comments are also `#`; a `#` inside a
        # quoted string would be cut too, which only ever hides text, never
        # invents a finding.
        yield number, line.split("#", 1)[0] if not line.lstrip().startswith("#") else ""


def check(repo: Path) -> tuple[list[str], int]:
    findings: list[str] = []
    checked = 0
    pin_path = repo / "cmake" / "qt-pin.env"
    if not pin_path.exists():
        return [f"{pin_path.relative_to(repo)} is missing"], 0
    pin = read_pin(pin_path)
    required = ("QT_VERSION", "QT_PACKAGE_REVISION", "QT_SOURCE_FLOOR",
                "AQTINSTALL_VERSION", "AQTINSTALL_GIT_REF", "PY7ZR_VERSION",
                "QT_MODULES")
    for key in required:
        if not pin.get(key):
            findings.append(f"cmake/qt-pin.env: {key} is missing or empty")
    if findings:
        return findings, 0

    for rel, pattern, key, minimum in SITES:
        path = repo / rel
        if not path.exists():
            findings.append(f"{rel}: missing — the pin site list is out of date")
            continue
        regex = re.compile(pattern)
        hits = 0
        for number, text in code_lines(path):
            for value in regex.findall(text):
                hits += 1
                checked += 1
                if value != pin[key]:
                    findings.append(f"{rel}:{number}: {key} is '{value}', "
                                    f"cmake/qt-pin.env says '{pin[key]}'")
        if hits < minimum:
            findings.append(f"{rel}: expected {minimum} {key} site(s) matching "
                            f"/{pattern}/, found {hits} — the file changed shape, "
                            f"so update SITES in tools/check_qt_pin.py")

    want_modules = sorted(pin["QT_MODULES"].split())
    for rel, minimum in MODULE_SITES.items():
        hits = 0
        for number, text in code_lines(repo / rel):
            for group in MODULES_RE.findall(text):
                hits += 1
                checked += 1
                got = sorted(group.split())
                if got != want_modules:
                    findings.append(f"{rel}:{number}: aqt modules {got}, "
                                    f"cmake/qt-pin.env says {want_modules}")
        if hits < minimum:
            findings.append(f"{rel}: expected {minimum} aqt `-m` module list(s), "
                            f"found {hits}")
    win_hits = 0
    for number, text in code_lines(repo / WIN_INSTALLER):
        match = WIN_MODULES_RE.match(text)
        if match:
            win_hits += 1
            checked += 1
            got = sorted(m for m in match.group(1).split()
                         if m != "debug_info" and not m.endswith(".debug_information"))
            if got != want_modules:
                findings.append(f"{WIN_INSTALLER}:{number}: modules {got}, "
                                f"cmake/qt-pin.env says {want_modules}")
    if win_hits < 1:
        findings.append(f"{WIN_INSTALLER}: no install-qt-action `modules:` line found")

    floor_mm = pin["QT_SOURCE_FLOOR"]
    pin_mm = ".".join(pin["QT_VERSION"].split(".")[:2])
    if tuple(map(int, floor_mm.split("."))) > tuple(map(int, pin_mm.split("."))):
        findings.append(f"cmake/qt-pin.env: QT_SOURCE_FLOOR {floor_mm} is above "
                        f"the pinned Qt {pin['QT_VERSION']} — no shipped build "
                        f"could satisfy it")
    cmake_text = (repo / "CMakeLists.txt").read_text(encoding="utf-8")
    for regex in CMAKE_FLOOR_RES:
        values = regex.findall(cmake_text)
        if not values:
            findings.append(f"CMakeLists.txt: no match for /{regex.pattern}/ — "
                            f"the floor check moved, update tools/check_qt_pin.py")
        for value in values:
            checked += 1
            if value != floor_mm:
                findings.append(f"CMakeLists.txt: Qt floor is {value}, "
                                f"cmake/qt-pin.env QT_SOURCE_FLOOR is {floor_mm}")

    allowed = {pin["QT_VERSION"]}
    for rel in UNKNOWN_SCAN:
        path = repo / rel
        if not path.exists():
            continue
        for number, text in code_lines(path):
            for value in QT_LITERAL_RE.findall(text):
                if value not in allowed:
                    findings.append(f"{rel}:{number}: Qt version literal {value} "
                                    f"is not the pin ({pin['QT_VERSION']})")
    return findings, checked


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Fail when a workflow or the CI image disagrees with "
                    "cmake/qt-pin.env")
    parser.add_argument("--strict", action="store_true",
                        help="exit 1 when any pin site drifts")
    args = parser.parse_args()

    findings, checked = check(REPO)
    print(f"Checked {checked} pin site(s) against cmake/qt-pin.env.")
    if not findings:
        print("OK: every workflow and the CI image agree with the Qt pin.")
        return 0
    print(f"\n{len(findings)} Qt pin mismatch(es):\n")
    for finding in findings:
        print(f"  {finding}")
    print("\nThe pin is cmake/qt-pin.env. Change it there, then make every site "
          "above agree — a release built from a workflow that disagrees ships a "
          "Qt nothing else tested.")
    return 1 if args.strict else 0


if __name__ == "__main__":
    sys.exit(main())
