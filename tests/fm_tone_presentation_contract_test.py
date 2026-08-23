#!/usr/bin/env python3
"""Pin model-gated TX/RX CTCSS labels on the two existing GUI surfaces.

The applets are part of the full desktop target and have no practical unit-test
link seam.  This deliberately narrow source contract prevents the regression
where adding an IC-9700 label prefix changed Flex and every legacy backend.

This is not a behavioral widget test and does not claim general visibility
coverage.  It additionally pins the specific RxApplet mode-change edge whose
absence left explicitly hidden tone children hidden when returning to FM.
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
    rx_path = source_root / "src/gui/RxApplet.cpp"
    check_surface(rx_path, "toneLabel")
    check_surface(source_root / "src/gui/VfoWidget.cpp", "frequency")
    rx_source = rx_path.read_text(encoding="utf-8")
    mode_visibility_edge = re.compile(
        r"m_fmContainer->setVisible\(isFM\);\s*"
        r"(?:\s*//[^\n]*\n)*\s*configureFmToneControls\(\);"
    )
    if not mode_visibility_edge.search(rx_source):
        fail("RxApplet no longer re-evaluates tone children when FM visibility changes")
    print("fm-tone presentation contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
