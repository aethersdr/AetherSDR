#!/usr/bin/env python3
"""Configure real CMake targets to test CI strip scope, defaults and exclusions."""
from pathlib import Path
import subprocess
import tempfile
import unittest

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


if __name__ == "__main__":
    unittest.main()

