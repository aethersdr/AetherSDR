#!/usr/bin/env python3
"""
AetherSDR accessibility static checker.

Scans changed (or all) src/gui/ C++/header files for common accessibility
omissions and emits GitHub Actions annotation-format warnings so findings
appear inline on PRs.

Exit 0 always -- accessibility issues are informational, not build-blocking.
Sighted developers should see the findings without being blocked by them.

Usage:
    python tools/check_a11y.py [file1.cpp file2.h ...]
    git diff --name-only origin/main...HEAD | python tools/check_a11y.py
    python tools/check_a11y.py          # scans all of src/gui/
"""

import re
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# Annotation helpers
# ---------------------------------------------------------------------------

def warn(filepath: str, line: int, title: str, message: str) -> None:
    """Emit a GitHub Actions warning annotation."""
    clean = filepath.lstrip("./")
    print(f"::warning file={clean},line={line},title={title}::{message}")


# ---------------------------------------------------------------------------
# File collection
# ---------------------------------------------------------------------------

def collect_files(args: list) -> list:
    candidates = []

    if args:
        candidates = args
    elif not sys.stdin.isatty():
        candidates = [line.strip() for line in sys.stdin if line.strip()]

    if candidates:
        return [
            Path(p) for p in candidates
            if re.search(r"src[/\\]gui[/\\]", p)
            and p.endswith((".cpp", ".h"))
            and Path(p).is_file()
        ]

    # Full scan fallback
    gui_root = Path("src/gui")
    if not gui_root.is_dir():
        gui_root = Path(__file__).parent.parent / "src" / "gui"
    if not gui_root.is_dir():
        print("::warning title=a11y-check::Could not locate src/gui/ -- skipping scan")
        return []

    return list(gui_root.rglob("*.cpp")) + list(gui_root.rglob("*.h"))


# ---------------------------------------------------------------------------
# Per-file checks
# ---------------------------------------------------------------------------

def check_file(path: Path) -> list:
    """Run all accessibility checks on a single file.
    Returns list of (line_number, title, message) tuples.
    """
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return [(1, "a11y-check-error", f"Could not read file: {exc}")]

    lines = text.splitlines()
    findings = []
    findings += check_interactive_labels(lines)
    findings += check_value_change_methods(lines)
    findings += check_widget_constructor_names(lines)
    findings += check_custom_painted_widgets(lines, path)
    return findings


# ---------------------------------------------------------------------------
# Check 1 -- Interactive QLabel
# ---------------------------------------------------------------------------

def check_interactive_labels(lines: list) -> list:
    findings = []
    label_vars = set()
    qlabel_decl = re.compile(r"\bQLabel\s*\*?\s*(\w+)\s*(?:=|;|\()")
    for ln, line in enumerate(lines, 1):
        m = qlabel_decl.search(line)
        if m:
            label_vars.add(m.group(1))

    subclass_qlabel = re.compile(r"class\s+\w+\s*:\s*public\s+QLabel")
    mouse_press = re.compile(r"\bmousePressEvent\b")
    event_filter = re.compile(r"\beventFilter\b")
    full_text = "\n".join(lines)
    is_qlabel_subclass = bool(subclass_qlabel.search(full_text))

    for ln, line in enumerate(lines, 1):
        if is_qlabel_subclass and mouse_press.search(line):
            findings.append((
                ln,
                "interactive-label-no-role",
                "QLabel subclass overrides mousePressEvent -- replace with QPushButton "
                "or add keyboard activation + QAccessibleInterface returning Button role.",
            ))

        if event_filter.search(line):
            for var in label_vars:
                start = max(0, ln - 6)
                end = min(len(lines), ln + 5)
                context = "\n".join(lines[start:end])
                if re.search(rf"\b{re.escape(var)}\b", context):
                    findings.append((
                        ln,
                        "interactive-label-no-role",
                        f"QLabel '{var}' appears in eventFilter -- replace with "
                        "QPushButton or add keyboard activation path.",
                    ))
                    break

    return findings


# ---------------------------------------------------------------------------
# Check 2 -- Value-change methods missing live-region update
# ---------------------------------------------------------------------------

def check_value_change_methods(lines: list) -> list:
    findings = []
    value_method_re = re.compile(
        r"\b(setLevel|setValue|updateFreqLabel|setText)\s*\("
    )
    update_a11y_re = re.compile(r"QAccessible::updateAccessibility")

    i = 0
    while i < len(lines):
        line = lines[i]
        m = value_method_re.search(line)
        if m:
            method_start = i + 1
            method_name = m.group(1)

            brace_depth = 0
            body_lines = []
            j = i
            found_open = False
            while j < len(lines):
                for ch in lines[j]:
                    if ch == "{":
                        brace_depth += 1
                        found_open = True
                    elif ch == "}":
                        brace_depth -= 1
                if found_open:
                    body_lines.append(lines[j])
                    if brace_depth == 0:
                        break
                j += 1

            if found_open and body_lines:
                body_text = "\n".join(body_lines)
                if not update_a11y_re.search(body_text):
                    findings.append((
                        method_start,
                        "value-method-missing-a11y-update",
                        f"{method_name}() changes a displayed value but never calls "
                        "QAccessible::updateAccessibility -- screen readers won't "
                        "announce the change. Add: "
                        "QAccessibleValueChangeEvent ev(this, newValue); "
                        "QAccessible::updateAccessibility(&ev);",
                    ))
            i = j + 1
            continue
        i += 1

    return findings


# ---------------------------------------------------------------------------
# Check 3 -- Widget constructor with no setAccessibleName calls
# ---------------------------------------------------------------------------

def check_widget_constructor_names(lines: list) -> list:
    findings = []
    ctor_re = re.compile(r"^(\w+)::\1\s*\(")
    qwidget_subclass_re = re.compile(
        r"class\s+(\w+)\s*:[^{]*\bQ(?:Widget|Frame|Dialog|MainWindow|GroupBox|"
        r"ScrollArea|StackedWidget|TabWidget|Splitter|DockWidget)\b"
    )
    new_qwidget_re = re.compile(r"\bnew\s+Q[A-Z]\w+\s*\(")
    accessible_name_re = re.compile(r"\bsetAccessibleName\s*\(")
    full_text = "\n".join(lines)

    widget_classes = set()
    for m in qwidget_subclass_re.finditer(full_text):
        widget_classes.add(m.group(1))

    if not widget_classes:
        return findings

    i = 0
    while i < len(lines):
        line = lines[i]
        m = ctor_re.search(line)
        if m:
            class_name = m.group(1)
            if class_name not in widget_classes:
                i += 1
                continue

            ctor_start = i + 1
            brace_depth = 0
            body_lines = []
            j = i
            found_open = False
            while j < len(lines):
                for ch in lines[j]:
                    if ch == "{":
                        brace_depth += 1
                        found_open = True
                    elif ch == "}":
                        brace_depth -= 1
                if found_open:
                    body_lines.append(lines[j])
                    if brace_depth == 0:
                        break
                j += 1

            if found_open and body_lines:
                body_text = "\n".join(body_lines)
                creates_widgets = bool(new_qwidget_re.search(body_text))
                has_names = bool(accessible_name_re.search(body_text))

                if creates_widgets and not has_names:
                    findings.append((
                        ctor_start,
                        "constructor-missing-accessible-names",
                        f"{class_name}::{class_name}() creates child widgets but "
                        "calls setAccessibleName() zero times -- AT users cannot "
                        "identify controls. Add setAccessibleName(tr(\"...\")) for "
                        "each interactive or informational child widget.",
                    ))
            i = j + 1
            continue
        i += 1

    return findings


# ---------------------------------------------------------------------------
# Check 4 -- Custom-painted widget without QAccessibleInterface companion
# ---------------------------------------------------------------------------

def check_custom_painted_widgets(lines: list, path: Path) -> list:
    findings = []
    paint_event_re = re.compile(r"\bpaintEvent\s*\(")
    accessible_iface_re = re.compile(r"QAccessibleInterface")
    qwidget_subclass_re = re.compile(
        r"class\s+(\w+)\s*:[^{]*\bQ(?:Widget|Frame|OpenGLWidget|GLWidget)\b"
    )
    full_text = "\n".join(lines)

    if not paint_event_re.search(full_text):
        return findings

    widget_classes = [m.group(1) for m in qwidget_subclass_re.finditer(full_text)]
    if not widget_classes:
        return findings

    has_iface_here = bool(accessible_iface_re.search(full_text))
    stem = path.stem
    companion_patterns = [
        path.parent / f"{stem}Accessible.h",
        path.parent / f"{stem}Accessible.cpp",
        path.parent / f"{stem}_accessible.h",
        path.parent / f"{stem}_accessible.cpp",
    ]
    has_companion = any(p.is_file() for p in companion_patterns)

    if has_iface_here or has_companion:
        return findings

    for ln, line in enumerate(lines, 1):
        if paint_event_re.search(line):
            for cls in widget_classes:
                findings.append((
                    ln,
                    "custom-painted-widget-needs-accessible-interface",
                    f"{cls} overrides paintEvent but has no QAccessibleInterface "
                    "subclass in this file or a companion *Accessible.h -- "
                    "screen readers cannot read custom-drawn content. "
                    "Implement QAccessibleInterface returning meaningful "
                    "text(Name)/text(Value) strings, or add "
                    "// TODO(a11y): QAccessibleInterface needed and open a "
                    "follow-up issue tagged 'GUI'.",
                ))
            break

    return findings


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    files = collect_files(sys.argv[1:])

    if not files:
        print("a11y-check: no src/gui/ files to check.")
        sys.exit(0)

    total_findings = 0
    file_count = 0

    for path in sorted(files):
        findings = check_file(path)
        if findings:
            file_count += 1
            for (line, title, message) in findings:
                warn(str(path), line, title, message)
                total_findings += 1

    print()
    print("=== AetherSDR accessibility check summary ===")
    print(f"Files scanned      : {len(files)}")
    print(f"Files with findings: {file_count}")
    print(f"Total findings     : {total_findings}")
    if total_findings == 0:
        print("No accessibility issues found.")
    else:
        print(
            "These are warnings only -- the build is not blocked. "
            "Findings appear inline on the PR diff."
        )

    sys.exit(0)


if __name__ == "__main__":
    main()
