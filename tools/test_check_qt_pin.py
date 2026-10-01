#!/usr/bin/env python3
"""
Self-test for tools/check_qt_pin.py.

The checker is only worth running if it fails on drift, so every case below
copies the real files into a scratch tree, breaks exactly one thing, and
requires a finding that names it. The unmodified tree must pass, and a comment
mentioning another Qt version must not be a finding.

Usage:
    python tools/test_check_qt_pin.py
"""

from __future__ import annotations

import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_qt_pin  # noqa: E402

REPO = check_qt_pin.REPO
FILES = [
    "cmake/qt-pin.env",
    "CMakeLists.txt",
    check_qt_pin.CI,
    check_qt_pin.APPIMAGE,
    check_qt_pin.MACOS_DMG,
    check_qt_pin.WIN_INSTALLER,
    check_qt_pin.DOCKERFILE,
    ".github/docker/Dockerfile.tsan",
]

PIN = check_qt_pin.read_pin(REPO / "cmake" / "qt-pin.env")
V, REV, FLOOR = PIN["QT_VERSION"], PIN["QT_PACKAGE_REVISION"], PIN["QT_FLOOR_VERSION"]

# (name, file, old text, new text, substring the finding must contain)
CASES = [
    ("ci.yml pin", check_qt_pin.CI,
     f"  QT_VERSION: '{V}'", "  QT_VERSION: '6.99.0'", "QT_VERSION is '6.99.0'"),
    ("ci.yml revision", check_qt_pin.CI,
     f"  QT_PACKAGE_REVISION: '{REV}'", "  QT_PACKAGE_REVISION: '1'",
     "QT_PACKAGE_REVISION is '1'"),
    ("floor-lane version", check_qt_pin.CI,
     f"QT_FLOOR_VERSION: '{FLOOR}'", "QT_FLOOR_VERSION: '6.8.4'",
     "QT_FLOOR_VERSION is '6.8.4'"),
    ("windows py7zr", check_qt_pin.WIN_INSTALLER,
     f"py7zrversion: '=={PIN['PY7ZR_VERSION']}'", "py7zrversion: '==9.9.9'",
     "PY7ZR_VERSION is '9.9.9'"),
    ("windows aqt ref", check_qt_pin.WIN_INSTALLER,
     PIN["AQTINSTALL_GIT_REF"], "deadbeef", "AQTINSTALL_GIT_REF is 'deadbeef'"),
    ("windows keychain revision", check_qt_pin.WIN_INSTALLER,
     f"qt{V}-{REV}-", f"qt{V}-202001010000-", "QT_PACKAGE_REVISION is '202001010000'"),
    ("Dockerfile revision", check_qt_pin.DOCKERFILE,
     f"ENV QT_PACKAGE_REVISION={REV}", "ENV QT_PACKAGE_REVISION=2",
     "QT_PACKAGE_REVISION is '2'"),
    ("Dockerfile aqt", check_qt_pin.DOCKERFILE,
     f"ENV AQTINSTALL_VERSION={PIN['AQTINSTALL_VERSION']}",
     "ENV AQTINSTALL_VERSION=9.9", "AQTINSTALL_VERSION is '9.9'"),
    ("appimage module dropped", check_qt_pin.APPIMAGE,
     "qtserialport qtshadertools", "qtserialport", "aqt modules"),
    ("windows module dropped", check_qt_pin.WIN_INSTALLER,
     "modules: 'qtmultimedia qtserialport", "modules: 'qtmultimedia", "modules ["),
    ("CMake floor", "CMakeLists.txt",
     "find_package(Qt6 6.8 REQUIRED", "find_package(Qt6 6.9 REQUIRED",
     "Qt floor is 6.9"),
    ("unlisted literal", check_qt_pin.APPIMAGE,
     "runs-on: ${{ matrix.runner }}",
     "runs-on: ${{ matrix.runner }}  # ok\n    timeout-minutes: 60 # fine\n"
     "    env2: { X: 6.11.2 }", "literal 6.11.2"),
    ("site disappears", check_qt_pin.MACOS_DMG,
     f"      QT_PACKAGE_REVISION: '{REV}'", "      QT_PKG_REV: 'x'",
     "expected 1 QT_PACKAGE_REVISION site"),
]


def scratch_tree() -> Path:
    root = Path(tempfile.mkdtemp(prefix="qtpin-"))
    for rel in FILES:
        dest = root / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(REPO / rel, dest)
    return root


def main() -> int:
    failures = 0

    root = scratch_tree()
    findings, checked = check_qt_pin.check(root)
    if findings or checked < 25:
        print(f"FAIL clean tree: {checked} site(s), findings: {findings}")
        failures += 1
    else:
        print(f"ok   clean tree passes ({checked} sites)")

    # A comment naming another Qt (history, rationale) is not drift.
    ci = root / check_qt_pin.CI
    ci.write_text(ci.read_text(encoding="utf-8") + "\n# Homebrew shipped 6.11.1\n",
                  encoding="utf-8")
    findings, _ = check_qt_pin.check(root)
    if findings:
        print(f"FAIL comment-only version was reported: {findings}")
        failures += 1
    else:
        print("ok   comment mentioning 6.11.1 is ignored")
    shutil.rmtree(root)

    for name, rel, old, new, expect in CASES:
        root = scratch_tree()
        path = root / rel
        text = path.read_text(encoding="utf-8")
        if old not in text:
            print(f"FAIL {name}: fixture text not found in {rel}: {old!r}")
            failures += 1
            shutil.rmtree(root)
            continue
        path.write_text(text.replace(old, new, 1), encoding="utf-8")
        findings, _ = check_qt_pin.check(root)
        if any(expect in f for f in findings):
            print(f"ok   {name}")
        else:
            print(f"FAIL {name}: no finding containing {expect!r}; got {findings}")
            failures += 1
        shutil.rmtree(root)

    print(f"\n{len(CASES) + 2 - failures}/{len(CASES) + 2} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
