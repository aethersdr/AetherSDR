#!/usr/bin/env python3
"""Pin model-gated TX/RX CTCSS labels on the two existing GUI surfaces.

The applets are part of the full desktop target and have no practical unit-test
link seam.  This deliberately narrow source contract prevents the regression
where adding an IC-9700 label prefix changed Flex and every legacy backend.
"""

from pathlib import Path
import re
import sys


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def check_surface(path: Path, legacy_label: str) -> None:
    source = path.read_text(encoding="utf-8")
    gate = "distinguishTxRx = presentation == FmTonePresentation::Ctcss"
    if gate not in source:
        fail(f"{path.name} no longer derives TX/RX labels from the CTCSS capability")
    if "const QString selected = m_slice" not in source:
        fail(f"{path.name} no longer derives the displayed mode from radio-backed slice state")
    if "index < 0 && presentation != FmTonePresentation::Ctcss" not in source:
        fail(f"{path.name} fabricates Off for an unoffered IC-9700 access state")

    for role in ("TX", "RX"):
        literal = f'QStringLiteral("{role}: %1")'
        if source.count(literal) != 1:
            fail(f"{path.name} must contain exactly one capability-gated {role} prefix")
        pattern = re.compile(
            rf"distinguishTxRx\s*\?\s*{re.escape(literal)}\.arg\([^)]*\)"
            rf"\s*:\s*{legacy_label}"
        )
        if not pattern.search(source):
            fail(f"{path.name} applies {role}: without preserving the legacy label")


def main() -> int:
    if len(sys.argv) != 2:
        fail("usage: fm_tone_presentation_contract_test.py <source-root>")
    source_root = Path(sys.argv[1])
    check_surface(source_root / "src/gui/RxApplet.cpp", "toneLabel")
    check_surface(source_root / "src/gui/VfoWidget.cpp", "frequency")
    print("fm-tone presentation contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
