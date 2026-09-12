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

WHAT COUNTS, AND WHY RESOLVING THE RECEIVER IS THE WHOLE PROBLEM.

The obvious matcher — `emit commandReady(` in models, `sendCommand(` in gui —
is wrong in both directions, and the first version of this checker shipped with
exactly that bug (#5619 review):

  * IT UNDER-COUNTS. SliceModel::sendCommand() is a ONE-LINE HELPER whose body
    is `emit commandReady(cmd);`, and ~62 call sites behind it carry raw wire
    text (`slice tune`, `filt %1 %2 %3`, `slice set %1 rxant=`). Counting only
    the literal emit saw 2 — freezing the milestone's LARGEST conversion target
    (#5262 names SliceModel at 60 sites) at a number a new control could grow
    under without tripping anything. A ratchet that reads green while the
    biggest target grows is worse than no ratchet, because the next change
    cites it as coverage.

  * IT OVER-COUNTS. `sendCommand` is not one protocol. AntennaGeniusModel has
    its own method of that name that writes to ITS OWN TCP SOCKET
    (AntennaGeniusModel.cpp:411), and TunerModel reaches a separate device
    through `m_directConn->sendCommand(...)`. Neither is the Flex command plane
    and neither should be frozen here.

So the receiver is resolved explicitly rather than guessed from the directory:

  1. `emit commandReady(` — the plane's own signal. Always counts.
  2. `sendCmd(` — RadioModel's internal sink name. Always counts.
  3. `sendCommand(` — counts UNLESS the receiver is a known foreign device,
     which the two exclusions below name. Helper DEFINITIONS are not call
     sites and are subtracted.

WHERE IT LOOKS. The same above-seam definition check_engine_boundary.py uses
(src/gui + src/core + src/models, plus the src/ root shell files), minus
src/core/backends/ — that is where the wire text is SUPPOSED to live, and M4's
direction is moving encode there. The first version scanned only models and gui,
which missed WfmDemodulator.cpp's two live emissions in src/core/ and made the
"backends/flex is excluded" claim describe an exclusion that did not exist,
since nothing under src/core/ was read at all.

PER-FILE, NOT A TOTAL. A total lets one subsystem's conversion pay for another's
regression; per-file means a converted file cannot quietly refill. A file absent
from the baseline must stay at zero, so a NEW control on the old plane fails
even while the old ones are still being migrated — which is the point.

ANTI-VACUITY. A missing root is an ERROR, not an empty scan. The first version
returned "every file is clear" and exit 0 when run from the wrong directory;
check_engine_boundary.py carries ABOVE_SEAM_DIR_FLOOR for exactly this failure
mode and this now does too.

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
    # ---- models ----
    "src/models/RadioModel.cpp": 139,
    "src/models/SliceModel.cpp": 63,
    "src/models/TransmitModel.cpp": 39,
    "src/models/CwxModel.cpp": 11,
    "src/models/DaxIqModel.cpp": 4,
    "src/models/EqualizerModel.cpp": 4,
    "src/models/FlexWaveformModel.cpp": 3,
    "src/models/UsbCableModel.cpp": 3,
    "src/models/RadioModel.h": 1,
    "src/models/SliceModel.h": 1,
    # ---- gui ----
    "src/gui/RadioSetupDialog.cpp": 30,
    "src/gui/MainWindow_Wiring.cpp": 22,
    "src/gui/MainWindow.cpp": 20,
    "src/gui/ProfileManagerDialog.cpp": 11,
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
    # ---- core (above the seam; backends/ is excluded) ----
    "src/core/TciServer.cpp": 8,
    "src/core/VkampConnection.cpp": 8,
    "src/core/AutomationServer.cpp": 5,
    "src/core/TgxlConnection.cpp": 5,
    "src/core/PgxlConnection.cpp": 3,
    "src/core/WfmDemodulator.cpp": 2,
    "src/core/DxClusterClient.cpp": 1,
    "src/core/DxClusterClient.h": 1,
    "src/core/FirmwareUploader.cpp": 1,
    "src/core/PgxlConnection.h": 1,
    "src/core/RigctlProtocol.cpp": 1,
    "src/core/TgxlConnection.h": 1,
    "src/core/VkampConnection.h": 1,
    "src/core/WanConnection.cpp": 1,
    "src/core/WanConnection.h": 1,
}

REPO = Path(__file__).resolve().parent.parent

# The settled above-seam definition (check_engine_boundary.py:65). Kept in the
# same shape deliberately: two different answers to "what is above the seam"
# would be a bug generator.
ABOVE_SEAM_DIRS = [REPO / "src" / "gui", REPO / "src" / "core", REPO / "src" / "models"]
ABOVE_SEAM_FILES = [REPO / "src" / "main.cpp"]
BACKENDS_PREFIX = "src/core/backends/"

# Per-directory vacuity floor, EB3's guard applied here (check_engine_boundary
# .py:210). If a root is renamed or the script runs from the wrong cwd, its
# files vanish, every row degrades to a cheerful "clear!" notice, and the
# ratchet disarms while CI stays green. Observed on the first version.
ABOVE_SEAM_DIR_FLOOR = 20

# Always a Flex command-plane site.
ALWAYS_RE = re.compile(r"emit\s+commandReady\s*\(|\bsendCmd\s*\(")
# A sendCommand call, qualified or not.
SEND_COMMAND_RE = re.compile(r"\bsendCommand\s*\(")
# DEFINITIONS and declarations — not call sites.
SEND_COMMAND_DEF_RE = re.compile(r"\b\w+::sendCommand\s*\(")
SEND_CMD_DEF_RE = re.compile(r"\b\w+::sendCmd\s*\(|^\s*\w[\w:<>,\s&*]*\bsendCmd\s*\([^)]*\)\s*;\s*$",
                             re.M)

# Files whose own `sendCommand` is a DIFFERENT DEVICE'S protocol. Named per file
# rather than pattern-matched, because the distinction is semantic: it is about
# which wire the string ends up on, which no regex can see.
FOREIGN_SEND_COMMAND_FILES = {
    # Writes to its own QTcpSocket with a "C<seq>|<cmd>" framing (:411).
    "src/models/AntennaGeniusModel.cpp",
    "src/models/AntennaGeniusModel.h",
}
# Receivers that are a different device even in an otherwise-counted file.
FOREIGN_RECEIVER_RE = re.compile(r"m_directConn\s*->\s*sendCommand\s*\(")


def count_for(path: Path) -> int:
    """Flex command-plane sites in one above-seam file, receiver resolved."""
    text = path.read_text(encoding="utf-8", errors="replace")
    # Strip // comments and string-free enough for counting: a comment that says
    # "emit commandReady" is not a call site. grep counted one in SliceModel.cpp.
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)

    rel = path.relative_to(REPO).as_posix()
    n = len(ALWAYS_RE.findall(text))
    n -= len(SEND_CMD_DEF_RE.findall(text))
    if rel not in FOREIGN_SEND_COMMAND_FILES:
        n += len(SEND_COMMAND_RE.findall(text))
        n -= len(SEND_COMMAND_DEF_RE.findall(text))
        n -= len(FOREIGN_RECEIVER_RE.findall(text))
    return max(n, 0)


def scan() -> dict[str, int]:
    current: dict[str, int] = {}
    for root in ABOVE_SEAM_DIRS:
        if not root.exists():
            raise SystemExit(f"check_command_plane: above-seam root missing: {root}")
        seen = 0
        for path in sorted(list(root.rglob("*.cpp")) + list(root.rglob("*.h"))):
            rel = path.relative_to(REPO).as_posix()
            if rel.startswith(BACKENDS_PREFIX):
                continue
            seen += 1
            n = count_for(path)
            if n:
                current[rel] = n
        if seen < ABOVE_SEAM_DIR_FLOOR:
            raise SystemExit(
                f"check_command_plane: {root} yielded only {seen} file(s), below the "
                f"{ABOVE_SEAM_DIR_FLOOR} floor — the scan is vacuous and the ratchet "
                f"would disarm silently. Run from the repo root, or update "
                f"ABOVE_SEAM_DIRS if the tree moved.")
    for path in ABOVE_SEAM_FILES:
        if path.exists():
            n = count_for(path)
            if n:
                current[path.relative_to(REPO).as_posix()] = n
    return current


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true",
                    help="exit 1 when any file has grown, or a new file appears")
    args = ap.parse_args()

    current = scan()

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
