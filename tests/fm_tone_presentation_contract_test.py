#!/usr/bin/env python3
"""Pin radio-backed CTCSS wiring on the two existing GUI surfaces.

The applets are part of the full desktop target and have no practical unit-test
link seam. Label behavior is covered by fm_tone_presentation_test; this narrow
source contract only guards the remaining widget wiring.

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


def check_surface(path: Path, ctcss_members: tuple[str, str]) -> None:
    source = path.read_text(encoding="utf-8")
    if "const QString selected = m_slice" not in source:
        fail(f"{path.name} no longer derives the displayed mode from radio-backed slice state")
    if "index < 0 && presentation != FmTonePresentation::Ctcss" not in source:
        fail(f"{path.name} fabricates Off for an unoffered IC-9700 access state")

    for role in ("Tx", "Rx"):
        if f"FmToneRole::{role}" not in source:
            fail(f"{path.name} no longer formats the {role} tone control")
    if "&SliceModel::fmDtcsChanged" not in source:
        fail(f"{path.name} no longer follows radio-authoritative DTCS readback")
    if "setFmDtcs(" not in source:
        fail(f"{path.name} no longer publishes DTCS operator intent")
    if "caps.fmDtcsCodes" not in source:
        fail(f"{path.name} bypasses the model-specific DTCS capability vocabulary")
    if "fmToneUsesDtcsTx(mode)" not in source:
        fail(f"{path.name} no longer places DTCS controls according to their TX/RX role")
    for member in ctcss_members:
        population = re.compile(rf"populateCtcssToneCombo\(\s*{member}\s*\)")
        labels = re.compile(
            rf"configureCtcssToneComboLabels\(\s*{member}\s*,\s*"
            r"presentation\s*,\s*FmToneRole::(?:Tx|Rx)\s*\)"
        )
        style = re.compile(
            rf"applyComboStyle\(\s*{member}\s*,\s*"
            r"AetherSDR::ctcssToneComboStyleRules\(\)\s*\)"
        )
        if not population.search(source):
            fail(f"{path.name} no longer populates {member} through the shared CTCSS path")
        if not labels.search(source):
            fail(f"{path.name} no longer configures {member} through the shared label path")
        if not style.search(source):
            fail(f"{path.name} no longer applies shared popup styling to {member}")


def main() -> int:
    if len(sys.argv) != 2:
        fail("usage: fm_tone_presentation_contract_test.py <source-root>")
    source_root = Path(sys.argv[1])
    rx_path = source_root / "src/gui/RxApplet.cpp"
    check_surface(rx_path, ("m_toneValueCmb", "m_toneRxValueCmb"))
    check_surface(
        source_root / "src/gui/VfoWidget.cpp",
        ("m_fmToneValueCmb", "m_fmToneRxValueCmb"),
    )
    rx_source = rx_path.read_text(encoding="utf-8")
    radio_model_source = (source_root / "src/models/RadioModel.cpp").read_text(
        encoding="utf-8"
    )
    if "&SliceModel::fmDtcsCommandIssued" not in radio_model_source:
        fail("RadioModel no longer forwards normalized DTCS operator intent")
    if "m_backend->setSliceFmDtcs(" not in radio_model_source:
        fail("RadioModel no longer sends DTCS intent through IRadioBackend")
    mode_visibility_edge = re.compile(
        r"m_fmContainer->setVisible\(isFM\);\s*"
        r"(?:\s*//[^\n]*\n)*\s*configureFmToneControls\(\);"
    )
    if not mode_visibility_edge.search(rx_source):
        fail("RxApplet no longer re-evaluates tone children when FM visibility changes")
    vfo_source = (source_root / "src/gui/VfoWidget.cpp").read_text(encoding="utf-8")
    for container in ("m_fmToneRxContainer", "m_fmDtcsContainer"):
        visibility = re.compile(
            rf"{container}->setVisible\(\s*modeEligible\s*&&"
        )
        if not visibility.search(vfo_source):
            fail(f"VfoWidget {container} can remain visible outside an FM mode")
    print("fm-tone presentation contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
