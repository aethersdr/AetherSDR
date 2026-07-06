#!/usr/bin/env python3
"""
AetherSDR engine-boundary static checker (aetherd migration ratchet).

Guards the dependency direction the aetherd RFC
(docs/aetherd-headless-engine-design.md) is built on:

  EB1  No file under src/core/ or src/models/ may include a src/gui/
       header. One legacy offender exists (AutomationServer.cpp →
       ConnectionPanel.h) and is tracked for seam-extraction in RFC
       step 1; any OTHER occurrence is an error.

  EB2  No file under src/core/ or src/models/ may include a QtWidgets
       class header. Five legacy offenders are known and tracked for
       relocation during RFC step 1 (KNOWN_WIDGETS_LEGACY below); they
       warn as "known". Any OTHER file warns as NEW leakage.

  EB3  No file ABOVE the radio seam may include a VENDOR header —
       family-specific wire code (SmartSDR/FlexLib + KiwiSDR) that the
       aetherd RFC keeps *behind* IRadioBackend. "Above the seam" is
       everything in src/gui/, src/core/, and src/models/ EXCEPT the
       backend tree (src/core/backends/) and the vendor translation
       units themselves. Today's coupling is frozen as a per-file,
       shrink-only baseline (KNOWN_VENDOR_INCLUDE_BASELINE); any NEW
       above-seam vendor include, or any INCREASE in a tracked file,
       fails --strict. This is RFC step 2.4's ratchet: the interface
       already exists, so no new code should reach around it — the
       existing includers are decoupled subsystem-by-subsystem (each
       routed through the seam) and their counts driven to zero.
       Ratchet-only: the vendor files are NOT relocated in this step;
       EB3 makes the boundary enforceable in place. See AGENTS.md
       ("Engine boundary ratchet — EB3 vendor includes").

Exit 0 always in default mode (annotation/warning stage, like
check_a11y.py). --strict exits 1 on any non-legacy EB1/EB2/EB3 finding
— new leakage blocks immediately while the tracked baselines shrink to
zero during the migration. Shrink the legacy lists; never grow them.

Usage:
    python tools/check_engine_boundary.py [file1 file2 ...]
    python tools/check_engine_boundary.py            # scan all engine files
    python tools/check_engine_boundary.py --strict

stdlib only; no third-party dependencies.
"""

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ENGINE_DIRS = [REPO / "src" / "core", REPO / "src" / "models"]
GUI_DIR = REPO / "src" / "gui"
# EB3 scans a WIDER set than EB1/EB2: the radio seam lives below all three of
# these, so gui/ is in scope for vendor-include leakage too.
ABOVE_SEAM_DIRS = [REPO / "src" / "gui", REPO / "src" / "core", REPO / "src" / "models"]
# Below the seam = the backend tree. Anything here may include vendor code freely.
BACKENDS_PREFIX = "src/core/backends/"

# Scanned source suffixes. Includes .mm (Objective-C++, e.g. MacMicPermission.mm)
# and .hpp/.cc so no engine TU is a blind spot for gui/QtWidgets leakage.
ENGINE_SUFFIXES = (".h", ".hpp", ".cpp", ".cc", ".mm")

# Legacy boundary violations inside the engine, tracked for relocation
# or seam-extraction in RFC step 1. Shrink these; never grow them.
# EB1 is now clear: no engine file includes a gui/ header (AutomationServer's
# ConnectionPanel dependency was inverted behind IConnectionAutomation in the
# step-1 PR). Any EB1 finding is now a hard error.
KNOWN_GUI_INCLUDE_LEGACY = set()
# EB2 is a per-file BASELINE COUNT, not a whole-file exemption: a tracked file
# may keep its known QtWidgets usages but any INCREASE fails --strict, so new
# leakage into the actively-migrating files (e.g. AutomationServer.cpp) is
# caught. Counts may only drop; when a file hits 0, remove it.
KNOWN_WIDGETS_LEGACY = {
    "src/core/TxKeyingMarker.h": 1,
    "src/core/ThemeManager.cpp": 1,
    "src/core/AutomationServer.cpp": 20,
    "src/core/ShortcutManager.cpp": 1,
    "src/core/SettingsHelpers.cpp": 1,
}

# ---- EB3: vendor headers (family-specific wire code, kept behind the seam) ----
# The 26 headers the aetherd touchpoint audit tags `vendor(flex)` / `vendor(kiwi)`
# (docs/architecture/aetherd-touchpoint-tags.json). Keyed by basename STEM — an
# include is a vendor include when its file's stem is in this map, regardless of
# how the path is written ("core/RadioConnection.h", "RadioConnection.h",
# "../core/RadioConnection.h", or <...>). Keep this in sync with the audit's
# vendor tag set; there are no basename collisions with non-vendor headers.
VENDOR_HEADERS = {
    # SmartSDR / FlexLib / Flex-ecosystem wire code
    "CommandParser": "flex", "DaxTxPolicy": "flex", "DvkWavTransfer": "flex",
    "FirmwareStager": "flex", "FirmwareUploader": "flex", "FlexControlManager": "flex",
    "MemoryCsvCompat": "flex", "PanadapterStream": "flex", "PgxlConnection": "flex",
    "ProfileTransfer": "flex", "RadioConnection": "flex", "SmartLinkClient": "flex",
    "StreamStatus": "flex", "TgxlConnection": "flex", "WanConnection": "flex",
    "WaveformInstaller": "flex", "AntennaGeniusModel": "flex", "DaxIqModel": "flex",
    "FlexWaveformModel": "flex", "ProfileLoadCommand": "flex",
    "RadioStatusOwnership": "flex", "TunerModel": "flex",
    # KiwiSDR wire code (the eventual second-backend template)
    "KiwiPublicDirectory": "kiwi", "KiwiSdrClient": "kiwi",
    "KiwiSdrManager": "kiwi", "KiwiSdrProtocol": "kiwi",
}

VENDOR_INCLUDE_RE = re.compile(
    r'^\s*#\s*include\s*[<"](?P<path>[A-Za-z0-9_./]+)\.h[>"]'
)

# EB3 per-file, shrink-only baseline: today's above-seam files and HOW MANY
# vendor headers each includes (frozen 2026-07-06, RFC step 2.4). Like EB2 this
# is a COUNT, not a whole-file exemption — any increase fails --strict. Drive
# each to zero by routing that file's radio access through IRadioBackend; when a
# file reaches 0, delete its row. NEVER add a row or raise a count.
KNOWN_VENDOR_INCLUDE_BASELINE = {
    "src/core/TciProtocol.cpp": 1,
    "src/core/TciServer.cpp": 2,
    "src/core/WfmDemodulator.cpp": 1,
    "src/gui/AntennaGeniusApplet.cpp": 1,
    "src/gui/Ax25HfPacketDecodeDialog.cpp": 1,
    "src/gui/ConnectionPanel.h": 1,
    "src/gui/DaxIqApplet.cpp": 1,
    "src/gui/DvkPanel.cpp": 1,
    "src/gui/KiwiPublicReceiverPicker.h": 1,
    "src/gui/KiwiSdrApplet.h": 1,
    "src/gui/MainWindow.cpp": 7,
    "src/gui/MainWindow.h": 7,
    "src/gui/MainWindowHelpers.cpp": 2,
    "src/gui/MainWindow_Controllers.cpp": 1,
    "src/gui/MainWindow_KiwiSdr.cpp": 3,
    "src/gui/MainWindow_ReceiveSync.cpp": 1,
    "src/gui/MainWindow_Shortcuts.cpp": 1,
    "src/gui/MainWindow_Wiring.cpp": 3,
    "src/gui/MemoryDialog.cpp": 2,
    "src/gui/NetworkDiagnosticsDialog.h": 1,
    "src/gui/ProfileImportExportDialog.h": 1,
    "src/gui/RadioSetupDialog.cpp": 9,
    "src/gui/RxApplet.cpp": 2,
    "src/gui/SMeterWidget.h": 1,
    "src/gui/ShackSwitchApplet.cpp": 1,
    "src/gui/SpectrumOverlayMenu.cpp": 1,
    "src/gui/SpectrumWidget.cpp": 1,
    "src/gui/SupportDialog.cpp": 1,
    "src/gui/TunerApplet.cpp": 1,
    "src/gui/TxApplet.cpp": 1,
    "src/gui/VfoWidget.cpp": 2,
    "src/gui/VfoWidget.h": 1,
    "src/gui/WaveformsDialog.cpp": 2,
    "src/models/RadioModel.cpp": 4,
    "src/models/RadioModel.h": 9,
    "src/models/SliceModel.cpp": 1,
    "src/models/TransmitInhibitPolicy.h": 1,
}

# Matches gui includes in "..." OR <...>, optional space, optional gui/ prefix,
# and SUBDIR paths (name group carries '/'), so angle-bracket, no-space, and
# bare-subpath forms (the tree uses e.g. "containers/ContainerManager.h") can't
# slip an engine->gui dependency past --strict.
GUI_INCLUDE_RE = re.compile(
    r'^\s*#\s*include\s*[<"](?:\.\./)*(?:gui/)?(?P<name>[A-Za-z0-9_./]+\.h)[>"]'
)
GUI_PREFIXED_RE = re.compile(r'^\s*#\s*include\s*[<"](?:\.\./)*gui/')

# Common QtWidgets class headers (curated; QShortcut/QAction/QUndoStack
# and QFileSystemModel live in QtGui in Qt 6 and are deliberately absent).
QTWIDGETS_CLASSES = {
    "QWidget", "QApplication", "QDialog", "QMainWindow", "QLabel",
    "QPushButton", "QToolButton", "QCheckBox", "QComboBox", "QSpinBox",
    "QDoubleSpinBox", "QSlider", "QLineEdit", "QTextEdit",
    "QPlainTextEdit", "QMenu", "QMenuBar", "QToolBar", "QStatusBar",
    "QMessageBox", "QFileDialog", "QColorDialog", "QFontDialog",
    "QInputDialog", "QScrollArea", "QScrollBar", "QSplitter",
    "QStackedWidget", "QTabWidget", "QTabBar", "QTableWidget",
    "QTableView", "QTreeWidget", "QTreeView", "QListWidget", "QListView",
    "QHeaderView", "QGroupBox", "QRadioButton", "QProgressBar", "QFrame",
    "QGraphicsView", "QGraphicsScene", "QBoxLayout", "QHBoxLayout",
    "QVBoxLayout", "QGridLayout", "QFormLayout", "QLayout", "QSizePolicy",
    "QStyle", "QStyleOption", "QStylePainter", "QProxyStyle", "QToolTip",
    "QWhatsThis", "QCompleter", "QSystemTrayIcon", "QDockWidget",
    "QWizard", "QCalendarWidget", "QDial", "QLCDNumber", "QFontComboBox",
    "QKeySequenceEdit", "QDateTimeEdit", "QRhiWidget", "QAbstractItemView",
    "QAbstractButton", "QAbstractScrollArea", "QAbstractSpinBox",
    "QAbstractSlider", "QButtonGroup", "QRubberBand", "QSplashScreen",
    "QToolBox", "QWidgetAction",
}
QT_INCLUDE_RE = re.compile(
    r'^\s*#\s*include\s*[<"](?:QtWidgets/)?(?P<name>Q[A-Za-z0-9]+)[>"]'
)
QTWIDGETS_MODULE_RE = re.compile(r'^\s*#\s*include\s*[<"]QtWidgets(?:/|[>"])')


def annotate(sev, filepath, line, title, message):
    print(f"::{sev} file={filepath},line={line},title={title}::{message}")


def collect_files(args):
    if args:
        files = []
        for a in args:
            p = (REPO / a) if not Path(a).is_absolute() else Path(a)
            if p.suffix in ENGINE_SUFFIXES and p.is_file():
                rel = p.resolve().relative_to(REPO)
                if any(str(rel).startswith(f"src/{d}/") for d in ("core", "models")):
                    files.append(p.resolve())
        return files
    files = []
    for d in ENGINE_DIRS:
        files.extend(p for p in sorted(d.rglob("*")) if p.suffix in ENGINE_SUFFIXES)
    return files


def check_file(path):
    """Return a list of (severity, line_no, rule, message) findings for one
    file. EB1 is per-line; EB2 is per-file (baseline-count) so growth in a
    tracked legacy file is caught, not exempted."""
    rel = path.relative_to(REPO).as_posix()
    text = path.read_text(errors="replace")
    lines = text.splitlines()
    findings = []
    widget_hits = []  # (line_no, class-name) across the whole file
    for i, line in enumerate(lines, 1):
        # EB1 — engine file includes a gui header (per line)
        m = GUI_INCLUDE_RE.match(line)
        if m and (GUI_PREFIXED_RE.match(line) or (GUI_DIR / m.group("name")).is_file()):
            # Bare-name includes only count when the header exists in gui/
            # and not beside the engine file (sibling engine includes are fine).
            if GUI_PREFIXED_RE.match(line) or not (path.parent / m.group("name")).is_file():
                if rel in KNOWN_GUI_INCLUDE_LEGACY:
                    findings.append(("warning", i, "EB1-known",
                        f"{rel} includes gui header {m.group('name')} — known "
                        "legacy; do not add further gui includes"))
                else:
                    findings.append(("error", i, "EB1",
                        f"{rel} includes gui header {m.group('name')} — the "
                        "engine must never depend on the UI (aetherd RFC "
                        "§2/§10; gui→core only)"))
        # EB2 — collect QtWidgets includes; verdict is per-file below
        qm = QT_INCLUDE_RE.match(line)
        if QTWIDGETS_MODULE_RE.match(line) or (
                qm and qm.group("name") in QTWIDGETS_CLASSES):
            widget_hits.append((i, qm.group("name") if qm else "QtWidgets"))

    baseline = KNOWN_WIDGETS_LEGACY.get(rel)
    if baseline is None:
        # Untracked file: every QtWidgets use is a blocking EB2.
        for i, what in widget_hits:
            findings.append(("warning", i, "EB2",
                f"{rel} uses QtWidgets ({what}) — engine code must not depend "
                "on QtWidgets (libaethercore must link without it, aetherd "
                "RFC §10); put UI code in src/gui/"))
    else:
        for i, what in widget_hits:
            findings.append(("warning", i, "EB2-known",
                f"{rel} uses QtWidgets ({what}) — tracked legacy "
                f"(baseline {baseline}); the count may only shrink"))
        if len(widget_hits) > baseline:
            # New widget leakage into a tracked, actively-migrating file.
            findings.append(("error", widget_hits[-1][0], "EB2",
                f"{rel} QtWidgets usage grew to {len(widget_hits)} (baseline "
                f"{baseline}) — the tracked count may only shrink; move new "
                "widget code to src/gui/ (or lower the baseline if you removed "
                "some)"))
    return findings


def is_below_seam(rel):
    """A file is BELOW the radio seam — free to include vendor code — if it is
    in the backend tree, or is a vendor translation unit itself (its stem names
    a vendor header, so RadioConnection.cpp including StreamStatus.h is fine)."""
    return rel.startswith(BACKENDS_PREFIX) or Path(rel).stem in VENDOR_HEADERS


def check_vendor_file(path):
    """EB3 — vendor-include findings for one ABOVE-SEAM file. Per-file count vs
    the frozen baseline (mirrors EB2): a tracked file's known vendor includes
    warn 'EB3-known'; a NEW above-seam includer, or a tracked file whose count
    grew, is a blocking EB3."""
    rel = path.relative_to(REPO).as_posix()
    if is_below_seam(rel):
        return []
    lines = path.read_text(errors="replace").splitlines()
    hits = []  # (line_no, stem, family)
    for i, line in enumerate(lines, 1):
        m = VENDOR_INCLUDE_RE.match(line)
        if not m:
            continue
        stem = Path(m.group("path")).name
        fam = VENDOR_HEADERS.get(stem)
        if fam:
            hits.append((i, stem, fam))

    findings = []
    baseline = KNOWN_VENDOR_INCLUDE_BASELINE.get(rel)
    if baseline is None:
        # Untracked above-seam file: every vendor include is NEW coupling.
        for i, stem, fam in hits:
            findings.append(("error", i, "EB3",
                f"{rel} includes vendor({fam}) header {stem}.h — code above the "
                "radio seam must reach the wire only through IRadioBackend "
                "(aetherd RFC §5.5 / step 2.4); no new vendor coupling. Route "
                "this through the backend, or add a seam verb/signal."))
    else:
        for i, stem, fam in hits:
            findings.append(("warning", i, "EB3-known",
                f"{rel} includes vendor({fam}) header {stem}.h — tracked legacy "
                f"(baseline {baseline}); the count may only shrink. Decouple via "
                "IRadioBackend and lower the baseline."))
        if len(hits) > baseline:
            findings.append(("error", hits[-1][0], "EB3",
                f"{rel} vendor includes grew to {len(hits)} (baseline "
                f"{baseline}) — the tracked count may only shrink; route new "
                "radio access through IRadioBackend (or lower the baseline if "
                "you removed some)."))
    return findings


def collect_above_seam_files(args):
    """Files to scan for EB3. In file-args mode, honor the passed set (filtered
    to the above-seam dirs); otherwise the full gui+core+models tree."""
    if args:
        out = []
        for a in args:
            p = (REPO / a) if not Path(a).is_absolute() else Path(a)
            if p.suffix in ENGINE_SUFFIXES and p.is_file():
                rel = p.resolve().relative_to(REPO)
                if any(str(rel).startswith(f"src/{d}/") for d in ("gui", "core", "models")):
                    out.append(p.resolve())
        return out
    out = []
    for d in ABOVE_SEAM_DIRS:
        out.extend(p for p in sorted(d.rglob("*")) if p.suffix in ENGINE_SUFFIXES)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="*")
    ap.add_argument("--strict", action="store_true",
                    help="exit 1 on EB1 or non-legacy EB2 findings")
    args = ap.parse_args()

    files = collect_files(args.files)

    # Floor check: a full-tree scan that finds no engine files means the
    # directory layout moved and the ratchet silently disarmed. Fail loudly
    # rather than pass vacuously. (Skipped when specific files are passed.)
    if not args.files and len(files) < 50:
        annotate("error", "tools/check_engine_boundary.py", 1, "EB0",
                 f"only {len(files)} engine files found scanning "
                 f"{[str(d.relative_to(REPO)) for d in ENGINE_DIRS]} — the "
                 "engine tree moved; the boundary ratchet is disarmed. Fix "
                 "ENGINE_DIRS.")
        print(f"engine-boundary: {len(files)} file(s) scanned — FLOOR TRIPPED")
        return 1

    blocking = 0
    total = 0
    for f in files:
        for sev, line, rule, msg in check_file(f):
            annotate(sev, f.relative_to(REPO).as_posix(), line, rule, msg)
            total += 1
            if rule in ("EB1", "EB2", "EB0"):
                blocking += 1

    # EB3 — vendor-include ratchet over the wider above-seam tree.
    seam_files = collect_above_seam_files(args.files)
    for f in seam_files:
        for sev, line, rule, msg in check_vendor_file(f):
            annotate(sev, f.relative_to(REPO).as_posix(), line, rule, msg)
            total += 1
            if rule == "EB3":
                blocking += 1

    # Stale-baseline hygiene (full-tree scans only): a baseline row for a file
    # that no longer exists is dead weight — flag it non-blocking so it gets
    # pruned. (A row whose COUNT dropped is fine; that's the ratchet working.)
    if not args.files:
        for rel in sorted(KNOWN_VENDOR_INCLUDE_BASELINE):
            if not (REPO / rel).is_file():
                annotate("warning", "tools/check_engine_boundary.py", 1,
                         "EB3-stale", f"baseline names {rel}, which no longer "
                         "exists — remove the stale row.")
                total += 1

    print(f"engine-boundary: {len(files)} engine + {len(seam_files)} above-seam "
          f"file(s) scanned, {total} finding(s), {blocking} would block under "
          "--strict")
    if args.strict and blocking:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
