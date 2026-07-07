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
       units themselves. Today's coupling is frozen as a per-file
       baseline of the EXACT vendor headers each file may include
       (KNOWN_VENDOR_INCLUDE_BASELINE). A file may only SHRINK its set;
       any header not in its baseline row — a brand-new include, OR a
       lateral swap that keeps the count flat (drop RadioConnection, add
       KiwiSdrManager) — fails --strict. A set, not a count, so churn
       that trades one vendor dependency for another can't slip through.
       This is RFC step 2.4's ratchet: the interface already exists, so
       no new code should reach around it — existing includers are
       decoupled subsystem-by-subsystem (each routed through the seam)
       and their rows driven to empty. Ratchet-only: the vendor files
       are NOT relocated in this step; EB3 makes the boundary
       enforceable in place. The vendor vocabulary is derived at runtime
       from the touchpoint audit (docs/architecture/
       aetherd-touchpoint-tags.json) so the audit is the single source
       of truth — a header newly tagged vendor there is enforced without
       editing this file. See AGENTS.md ("Engine boundary ratchet — EB3").

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
import json
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
# The vendor vocabulary is DERIVED at runtime from the touchpoint audit
# (single source of truth) rather than hand-copied, so a header newly tagged
# `vendor(*)` there is enforced automatically — no silent drift where the audit
# grows a vendor family but this checker keeps permitting it.
VENDOR_TAGS_JSON = REPO / "docs" / "architecture" / "aetherd-touchpoint-tags.json"
# Sanity floor: the audit currently tags 21 radio-family vendor headers (the
# original 26 minus reclassified peripherals not behind the radio seam — the
# 4O3A accessory family TunerModel/AntennaGeniusModel/Tgxl/Pgxl → peripheral(4o3a),
# and the FlexControl USB knob → ui-support). This floor only guards against the
# audit being moved/gutted (a parse yielding near zero), NOT the exact count —
# deliberate reclassifications lower it over time, so keep the floor well below
# the live count.
VENDOR_STEMS_FLOOR = 15


def load_vendor_vocabulary():
    """Derive (stem -> family, {vendor TU rel-paths}) from the touchpoint audit.

    - stems: an include is a vendor include when the included header's basename
      stem is a key here (so "core/RadioConnection.h", "RadioConnection.h",
      "../core/RadioConnection.h", and <...> all resolve the same).
    - tu_paths: the EXACT rel-paths of the vendor translation units (each tagged
      header plus its sibling impl files). The below-seam exemption keys on these
      full paths, NOT a bare stem — so a *different* file that merely shares a
      vendor stem (a future src/gui/CommandParser.cpp) is NOT exempted.

    Returns (stems, tu_paths, error-or-None). On any load/parse failure the
    error string is returned so main() can emit a blocking EB3-load finding
    rather than silently scanning with an empty vocabulary.
    """
    try:
        data = json.loads(VENDOR_TAGS_JSON.read_text())
    except (OSError, ValueError) as e:
        return {}, set(), f"cannot read {VENDOR_TAGS_JSON.name}: {e}"
    stems, tu_paths = {}, set()
    for hdr, meta in data.items():
        tag = (meta or {}).get("tag", "")
        if not tag.startswith("vendor"):
            continue
        family = tag[tag.find("(") + 1:tag.find(")")] if "(" in tag else "vendor"
        stems[Path(hdr).stem] = family
        base = Path("src") / hdr           # e.g. "src/core/CommandParser.h"
        for suf in ENGINE_SUFFIXES:         # header + sibling impl TUs
            tu_paths.add(base.with_suffix(suf).as_posix())
    if len(stems) < VENDOR_STEMS_FLOOR:
        return stems, tu_paths, (
            f"parsed only {len(stems)} vendor stems from {VENDOR_TAGS_JSON.name} "
            f"(expected >= {VENDOR_STEMS_FLOOR}) — the audit moved or its schema "
            "changed; EB3 is under-armed")
    return stems, tu_paths, None


VENDOR_INCLUDE_RE = re.compile(
    r'^\s*#\s*include\s*[<"](?P<path>[A-Za-z0-9_./]+)\.h[>"]'
)

# EB3 per-file baseline: today's above-seam files and the EXACT set of vendor
# headers each may include (frozen 2026-07-06, RFC step 2.4). A SET, not a count
# — a file may only drop headers from its row; any header not listed (a new
# include OR a lateral swap that keeps the count flat) fails --strict. Decouple
# a file by routing its radio access through IRadioBackend, then delete the
# dropped stem(s) from its row; delete the row when it empties. NEVER add a stem
# or a row to make a build pass.
KNOWN_VENDOR_INCLUDE_BASELINE = {
    "src/core/TciProtocol.cpp": ["DaxIqModel"],
    "src/core/TciServer.cpp": ["DaxIqModel", "StreamStatus"],
    "src/core/WfmDemodulator.cpp": ["DaxIqModel"],
    "src/gui/Ax25HfPacketDecodeDialog.cpp": ["DaxTxPolicy"],
    "src/gui/ConnectionPanel.h": ["SmartLinkClient"],
    "src/gui/DaxIqApplet.cpp": ["DaxIqModel"],
    "src/gui/DvkPanel.cpp": ["DvkWavTransfer"],
    "src/gui/KiwiPublicReceiverPicker.h": ["KiwiPublicDirectory"],
    "src/gui/KiwiSdrApplet.h": ["KiwiSdrClient"],
    "src/gui/MainWindow.cpp": ["DvkWavTransfer", "KiwiSdrManager", "PanadapterStream", "RadioStatusOwnership", "StreamStatus"],
    "src/gui/MainWindow.h": ["SmartLinkClient", "WanConnection"],
    "src/gui/MainWindowHelpers.cpp": ["PanadapterStream", "SmartLinkClient"],
    "src/gui/MainWindow_Controllers.cpp": ["KiwiSdrProtocol"],
    "src/gui/MainWindow_KiwiSdr.cpp": ["KiwiSdrClient", "KiwiSdrManager", "KiwiSdrProtocol"],
    "src/gui/MainWindow_ReceiveSync.cpp": ["KiwiSdrManager"],
    "src/gui/MainWindow_Shortcuts.cpp": ["KiwiSdrProtocol"],
    "src/gui/MainWindow_Wiring.cpp": ["KiwiSdrManager", "KiwiSdrProtocol", "ProfileLoadCommand"],
    "src/gui/MemoryDialog.cpp": ["MemoryCsvCompat", "RadioConnection"],
    "src/gui/NetworkDiagnosticsDialog.h": ["PanadapterStream"],
    "src/gui/ProfileImportExportDialog.h": ["ProfileTransfer"],
    "src/gui/RadioSetupDialog.cpp": ["FirmwareStager", "FirmwareUploader", "KiwiSdrManager", "PanadapterStream", "WanConnection"],
    "src/gui/RxApplet.cpp": ["KiwiSdrManager", "KiwiSdrProtocol"],
    "src/gui/SMeterWidget.h": ["KiwiSdrProtocol"],
    "src/gui/SpectrumOverlayMenu.cpp": ["KiwiSdrManager"],
    "src/gui/SpectrumWidget.cpp": ["KiwiSdrProtocol"],
    "src/gui/SupportDialog.cpp": ["RadioConnection"],
    "src/gui/VfoWidget.cpp": ["KiwiSdrManager", "KiwiSdrProtocol"],
    "src/gui/VfoWidget.h": ["KiwiSdrProtocol"],
    "src/gui/WaveformsDialog.cpp": ["FlexWaveformModel", "WaveformInstaller"],
    "src/models/RadioModel.cpp": ["CommandParser", "ProfileLoadCommand", "RadioStatusOwnership", "StreamStatus"],
    "src/models/RadioModel.h": ["CommandParser", "DaxIqModel", "DaxTxPolicy", "FlexWaveformModel", "PanadapterStream", "RadioConnection", "RadioStatusOwnership", "WanConnection"],
    "src/models/SliceModel.cpp": ["KiwiSdrProtocol"],
    "src/models/TransmitInhibitPolicy.h": ["CommandParser"],
}
# Per-directory vacuity floor for the above-seam scan (EB3's analog of EB0):
# if src/gui/ — the bulk of the baseline — is ever renamed/relocated, its files
# vanish from the scan, every gui row degrades to a non-blocking EB3-stale
# warning, and the gui ratchet silently disarms while CI stays green. Require
# each above-seam dir to still yield at least this many files on a full scan.
ABOVE_SEAM_DIR_FLOOR = 20

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
                rel = p.resolve().relative_to(REPO).as_posix()
                if any(rel.startswith(f"src/{d}/") for d in ("core", "models")):
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


def is_below_seam(rel, vendor_tu_paths):
    """A file is BELOW the radio seam — free to include vendor code — if it is
    in the backend tree, or IS one of the vendor translation units itself
    (RadioConnection.cpp including StreamStatus.h is fine). The exemption keys on
    the vendor TU's EXACT rel-path, not a bare stem: a different file that merely
    shares a vendor basename (a future src/gui/CommandParser.cpp) is NOT exempt
    and its vendor includes are still checked."""
    return rel.startswith(BACKENDS_PREFIX) or rel in vendor_tu_paths


def check_vendor_file(path, vendor_stems, vendor_tu_paths):
    """EB3 — vendor-include findings for one ABOVE-SEAM file. The baseline row is
    the SET of vendor headers the file may include: a listed stem warns
    'EB3-known'; any stem NOT in the row — a new include, or a lateral swap that
    keeps the count flat — is a blocking EB3. A file with no row may include no
    vendor header at all."""
    rel = path.relative_to(REPO).as_posix()
    if is_below_seam(rel, vendor_tu_paths):
        return []
    lines = path.read_text(errors="replace").splitlines()
    hits = []  # (line_no, stem, family)
    for i, line in enumerate(lines, 1):
        m = VENDOR_INCLUDE_RE.match(line)
        if not m:
            continue
        stem = Path(m.group("path")).name
        fam = vendor_stems.get(stem)
        if fam:
            hits.append((i, stem, fam))

    allowed = set(KNOWN_VENDOR_INCLUDE_BASELINE.get(rel, ()))
    tracked = rel in KNOWN_VENDOR_INCLUDE_BASELINE
    findings = []
    for i, stem, fam in hits:
        if stem in allowed:
            findings.append(("warning", i, "EB3-known",
                f"{rel} includes vendor({fam}) header {stem}.h — tracked legacy; "
                "the baseline may only shrink. Decouple via IRadioBackend, then "
                "drop this stem from the file's KNOWN_VENDOR_INCLUDE_BASELINE row."))
        elif tracked:
            # A stem not in this tracked file's allowed set — new or swapped-in.
            findings.append(("error", i, "EB3",
                f"{rel} includes vendor({fam}) header {stem}.h, which is not in "
                "its EB3 baseline set — new (or laterally swapped-in) vendor "
                "coupling above the radio seam. Route it through IRadioBackend "
                "(aetherd RFC §5.5 / step 2.4); the baseline set only shrinks."))
        else:
            # Untracked above-seam file: any vendor include is new coupling.
            findings.append(("error", i, "EB3",
                f"{rel} includes vendor({fam}) header {stem}.h — code above the "
                "radio seam must reach the wire only through IRadioBackend "
                "(aetherd RFC §5.5 / step 2.4); no new vendor coupling. Route "
                "this through the backend, or add a seam verb/signal."))
    return findings


def collect_above_seam_files(args):
    """Files to scan for EB3. In file-args mode, honor the passed set (filtered
    to the above-seam dirs); otherwise the full gui+core+models tree."""
    if args:
        out = []
        for a in args:
            p = (REPO / a) if not Path(a).is_absolute() else Path(a)
            if p.suffix in ENGINE_SUFFIXES and p.is_file():
                rel = p.resolve().relative_to(REPO).as_posix()
                if any(rel.startswith(f"src/{d}/") for d in ("gui", "core", "models")):
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
                    help="exit 1 on any non-legacy EB0/EB1/EB2/EB3 finding")
    args = ap.parse_args()

    files = collect_files(args.files)
    vendor_stems, vendor_tu_paths, vocab_err = load_vendor_vocabulary()

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

    # EB3 vocabulary must load, or the whole vendor ratchet is disarmed — the
    # scan would run with an empty vendor set and pass every file vacuously.
    if vocab_err:
        annotate("error", "tools/check_engine_boundary.py", 1, "EB3-load",
                 f"EB3 vendor vocabulary failed to load: {vocab_err}. The vendor "
                 "ratchet is disarmed until this is fixed.")
        total += 1
        blocking += 1

    for f in files:
        for sev, line, rule, msg in check_file(f):
            annotate(sev, f.relative_to(REPO).as_posix(), line, rule, msg)
            total += 1
            if rule in ("EB1", "EB2", "EB0"):
                blocking += 1

    # EB3 — vendor-include ratchet over the wider above-seam tree.
    seam_files = collect_above_seam_files(args.files)

    # Per-directory vacuity floor (EB3's analog of EB0): on a full scan, if any
    # above-seam dir collapses below the floor it was renamed/relocated and its
    # half of the ratchet silently disarmed — fail loudly. gui/ holds the bulk
    # of the baseline, so an EB0 that only counts core+models wouldn't catch it.
    if not args.files:
        for d in ABOVE_SEAM_DIRS:
            n = sum(1 for p in d.rglob("*") if p.suffix in ENGINE_SUFFIXES)
            if n < ABOVE_SEAM_DIR_FLOOR:
                annotate("error", "tools/check_engine_boundary.py", 1, "EB3-floor",
                         f"only {n} files under {d.relative_to(REPO)} "
                         f"(floor {ABOVE_SEAM_DIR_FLOOR}) — the above-seam tree "
                         "moved; the EB3 ratchet is disarmed for it. Fix "
                         "ABOVE_SEAM_DIRS.")
                total += 1
                blocking += 1

    for f in seam_files:
        for sev, line, rule, msg in check_vendor_file(f, vendor_stems, vendor_tu_paths):
            annotate(sev, f.relative_to(REPO).as_posix(), line, rule, msg)
            total += 1
            if rule in ("EB3", "EB3-floor", "EB3-load"):
                blocking += 1

    # Stale-baseline hygiene (full-tree scans only): a baseline row for a file
    # that no longer exists is dead weight — flag it non-blocking so it gets
    # pruned. (A row whose set merely shrank is fine; that's the ratchet working.)
    if not args.files:
        for rel in sorted(KNOWN_VENDOR_INCLUDE_BASELINE):
            if not (REPO / rel).is_file():
                annotate("warning", "tools/check_engine_boundary.py", 1,
                         "EB3-stale", f"baseline names {rel}, which no longer "
                         "exists — remove the stale row.")
                total += 1

    scanned = sorted({f.as_posix() for f in files} | {f.as_posix() for f in seam_files})
    print(f"engine-boundary: {len(scanned)} file(s) scanned "
          f"({len(files)} engine, {len(seam_files)} above-seam), "
          f"{total} finding(s), {blocking} would block under --strict")
    if args.strict and blocking:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
