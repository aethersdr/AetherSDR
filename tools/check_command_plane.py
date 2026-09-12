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
    "src/models/RadioModel.cpp": 137,
    "src/models/SliceModel.cpp": 63,
    "src/models/TransmitModel.cpp": 39,
    "src/models/CwxModel.cpp": 11,
    "src/models/DaxIqModel.cpp": 4,
    "src/models/EqualizerModel.cpp": 4,
    "src/models/FlexWaveformModel.cpp": 3,
    "src/models/UsbCableModel.cpp": 3,
    # ---- gui ----
    "src/gui/RadioSetupDialog.cpp": 41,
    "src/gui/MainWindow.cpp": 21,
    "src/gui/MainWindow_Wiring.cpp": 20,
    "src/gui/ProfileManagerDialog.cpp": 12,
    "src/gui/MemoryDialog.cpp": 5,
    "src/gui/MainWindow_Controllers.cpp": 4,
    "src/gui/MainWindow_Shortcuts.cpp": 4,
    "src/gui/TxBandDialog.cpp": 4,
    "src/gui/MainWindow_Nets.cpp": 3,
    "src/gui/MemoryCommands.cpp": 3,
    "src/gui/MainWindow_DigitalModes.cpp": 2,
    "src/gui/DxClusterDialog.cpp": 1,
    "src/gui/MainWindow_Spots.cpp": 1,
    "src/gui/PskReporterMapDialog.cpp": 1,
    "src/gui/SpectrumOverlayMenu.cpp": 1,
    "src/gui/SpotSettingsDialog.cpp": 1,
    # ---- core (above the seam; backends/ is excluded) ----
    "src/core/TciServer.cpp": 11,
    "src/core/AutomationServer.cpp": 6,
    "src/core/WaveformInstaller.cpp": 3,
    "src/core/DvkWavTransfer.cpp": 2,
    "src/core/FirmwareUploader.cpp": 2,
    "src/core/RigctlProtocol.cpp": 2,
    "src/core/WfmDemodulator.cpp": 2,
    "src/core/TciProtocol.cpp": 1,
}

REPO = Path(__file__).resolve().parent.parent

# The settled above-seam definition (check_engine_boundary.py:65). Kept in the
# same shape deliberately: two different answers to "what is above the seam"
# would be a bug generator.
ABOVE_SEAM_DIRS = [REPO / "src" / "gui", REPO / "src" / "core", REPO / "src" / "models"]
# The same three root shell files the sibling lists — not just main.cpp. The
# docstring's reason for copying this definition was that two answers to "what
# is above the seam" is a bug generator; a THIRD answer is no better (#5619
# review, K5PTB). Neither MacStartupAbortGuard file carries a command today, so
# this closes a latent divergence rather than a live gap.
ABOVE_SEAM_FILES = [
    REPO / "src" / "main.cpp",
    REPO / "src" / "MacStartupAbortGuard.h",
    REPO / "src" / "MacStartupAbortGuard.cpp",
]
BACKENDS_PREFIX = "src/core/backends/"
# Same suffix set as check_engine_boundary.py's ENGINE_SUFFIXES, for the same
# reason: .mm (MacMicPermission.mm) and .hpp/.cc must not be blind spots. Only
# the .mm file exists above the seam today and it is clean.
SCANNED_SUFFIXES = (".h", ".hpp", ".cpp", ".cc", ".mm")

# Per-directory vacuity floor, EB3's guard applied here (check_engine_boundary
# .py:210). If a root is renamed or the script runs from the wrong cwd, its
# files vanish, every row degrades to a cheerful "clear!" notice, and the
# ratchet disarms while CI stays green. Observed on the first version.
ABOVE_SEAM_DIR_FLOOR = 20

# ---- receiver classification -------------------------------------------------
#
# ALLOW-LIST, NOT DENY-LIST, and the direction is the point (#5619 review, Ozy).
# `sendCommand` is a method name several unrelated device protocols share. A
# deny-list gets the default wrong: the first version froze VkampConnection's
# VK-amplifier poll and DxClusterDialog's telnet `show/dx` as if they were Flex
# wire text, because they were not on it. Allow-listing the RadioModel receivers
# inverts that — a NEW foreign protocol is ignored by default, which is correct,
# and the residual risk becomes under-counting a Flex call made through an
# unfamiliar receiver name. That risk is bounded by the census below, which was
# taken over the whole above-seam tree rather than guessed.
#
# Receivers that ARE the Flex command plane (census of every qualified call):
#     m_radioModel  61x sendCommand + 3x sendCmdPublic
#     m_model       51x sendCommand + 12x sendCmdPublic
#     model         10x sendCmdPublic + 1x sendCommand
#     modelGuard     4x sendCommand
#     this           1x sendCmd
# Receivers that are OTHER DEVICES, each its own wire:
#     client / m_dxCluster (DX cluster + RBN telnet), m_wanConn (SmartLink WAN),
#     m_pgxlConn (PGXL amp), m_directConn (tuner), connection
# Files whose UNQUALIFIED send* calls route to the Flex plane. Everything else
# unqualified belongs to the object that defines it — AntennaGeniusModel writes
# its own QTcpSocket, VkampConnection/TgxlConnection/PgxlConnection/WanConnection
# each speak their own protocol.
#
# RadioModel.cpp is the sink itself; SliceModel.cpp reaches it through a one-line
# helper whose body is `emit commandReady(cmd)`.
PLANE_HELPER_FILES = {
    "src/models/RadioModel.cpp",
    "src/models/SliceModel.cpp",
}

RADIO_MODEL_RECEIVERS = ("m_radioModel", "m_model", "model", "modelGuard", "this")

# ACCESSOR-CALL receivers: `radioModel().sendCommand(...)`. Real API on two
# classes — MainWindow::radioModel() and RadioSession::radioModel() — so any
# holder of a MainWindow* or RadioSession* reaches the plane without naming a
# member. Zero incidence in the tree today, which is why this is head-room
# rather than an under-count; it is closed anyway because a receiver that is
# neither counted nor reported defeats the fail-closed invariant the allow-list
# rests on (#5619 re-review, ten9876).
RADIO_MODEL_ACCESSORS = ("radioModel", "model", "radio")

# Receivers that are a DIFFERENT DEVICE. Named so that an unknown one can be an
# error rather than a guess — see UNKNOWN_RECEIVER below.
FOREIGN_RECEIVERS = (
    "client", "m_client", "m_rbnClient", "m_dxCluster",   # DX cluster + RBN telnet
    # WanConnection speaks the SAME V/H/R/S/M protocol as RadioConnection, over
    # TLS — it is the Flex plane, not a foreign device. It is listed here anyway
    # because RadioModel's calls THROUGH it are transport dispatch of a command
    # some other site already produced; counting them would double-count. Its own
    # internal sendCommand("ping") is SmartLink keepalive on its own socket.
    # Right answer, different reason than "foreign" suggests.
    "m_wanConn",
    "m_pgxlConn", "m_tgxlConn",                           # PGXL / TGXL amp + tuner
    "m_directConn",                                       # tuner direct connection
    "connection",
)

# GENERIC PASSTHROUGH HELPERS: methods that wrap `emit commandReady` AND take the
# wire key or the whole command FROM THE CALLER. A call to one carries raw wire
# text at the call site, so it is a conversion site (#5619 review,
# aethersdr-agent: a new `usb_cable` control slipped through because only
# send(Command|Cmd|CmdPublic) was resolved).
#
# WHY THIS IS HAND-LISTED RATHER THAN DERIVED, which was the review's preferred
# fix. Deriving "every method whose body emits commandReady" finds 64 of them —
# and most are TYPED SETTERS: TransmitModel::setRfPower, setCwSpeed, setVoxEnable
# and ~40 more. Those assemble their own wire text from a typed argument, and a
# call to one is precisely the typed intent M4 is migrating TOWARD. Counting them
# would freeze the destination as if it were the thing being left behind — six
# sampled setters alone are 26 gui call sites.
#
# The property that actually matters is "does the CALLER name the wire key?",
# which no regex can decide:
#     sendCommand(QString cmd)          caller supplies the whole command  -> count
#     sendSet(serial, key, value)       caller supplies the key            -> count
#     setRfPower(int watts)             key is internal                    -> do not
# So the list is curated against that criterion, and the criterion is written
# here so the next addition is a judgement someone can check rather than repeat.
WIRE_TEXT_HELPERS = ("sendSet", "sendSetBit", "sendRemove")

# The plane's own signal. Always a site, wherever it appears above the seam.
EMIT_RE = re.compile(r"(?:emit|Q_EMIT)\s+commandReady\s*\(")
# A qualified call on one of the RadioModel receivers. sendCmdPublic is included
# explicitly: it forwards straight to sendCmd(), and an earlier matcher of
# `\bsendCmd\s*\(` did not match it, hiding ~26 call sites.
QUALIFIED_RE = re.compile(
    r"\b(?:" + "|".join(RADIO_MODEL_RECEIVERS) + r")\s*(?:->|\.)\s*"
    r"send(?:Command|Cmd|CmdPublic)\s*\(")
ACCESSOR_RE = re.compile(
    r"\b(?:" + "|".join(RADIO_MODEL_ACCESSORS) + r")\s*\(\s*\)\s*(?:->|\.)\s*"
    r"send(?:Command|Cmd|CmdPublic)\s*\(")
# A call to a generic passthrough helper, on any receiver.
HELPER_RE = re.compile(
    r"(?:->|\.)\s*(?:" + "|".join(WIRE_TEXT_HELPERS) + r")\s*\(")
# An unqualified call, only meaningful inside PLANE_HELPER_FILES.
UNQUALIFIED_RE = re.compile(r"(?<![\w>.])send(?:Command|Cmd|CmdPublic)\s*\(")
# Definitions and declarations are not call sites.
DEF_RE = re.compile(r"\b\w+::send(?:Command|Cmd|CmdPublic)\s*\(")

# ANY qualified send(Command|Cmd|CmdPublic), whatever the receiver. Used to find
# receivers that are in NEITHER list, which is a hard error rather than a silent
# choice — see the UNKNOWN_RECEIVER note in main().
ANY_QUALIFIED_RE = re.compile(
    r"\b(\w+)\s*(?:\(\s*\))?\s*(?:->|\.)\s*send(?:Command|Cmd|CmdPublic)\s*\(")


def unknown_receivers(text: str) -> set[str]:
    """Receivers of a send* call that are classified neither Flex nor foreign.

    An allow-list gets the DEFAULT right for a new foreign protocol — it is
    ignored rather than frozen — but it makes the opposite mistake silently: a
    Flex call through an unrecognised member name (`m_radio->sendCommand(...)`)
    is not counted, not reported and not blocked, in a brand-new file included
    (#5619 re-review, K5PTB). That is the same failure shape as the original
    SliceModel hole, arriving by a different route, and it contradicts the
    guarantee static-checks.yml states.

    So every receiver is classified and an unknown one FAILS, asking for a
    decision instead of making one. A new amplifier driver costs one line in
    FOREIGN_RECEIVERS; a misspelled Flex receiver costs a CI failure rather than
    a hole.
    """
    seen = {m.group(1) for m in ANY_QUALIFIED_RE.finditer(text)}
    return (seen - set(RADIO_MODEL_RECEIVERS) - set(RADIO_MODEL_ACCESSORS)
            - set(FOREIGN_RECEIVERS))


UNCLASSIFIED: dict[str, list[str]] = {}


def _strip(text: str) -> str:
    """Blank string literals, then remove comments. See count_for."""
    text = re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', text)
    text = re.sub(r"//[^\n]*", "", text)
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def count_for(path: Path) -> int:
    """Flex command-plane sites in one above-seam file, receiver resolved."""
    text = path.read_text(encoding="utf-8", errors="replace")
    # A comment that SAYS "emit commandReady" is not a call site; grep counted
    # one in SliceModel.cpp.
    #
    # STRING LITERALS ARE BLANKED FIRST. Stripping `//` straight away ate the
    # rest of any line containing a "http://…" literal, taking a real call with
    # it — an UNDER-count, so it would have passed silently (#5619 review,
    # K5PTB). No instance exists above the seam today. Blanking is safe here
    # because only the call pattern is counted, never the command text.
    text = re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', text)
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)

    rel = path.relative_to(REPO).as_posix()
    n = (len(EMIT_RE.findall(text))
         + len(QUALIFIED_RE.findall(text))
         + len(ACCESSOR_RE.findall(text))
         + len(HELPER_RE.findall(text)))
    if rel in PLANE_HELPER_FILES:
        n += len(UNQUALIFIED_RE.findall(text)) - len(DEF_RE.findall(text))
    return max(n, 0)


def scan() -> dict[str, int]:
    current: dict[str, int] = {}
    for root in ABOVE_SEAM_DIRS:
        if not root.exists():
            raise SystemExit(f"check_command_plane: above-seam root missing: {root}")
        seen = 0
        paths: list[Path] = []
        for suffix in SCANNED_SUFFIXES:
            paths.extend(root.rglob(f"*{suffix}"))
        for path in sorted(set(paths)):
            rel = path.relative_to(REPO).as_posix()
            if rel.startswith(BACKENDS_PREFIX):
                continue
            seen += 1
            unknown = unknown_receivers(_strip(path.read_text(encoding="utf-8",
                                                              errors="replace")))
            if unknown:
                UNCLASSIFIED[rel] = sorted(unknown)
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

    for rel, names in sorted(UNCLASSIFIED.items()):
        blocking += 1
        print(f"::error file={rel},title=command-plane-receiver::"
              f"{rel} calls send(Command|Cmd|CmdPublic) on unclassified receiver(s) "
              f"{', '.join(names)}. This checker must know whether that reaches the "
              f"Flex command plane or a different device's wire — it will not guess. "
              f"Add the name to RADIO_MODEL_RECEIVERS (it is the RadioModel, and the "
              f"call is a conversion site) or to FOREIGN_RECEIVERS (it is another "
              f"device) in tools/check_command_plane.py (#5262 M4).")

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
