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

Exit 0 always in default mode (annotation/warning stage, like
check_a11y.py). --strict exits 1 on any non-legacy EB1/EB2 finding —
new leakage blocks immediately while the tracked legacy set shrinks
to zero during step 1. Shrink the legacy lists; never grow them.

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

# Legacy boundary violations inside the engine, tracked for relocation
# or seam-extraction in RFC step 1. Shrink these lists; never grow them.
# EB1 is now clear: no engine file includes a gui/ header (AutomationServer's
# ConnectionPanel dependency was inverted behind IConnectionAutomation in the
# step-1 PR). Any EB1 finding is now a hard error.
KNOWN_GUI_INCLUDE_LEGACY = set()
KNOWN_WIDGETS_LEGACY = {
    "src/core/TxKeyingMarker.h",
    "src/core/ThemeManager.cpp",
    "src/core/AutomationServer.cpp",
    "src/core/ShortcutManager.cpp",
    "src/core/SettingsHelpers.cpp",
}

GUI_INCLUDE_RE = re.compile(
    r'^\s*#\s*include\s+"(?:\.\./)*(?:gui/)?(?P<name>[A-Za-z0-9_]+\.h)"'
)
GUI_PREFIXED_RE = re.compile(r'^\s*#\s*include\s+"(?:\.\./)*gui/')

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
    r"^\s*#\s*include\s+<(?:QtWidgets/)?(?P<name>Q[A-Za-z0-9]+)>"
)
QTWIDGETS_MODULE_RE = re.compile(r"^\s*#\s*include\s+<QtWidgets(?:/|>)")


def annotate(sev, filepath, line, title, message):
    print(f"::{sev} file={filepath},line={line},title={title}::{message}")


def collect_files(args):
    if args:
        files = []
        for a in args:
            p = (REPO / a) if not Path(a).is_absolute() else Path(a)
            if p.suffix in (".h", ".cpp") and p.is_file():
                rel = p.resolve().relative_to(REPO)
                if any(str(rel).startswith(f"src/{d}/") for d in ("core", "models")):
                    files.append(p.resolve())
        return files
    files = []
    for d in ENGINE_DIRS:
        files.extend(p for p in sorted(d.rglob("*")) if p.suffix in (".h", ".cpp"))
    return files


def check_file(path):
    """Yield (severity, line_no, rule, message) findings for one file."""
    rel = path.relative_to(REPO).as_posix()
    text = path.read_text(errors="replace")
    for i, line in enumerate(text.splitlines(), 1):
        # EB1 — engine file includes a gui header
        m = GUI_INCLUDE_RE.match(line)
        if m and (GUI_PREFIXED_RE.match(line) or (GUI_DIR / m.group("name")).is_file()):
            # Bare-name includes only count when the header exists in gui/
            # and not beside the engine file (sibling engine includes are fine).
            if GUI_PREFIXED_RE.match(line) or not (path.parent / m.group("name")).is_file():
                if rel in KNOWN_GUI_INCLUDE_LEGACY:
                    yield ("warning", i, "EB1-known",
                           f"{rel} includes gui header {m.group('name')} — "
                           "known legacy, tracked for seam-extraction in "
                           "RFC step 1; do not add further gui includes")
                else:
                    yield ("error", i, "EB1",
                           f"{rel} includes gui header {m.group('name')} — "
                           "the engine must never depend on the UI "
                           "(aetherd RFC §2/§10; gui→core only)")
        # EB2 — engine file includes QtWidgets
        qm = QT_INCLUDE_RE.match(line)
        is_widgets = QTWIDGETS_MODULE_RE.match(line) or (
            qm and qm.group("name") in QTWIDGETS_CLASSES
        )
        if is_widgets:
            what = qm.group("name") if qm else "QtWidgets"
            if rel in KNOWN_WIDGETS_LEGACY:
                yield ("warning", i, "EB2-known",
                       f"{rel} uses QtWidgets ({what}) — known legacy, "
                       "tracked for relocation in RFC step 1; do not add "
                       "further widget code here")
            else:
                yield ("warning", i, "EB2",
                       f"{rel} uses QtWidgets ({what}) — engine code must "
                       "not depend on QtWidgets (libaethercore must link "
                       "without it, aetherd RFC §10); put UI code in "
                       "src/gui/")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="*")
    ap.add_argument("--strict", action="store_true",
                    help="exit 1 on EB1 or non-legacy EB2 findings")
    args = ap.parse_args()

    files = collect_files(args.files)
    blocking = 0
    total = 0
    for f in files:
        for sev, line, rule, msg in check_file(f):
            annotate(sev, f.relative_to(REPO).as_posix(), line, rule, msg)
            total += 1
            if rule in ("EB1", "EB2"):
                blocking += 1

    print(f"engine-boundary: {len(files)} file(s) scanned, "
          f"{total} finding(s), {blocking} would block under --strict")
    if args.strict and blocking:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
