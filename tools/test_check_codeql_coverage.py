#!/usr/bin/env python3
"""Socket-free regression tests for the CodeQL compilation coverage guard.

Run with: python3 tools/test_check_codeql_coverage.py
Ninja only lists fixture commands; no compiler is executed.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import check_codeql_coverage as coverage


@unittest.skipUnless(shutil.which("ninja"), "Ninja is required to inspect fixture graphs")
class CoverageGuardTest(unittest.TestCase):
    def setUp(self) -> None:
        self.scratch = tempfile.TemporaryDirectory(prefix="codeql-coverage-test-")
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name).resolve()
        self.build = self.root / "build"
        self.build.mkdir()
        self.output = self.root / "inventory"
        self.graph = self.build / "build.ninja"
        self.graph.write_text(
            "rule compile\n"
            "  command = fixture-cxx -color-diagnostics -coverage -c $in -o $out\n"
            "build app.o: compile ../src/app.cpp\n"
            "build duplicate.o: compile ../src/app.cpp\n"
            "build mic.o: compile ../src/mic.mm\n"
            "build legacy.o: compile ../src/legacy.cpp\n"
            "build reference.o: compile ../tools/reference.c\n"
            "build test.o: compile ../tests/test.cpp\n"
            "build vendor.o: compile ../third_party/vendor.c\n"
            "build orphan.o: compile ../src/orphan.cpp\n"
            "build app: phony app.o mic.o\n"
            "build retained: phony legacy.o reference.o\n"
            "build all: phony app retained duplicate.o test.o vendor.o\n",
            encoding="utf-8",
        )

    def run_guard(self, *targets: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(Path(coverage.__file__).resolve()), str(self.build),
             *targets, "--source-root", str(self.root), "--output-dir", str(self.output)],
            text=True, capture_output=True, check=False,
        )

    def test_selected_graph_preserves_inputs_and_reduces_compilation(self) -> None:
        result = self.run_guard("app", "retained")
        self.assertEqual(result.returncode, 0, result.stderr)
        baseline = (self.output / "baseline-sources.txt").read_text().splitlines()
        selected = (self.output / "selected-sources.txt").read_text().splitlines()
        self.assertEqual(baseline, ["src/app.cpp", "src/legacy.cpp", "src/mic.mm", "tools/reference.c"])
        self.assertEqual(selected, baseline)
        counts = json.loads((self.output / "counts.json").read_text())
        self.assertEqual(counts["baseline_compile_invocations"], 7)
        self.assertEqual(counts["selected_compile_invocations"], 4)
        self.assertEqual(counts["missing_first_party_inputs"], 0)

    def test_removing_retained_target_fails_and_writes_missing_inputs(self) -> None:
        result = self.run_guard("app")
        self.assertEqual(result.returncode, 1, result.stderr)
        missing = (self.output / "missing-sources.txt").read_text().splitlines()
        self.assertEqual(missing, ["src/legacy.cpp", "tools/reference.c"])
        self.assertIn("src/legacy.cpp", result.stderr)
        self.assertIn("tools/reference.c", result.stderr)

    def test_objective_cpp_omission_fails(self) -> None:
        result = self.run_guard("app.o", "retained")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual((self.output / "missing-sources.txt").read_text(), "src/mic.mm\n")

    def test_target_outside_all_may_add_coverage(self) -> None:
        result = self.run_guard("app", "retained", "orphan.o")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("src/orphan.cpp", (self.output / "baseline-sources.txt").read_text())
        self.assertIn("src/orphan.cpp", (self.output / "selected-sources.txt").read_text())

    def test_unknown_target_fails(self) -> None:
        result = self.run_guard("missing-target")
        self.assertEqual(result.returncode, 2)
        self.assertIn("ninja command listing failed", result.stderr)

    def test_empty_first_party_graph_fails(self) -> None:
        self.graph.write_text("build all: phony\nbuild app: phony\n", encoding="utf-8")
        result = self.run_guard("app")
        self.assertEqual(result.returncode, 1)
        self.assertIn("no first-party", result.stderr)

    def test_test_only_selection_fails(self) -> None:
        result = self.run_guard("test.o")
        self.assertEqual(result.returncode, 1)
        self.assertIn("selected targets have no first-party", result.stderr)

    def test_malformed_command_fails_closed(self) -> None:
        self.graph.write_text(
            "rule compile\n  command = fixture-cxx -c 'unclosed\n"
            "build app.o: compile ../src/app.cpp\nbuild all: phony app.o\n",
            encoding="utf-8",
        )
        result = self.run_guard("app.o")
        self.assertEqual(result.returncode, 2)
        self.assertIn("cannot parse Ninja command", result.stderr)


class CompilerInputTest(unittest.TestCase):
    def test_quoted_path_and_exact_compile_flag(self) -> None:
        build = Path(tempfile.gettempdir()).resolve() / "coverage-build"
        command = 'fixture-cxx -color-diagnostics -coverage -c "../src/a space.cpp" -o app.o'
        self.assertEqual(coverage.compiler_input(command, build),
                         (build / "../src/a space.cpp").resolve())

    def test_objective_c_suffixes(self) -> None:
        build = Path(tempfile.gettempdir()).resolve()
        for suffix in (".m", ".mm"):
            with self.subTest(suffix=suffix):
                self.assertEqual(coverage.compiler_input(f"fixture-cc -c source{suffix}", build),
                                 build / f"source{suffix}")


if __name__ == "__main__":
    unittest.main()
