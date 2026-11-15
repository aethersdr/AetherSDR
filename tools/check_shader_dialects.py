#!/usr/bin/env python3
"""
AetherSDR shader-dialect checker.

`qt_add_shaders` bakes one GLSL slice per target dialect, and at runtime QRhi
picks the highest baked slice <= the version the driver reports. A shader whose
source uses a construct that some baked dialect cannot express still BAKES — qsb
emits the illegal text and exits 0. The failure surfaces only when a driver that
selects that slice tries to compile it, on a machine that has such a driver.

That is #4746: `dss_mesh.{vert,frag}` use the `flat` interpolation qualifier
(#4539), which is legal from GLSL 130 / ES 300 onward. They were in the block
that takes qsb's default set (100es, 120, 150), so a 120 slice was baked
containing `flat varying float vOverlayLayer;`. Desktop GPUs report GL 3.2+ and
select the legal 150 slice, so CI and every developer machine were fine; the
Raspberry Pi's v3d driver reports GL 3.1 / GLSL 1.40, found no 140 slice, fell
back to 120, and the 3D FFT silently never rendered.

Note what CI could not have caught: the build was green, the .qsb files were
produced, and the only evidence was a qCWarning on one class of hardware. Fixing
the CMake target set (#4754) fixed that shader. This fails the NEXT one.

WHY QUALIFIERS AND NOT A FULL GLSL PARSE: the shader sources are Vulkan-style
GLSL 440, and SPIRV-Cross lowers nearly everything on the way down — explicit
locations, UBO blocks, and modern builtins all become legal 120 constructs. The
interpolation qualifiers are the exception: they carry semantics no lowering can
preserve, so SPIRV-Cross emits them verbatim into every slice. Matching those and
nothing else is what keeps this gate free of false positives; a gate that cries
wolf gets ignored, and an ignored gate is worse than no gate.

Usage:
    python tools/check_shader_dialects.py            # report, exit 0
    python tools/check_shader_dialects.py --strict   # exit 1 on a violation
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CMAKELISTS = REPO / "CMakeLists.txt"

# qsb's default when qt_add_shaders is given no GLSL argument. Keep in sync with
# the Qt version in use; it has been this triple since Qt 6.0.
QSB_DEFAULT_GLSL = "100es,120,150"

# Interpolation qualifiers, and the first dialect that accepts each. `None` means
# the qualifier does not exist in that flavour at any version.
#   (minimum desktop GLSL, minimum GLSL ES)
QUALIFIER_MIN = {
    "flat": (130, 300),
    "noperspective": (130, None),
    "centroid": (120, 300),
    "sample": (400, 320),
}

# `flat out float x;`, `layout(location = 7) flat out float x;`, `flat in ...`,
# and the pre-130 `centroid varying ...` spelling. The qualifier may also trail
# the storage keyword (`out flat float x;`), which some drivers accept.
QUALIFIER_RE = re.compile(
    r"\b(?:(flat|noperspective|centroid|sample)\s+(?:in|out|varying)"
    r"|(?:in|out|varying)\s+(flat|noperspective|centroid|sample))\b"
)

BLOCK_RE = re.compile(r"qt_add_shaders\s*\(\s*(\w+)\s+\"([^\"]+)\"(.*?)\n\s*\)",
                      re.DOTALL)
GLSL_ARG_RE = re.compile(r"\bGLSL\s+\"([^\"]+)\"")
FILES_RE = re.compile(r"\bFILES\b(.*)", re.DOTALL)
DIALECT_RE = re.compile(r"^(\d+)(es)?$")


def strip_comments(source: str) -> str:
    """Drop // and /* */ comments so a commented-out `flat` is not a finding."""
    source = re.sub(r"/\*.*?\*/", " ", source, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", source)


def parse_dialects(spec: str) -> list[tuple[int, bool]]:
    """"100es,120,150" -> [(100, True), (120, False), (150, False)]."""
    dialects = []
    for token in spec.split(","):
        match = DIALECT_RE.match(token.strip())
        if match:
            dialects.append((int(match.group(1)), match.group(2) == "es"))
    return dialects


def parse_blocks(cmake_text: str) -> list[dict]:
    """Every qt_add_shaders() call, with its dialect set and shader files."""
    blocks = []
    for match in BLOCK_RE.finditer(cmake_text):
        body = match.group(3)
        glsl_arg = GLSL_ARG_RE.search(body)
        files_match = FILES_RE.search(body)
        files = []
        if files_match:
            for line in files_match.group(1).splitlines():
                line = line.split("#", 1)[0].strip()
                if line.endswith((".vert", ".frag", ".comp")):
                    files.append(line)
        blocks.append({
            "resource": match.group(2),
            "spec": glsl_arg.group(1) if glsl_arg else QSB_DEFAULT_GLSL,
            "explicit": glsl_arg is not None,
            "files": files,
        })
    return blocks


def dialect_name(version: int, is_es: bool) -> str:
    return f"{version}es" if is_es else str(version)


def check_file(path: Path, dialects: list[tuple[int, bool]]) -> list[str]:
    """Qualifiers in `path` that at least one baked dialect cannot express."""
    findings = []
    source = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    for match in QUALIFIER_RE.finditer(source):
        qualifier = match.group(1) or match.group(2)
        min_desktop, min_es = QUALIFIER_MIN[qualifier]
        line_no = source.count("\n", 0, match.start()) + 1
        for version, is_es in dialects:
            minimum = min_es if is_es else min_desktop
            if minimum is None:
                findings.append(
                    f"{path.relative_to(REPO)}:{line_no}: `{qualifier}` does not "
                    f"exist in GLSL ES, but a {dialect_name(version, is_es)} "
                    f"slice is baked")
            elif version < minimum:
                findings.append(
                    f"{path.relative_to(REPO)}:{line_no}: `{qualifier}` needs "
                    f"GLSL {'ES ' if is_es else ''}{minimum}+, but a "
                    f"{dialect_name(version, is_es)} slice is baked")
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Fail when a shader uses a qualifier one of its baked GLSL "
                    "dialects cannot express")
    parser.add_argument("--strict", action="store_true",
                        help="exit 1 when a shader/dialect mismatch is found")
    args = parser.parse_args()

    if not CMAKELISTS.exists():
        print(f"FAIL: {CMAKELISTS} is missing — shader dialects cannot be "
              f"verified.")
        return 1

    blocks = parse_blocks(CMAKELISTS.read_text(encoding="utf-8"))
    if not blocks:
        # Not "nothing to do": the GPU spectrum path is unconditional in the
        # tree, so no blocks means the regexes stopped matching the file.
        print("FAIL: no qt_add_shaders() blocks found in CMakeLists.txt — the "
              "parser is out of date and this gate is checking nothing.")
        return 1

    findings: list[str] = []
    checked = 0
    for block in blocks:
        dialects = parse_dialects(block["spec"])
        origin = "explicit" if block["explicit"] else "qsb default"
        print(f'qt_add_shaders "{block["resource"]}" — GLSL {block["spec"]} '
              f'({origin}), {len(block["files"])} file(s)')
        for rel in block["files"]:
            path = REPO / rel
            if not path.exists():
                findings.append(f"{rel}: listed in CMakeLists.txt but missing "
                                f"from the tree")
                continue
            checked += 1
            findings.extend(check_file(path, dialects))

    print(f"\nChecked {checked} shader source(s).")
    if not findings:
        print("OK: every shader compiles in all of its baked GLSL dialects.")
        return 0

    print(f"\n{len(findings)} shader/dialect mismatch(es):\n")
    for finding in findings:
        print(f"  {finding}")
    print("\nA slice that cannot express the qualifier still BAKES — qsb emits "
          "illegal text and exits 0. It fails at runtime, on the subset of "
          "drivers that select that slice, as a log warning and a dead feature "
          "(see #4746).\n"
          "Fix by moving the shader into a qt_add_shaders() block whose GLSL "
          "set omits the dialects it cannot support, not by widening this "
          "check.")
    return 1 if args.strict else 0


if __name__ == "__main__":
    sys.exit(main())
