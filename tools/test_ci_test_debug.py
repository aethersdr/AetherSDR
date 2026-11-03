#!/usr/bin/env python3
"""Configure real CMake targets to test CI strip scope, defaults and exclusions."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from unittest.mock import MagicMock, patch

import ci_linux_build

ROOT = Path(__file__).resolve().parents[1]


class TestCiDebugPolicy(unittest.TestCase):
    def configure(self, enabled=False, sanitizer=False, system="Linux"):
        text = (ROOT / "tests/tests.cmake").read_text()
        start = text.index("set(_aether_compile_flags ")
        end = text.index("# The check that makes the guarantees above", start)
        policy = text[start:end]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "main.cpp").write_text("int main() { return 0; }\n")
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.25)\n'
                'project(CiDebugPolicy LANGUAGES CXX)\n'
                'enable_testing()\n'
                'set(AETHER_SANITIZER_TIMEOUT_SCALE 4)\n'
                f'set(CMAKE_SYSTEM_NAME "{system}")\n'
                f'set(AETHERSDR_SANITIZER {"address" if sanitizer else "none"})\n'
                f'set(AETHER_CI_STRIP_TEST_DEBUG {"ON" if enabled else "OFF"} CACHE BOOL "")\n'
                'add_executable(production main.cpp)\n'
                'get_property(_aether_targets_before_tests DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)\n'
                'set(_aether_executables_declared_here first_test late_test)\n'
                'function(aether_assert_tests_retrofitted)\nendfunction()\n'
                + policy +
                '\nadd_executable(first_test main.cpp)\n'
                'set_property(TARGET first_test PROPERTY AETHER_TEST_NO_WISDOM_ISOLATION TRUE)\n'
                'add_test(NAME first_test COMMAND first_test)\n'
                'cmake_language(DEFER CALL aether_retrofit_tests)\n'
                'add_executable(late_test main.cpp)\n'
                'set_property(TARGET late_test PROPERTY AETHER_TEST_NO_WISDOM_ISOLATION TRUE)\n'
                'function(record)\n'
                'foreach(t production first_test late_test)\n'
                'get_target_property(opts ${t} LINK_OPTIONS)\n'
                'file(APPEND "${CMAKE_BINARY_DIR}/options.txt" "${t}=${opts}\\n")\n'
                'endforeach()\nendfunction()\n'
                'cmake_language(DEFER CALL record)\n')
            result = subprocess.run(["cmake", "-S", str(root), "-B", str(root / "build"),
                                     "-G", "Ninja"], text=True, capture_output=True)
            options = root / "build/options.txt"
            manifest = root / "build/ci-stripped-test-executables.txt"
            return result, options.read_text() if options.exists() else "", (
                manifest.read_text().splitlines() if manifest.exists() else [])

    def test_default_keeps_debug(self):
        result, options, manifest = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("--strip-debug", options)
        self.assertEqual(manifest, [])

    def test_only_tests_including_later_declarations_are_stripped(self):
        result, options, manifest = self.configure(enabled=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("production=LINKER:", options)
        self.assertIn("first_test=LINKER:--strip-debug", options)
        self.assertIn("late_test=LINKER:--strip-debug", options)
        self.assertEqual({Path(p).name for p in manifest}, {"first_test", "late_test"})

    def test_sanitizers_keep_debug_by_default(self):
        result, options, _ = self.configure(sanitizer=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("--strip-debug", options)

    def test_explicit_strip_rejects_sanitizers(self):
        result, _, _ = self.configure(enabled=True, sanitizer=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("uninstrumented Linux", result.stdout + result.stderr)

    def test_explicit_strip_rejects_other_platforms(self):
        result, _, _ = self.configure(enabled=True, system="Darwin")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("uninstrumented Linux", result.stdout + result.stderr)


class TestDiskFailureReporting(unittest.TestCase):
    def test_workflow_build_invocation_preserves_shell_locale(self):
        # ccache hashes LC_CTYPE. Exercise the actual workflow invocation in a
        # clean C environment, including an explicitly selected locale.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "tools").mkdir()
            (root / "tools/ci_linux_build.py").write_text(
                'import os\nprint(os.environ.get("LC_CTYPE", "UNSET"))\n')
            for workflow in ("ci.yml", "full-suite.yml"):
                text = (ROOT / ".github/workflows" / workflow).read_text()
                command = next(line.strip() for line in text.splitlines()
                               if line.strip().endswith("python3 tools/ci_linux_build.py"))
                for selected in (None, "C"):
                    env = dict(os.environ, LANG="C")
                    for name in ("LC_ALL", "LC_CTYPE", "LC_MESSAGES", "PYTHONCOERCECLOCALE"):
                        env.pop(name, None)
                    if selected is not None:
                        env["LC_CTYPE"] = selected
                    with self.subTest(workflow=workflow, locale=selected):
                        result = subprocess.check_output(["sh", "-c", command],
                                                         cwd=root, env=env, text=True)
                        self.assertEqual(result.strip(), selected or "UNSET")

    def test_full_disk_still_reports_metrics_and_build_failure(self):
        child = MagicMock()
        child.wait.return_value = 7
        with (patch.object(Path, "read_text", return_value="1000"),
              patch.object(Path, "mkdir"),
              patch.object(Path, "write_text", side_effect=OSError(28, "No space left on device")),
              patch.object(ci_linux_build, "available_bytes", side_effect=[900, 700, 650]),
              patch.object(ci_linux_build.subprocess, "check_output", return_value="8"),
              patch.object(ci_linux_build.subprocess, "Popen", return_value=child),
              patch.object(ci_linux_build.subprocess, "run") as diagnostics,
              patch("builtins.print") as output):
            self.assertEqual(ci_linux_build.main(), 7)
        report = output.call_args_list[0].args[0]
        self.assertIn('"sampled_peak_growth_bytes": 350', report)
        self.assertIn('"build_exit_code": 7', report)
        self.assertEqual(diagnostics.call_count, 2)


if __name__ == "__main__":
    unittest.main()

