#!/usr/bin/env python3
"""Raw Flex command-plane ratchet above the seam — #5262 M4.

WHY THIS EXISTS. M4 converts the dual command plane: models still emit Flex wire
text that is SILENTLY DROPPED on HL2/Icom/ANAN/RTL (RadioModel::sendCmd()'s
no-command-plane path), so every unconverted control is a live-looking dead
control on those radios. M0 made that drop loud (#5265: qCWarning + a one-shot
operator notice), which turned the backlog visible — a live IC-7300MK2 connect
dropped 14 SmartSDR commands, each one a conversion site.

M4's first checklist item is this ratchet, and it comes first for a reason: the
conversion is long (hundreds of call sites, by subsystem), and without a floor
under it new controls keep arriving on the old plane faster than old ones leave.

WHAT COUNTS. Two shapes, both of which put raw Flex wire text above the seam:

  * `emit commandReady(...)` in src/models/ — a model handing RadioModel a wire
    string to forward.
  * `sendCommand(...)` in src/gui/ — a widget or dialog building wire text
    directly.

END STATE: both reach zero, the commandReady plane is deleted, and
usesFlexCommandPlane() dissolves. Until then this file's numbers only fall.

WHAT DOES NOT COUNT. Anything inside src/core/backends/flex/ — that is where the
wire text is SUPPOSED to live, and M4's whole direction is moving encode there
(symmetric with the 2.3 decode split). Growth under backends/flex/ is progress.

PER-FILE, NOT A TOTAL. A total lets one subsystem's conversion pay for another's
regression; per-file means a converted file cannot quietly refill. A file absent
from the baseline must stay at zero, so a NEW control on the old plane fails
even while the old ones are still being migrated — which is the point.

Usage:
    python tools/check_command_plane.py            # report
    python tools/check_command_plane.py --strict   # exit 1 on growth
"""

import argparse
import re
import sys
from pathlib import Path

# The surface at the freeze (#5262 M4, 2026-09-12). SHRINK ONLY.
#
# Convert by subsystem with the claim protocol + verify_slice0_rx.py recipe, and
# drop the number here in the same commit. When a file reaches 0, delete its row.
BASELINE = {
    # ---- models: emit commandReady(...) ----
    "src/models/TransmitModel.cpp": 39,
    "src/models/CwxModel.cpp": 11,
    "src/models/DaxIqModel.cpp": 4,
    "src/models/EqualizerModel.cpp": 4,
    "src/models/FlexWaveformModel.cpp": 3,
    "src/models/UsbCableModel.cpp": 3,
    "src/models/SliceModel.cpp": 2,
    # ---- gui: sendCommand(...) ----
    "src/gui/RadioSetupDialog.cpp": 30,
    "src/gui/MainWindow_Wiring.cpp": 24,
    "src/gui/MainWindow.cpp": 22,
    "src/gui/ProfileManagerDialog.cpp": 12,
    "src/gui/MainWindow_Controllers.cpp": 4,
    "src/gui/TxBandDialog.cpp": 4,
    "src/gui/DxClusterDialog.cpp": 3,
    "src/gui/MainWindow_Nets.cpp": 3,
    "src/gui/MainWindow_Shortcuts.cpp": 3,
    "src/gui/MainWindow_DigitalModes.cpp": 2,
    "src/gui/MainWindow_Spots.cpp": 1,
    "src/gui/PskReporterMapDialog.cpp": 1,
    "src/gui/SpectrumOverlayMenu.cpp": 1,
    "src/gui/SpotSettingsDialog.cpp": 1,
}

MODEL_PATTERN = re.compile(r"emit\s+commandReady\s*\(")
GUI_PATTERN = re.compile(r"\bsendCommand\s*\(")


def count_for(path: Path) -> int:
    text = path.read_text(encoding="utf-8", errors="replace")
    rel = path.as_posix()
    pattern = MODEL_PATTERN if rel.startswith("src/models/") else GUI_PATTERN
    return len(pattern.findall(text))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true",
                    help="exit 1 when any file has grown, or a new file appears")
    args = ap.parse_args()

    roots = [Path("src/models"), Path("src/gui")]
    current: dict[str, int] = {}
    for root in roots:
        if not root.exists():
            continue
        for path in sorted(root.rglob("*.cpp")):
            n = count_for(path)
            if n:
                current[path.as_posix()] = n

    blocking = 0
    total = 0
    for rel, n in sorted(current.items()):
        total += n
        allowed = BASELINE.get(rel)
        if allowed is None:
            blocking += 1
            print(f"::error file={rel},title=command-plane-ratchet::"
                  f"{rel} puts {n} raw Flex command(s) above the seam and is not in the "
                  f"baseline. A new control must emit a TYPED INTENT, not wire text — on "
                  f"HL2/Icom/ANAN/RTL this text is silently dropped and the control looks "
                  f"live while doing nothing (#5262 M4).")
        elif n > allowed:
            blocking += 1
            print(f"::error file={rel},title=command-plane-ratchet::"
                  f"{rel} grew from {allowed} to {n} raw Flex command(s) above the seam. "
                  f"This baseline may only shrink (#5262 M4).")
        elif n < allowed:
            print(f"::notice file={rel},title=command-plane-progress::"
                  f"{rel} is down to {n} from {allowed} — lower its row in "
                  f"tools/check_command_plane.py so the gain cannot be given back.")

    for rel, allowed in sorted(BASELINE.items()):
        if rel not in current:
            print(f"::notice title=command-plane-progress::"
                  f"{rel} is clear of raw Flex commands (was {allowed}) — delete its row "
                  f"from tools/check_command_plane.py.")

    print(f"command-plane: {len(current)} file(s), {total} raw Flex command(s) above "
          f"the seam; baseline {sum(BASELINE.values())} across {len(BASELINE)} file(s); "
          f"{blocking} would block under --strict")
    return 1 if (args.strict and blocking) else 0


if __name__ == "__main__":
    sys.exit(main())
