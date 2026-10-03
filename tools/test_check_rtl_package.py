#!/usr/bin/env python3
"""Socket-free package closure checks using injected dumpbin reports."""
from pathlib import Path
import tempfile
import unittest

from check_rtl_package import check_windows_runtime


class WindowsRuntimeTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.app = self.root / "AetherSDR.exe"
        self.app.touch()
        self.system = self.root / "System32"
        self.system.mkdir()
        (self.system / "KERNEL32.dll").touch()
        self.libraries = [self.root / name for name in
                          ("rtlsdr.dll", "libusb-1.0.dll", "libfftw3f-3.dll")]
        for path in self.libraries:
            path.touch()
        self.reports = {
            "rtlsdr.dll": "  LIBUSB-1.0.dll\n  pthreadVC3.dll\n  KERNEL32.dll\n",
            "libusb-1.0.dll": "  KERNEL32.dll\n  api-ms-win-crt-runtime-l1-1-0.dll\n",
            "libfftw3f-3.dll": "  KERNEL32.dll\n",
            "pthreadvc3.dll": "  helper.dll\n  KERNEL32.dll\n",
            "helper.dll": "  pthreadVC3.dll\n  KERNEL32.dll\n",
        }

    def check(self):
        return check_windows_runtime(self.app, self.libraries, self.system,
                                     lambda path: self.reports[path.name.lower()])

    def test_transitive_missing_dependency(self):
        with self.assertRaisesRegex(ValueError, "pthreadVC3.dll"):
            self.check()
        # A nested copy does not satisfy the executable's loader search path.
        nested = self.root / "unused"
        nested.mkdir()
        (nested / "pthreadVC3.dll").touch()
        with self.assertRaisesRegex(ValueError, "pthreadVC3.dll"):
            self.check()
        (self.root / "pthreadVC3.dll").touch()
        with self.assertRaisesRegex(ValueError, "helper.dll"):
            self.check()
        (self.root / "helper.dll").touch()
        self.assertEqual(len(self.check()), 5)  # Includes a cycle, visited once.

    def test_system_dependencies_need_no_package_copy(self):
        self.reports["rtlsdr.dll"] = "  libusb-1.0.dll\n  KERNEL32.dll\n"
        self.assertEqual(len(self.check()), 3)

    def test_unparseable_imports_fail_closed(self):
        self.reports["libfftw3f-3.dll"] = "unexpected dumpbin output"
        with self.assertRaisesRegex(ValueError, "No DLL imports"):
            self.check()

    def test_runtime_must_be_beside_executable(self):
        nested = self.root / "unused"
        nested.mkdir()
        self.libraries[0] = nested / "rtlsdr.dll"
        self.libraries[0].touch()
        with self.assertRaisesRegex(ValueError, "not beside"):
            self.check()


if __name__ == "__main__":
    unittest.main()
