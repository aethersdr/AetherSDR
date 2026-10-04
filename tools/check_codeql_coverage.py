#!/usr/bin/env python3
"""Check that selected Ninja targets compile every first-party CodeQL input.

Usage:
    python tools/check_codeql_coverage.py BUILD_DIR TARGET [TARGET ...]

The guard compares ``ninja -t commands all`` with the commands generated for
the requested production targets.  It deliberately considers only C and C++
inputs beneath ``src/`` and ``tools/``: test and third-party commands do not
affect the coverage decision.
"""

from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
C_CPP_SUFFIXES = frozenset({".c", ".cc", ".cp", ".cxx", ".cpp", ".c++", ".C"})


def ninja_commands(build_dir: Path, targets: list[str]) -> list[str]:
    """Return Ninja's expanded command lines for *targets*, or stop on error."""
    result = subprocess.run(
        ["ninja", "-C", str(build_dir), "-t", "commands", *targets],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        command = " ".join(shlex.quote(part) for part in result.args)
        print(f"error: {command} failed with exit status {result.returncode}", file=sys.stderr)
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)
        raise RuntimeError("ninja command listing failed")
    return result.stdout.splitlines()


def compiler_input(command: str, build_dir: Path) -> Path | None:
    """Return a C/C++ input from a shell compiler command, if it has one."""
    try:
        arguments = shlex.split(command, posix=True)
    except ValueError as error:
        raise RuntimeError(f"cannot parse Ninja command: {error}") from error

    for index, argument in enumerate(arguments):
        if argument == "-c" and index + 1 < len(arguments):
            source = arguments[index + 1]
        else:
            continue

        path = Path(source)
        if not path.is_absolute():
            path = build_dir / path
        path = path.resolve()
        return path if path.suffix in C_CPP_SUFFIXES else None
    return None


def first_party_inputs(commands: list[str], build_dir: Path, source_root: Path) -> tuple[int, set[Path]]:
    """Count compile commands and collect their ``src/`` or ``tools/`` inputs."""
    compile_count = 0
    inputs: set[Path] = set()
    source_root = source_root.resolve()
    for command in commands:
        source = compiler_input(command, build_dir)
        if source is None:
            continue
        compile_count += 1
        try:
            relative = source.relative_to(source_root)
        except ValueError:
            continue
        if relative.parts and relative.parts[0] in {"src", "tools"}:
            inputs.add(relative)
    return compile_count, inputs


def write_artifacts(output_dir: Path, baseline: set[Path], selected: set[Path], missing: set[Path],
                    counts: dict[str, int]) -> None:
    """Write sorted source inventories and their summary counts for CI review."""
    output_dir.mkdir(parents=True, exist_ok=True)
    for name, paths in (("baseline-sources.txt", baseline),
                        ("selected-sources.txt", selected),
                        ("missing-sources.txt", missing)):
        (output_dir / name).write_text(
            "".join(f"{path.as_posix()}\n" for path in sorted(paths)), encoding="utf-8")
    (output_dir / "counts.json").write_text(
        json.dumps(counts, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", type=Path, help="configured Ninja build directory")
    parser.add_argument("targets", nargs="+", help="targets CodeQL will build")
    parser.add_argument("--source-root", type=Path, default=REPO_ROOT,
                        help="repository root (default: this script's repository)")
    parser.add_argument("--output-dir", type=Path,
                        help="write baseline, selected, missing, and count artifacts here")
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    source_root = args.source_root.resolve()
    if not build_dir.is_dir():
        print(f"error: build directory does not exist: {build_dir}", file=sys.stderr)
        return 2
    if not source_root.is_dir():
        print(f"error: source root does not exist: {source_root}", file=sys.stderr)
        return 2

    try:
        baseline_commands = ninja_commands(build_dir, ["all"])
        selected_commands = ninja_commands(build_dir, args.targets)
        baseline_compile_count, baseline = first_party_inputs(
            baseline_commands, build_dir, source_root)
        selected_compile_count, selected = first_party_inputs(
            selected_commands, build_dir, source_root)
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    missing = baseline - selected
    counts = {
        "baseline_compile_invocations": baseline_compile_count,
        "baseline_first_party_inputs": len(baseline),
        "missing_first_party_inputs": len(missing),
        "selected_compile_invocations": selected_compile_count,
        "selected_first_party_inputs": len(selected),
    }

    print("CodeQL compile coverage:")
    print(f"  all:      {baseline_compile_count} compiler invocation(s), "
          f"{len(baseline)} first-party input(s)")
    print(f"  selected: {selected_compile_count} compiler invocation(s), "
          f"{len(selected)} first-party input(s)")
    if args.output_dir:
        write_artifacts(args.output_dir, baseline, selected, missing, counts)
        print(f"  artifacts: {args.output_dir.resolve()}")

    if not baseline:
        print("error: default Ninja graph has no first-party C/C++ compile inputs; refusing to pass",
              file=sys.stderr)
        return 1
    if not selected:
        print("error: selected targets have no first-party C/C++ compile inputs; refusing to pass",
              file=sys.stderr)
        return 1
    if missing:
        print("error: selected targets omit first-party CodeQL inputs:", file=sys.stderr)
        for path in sorted(missing):
            print(f"  {path.as_posix()}", file=sys.stderr)
        return 1

    print("CodeQL compile coverage: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
