#!/usr/bin/env python3
"""Socket-free package checks using injected binary inspection reports."""
from pathlib import Path
import sys
import tempfile
import unittest

from check_rtl_package import check_windows_runtime
from stage_rtl_appimage import stage_libusb


@unittest.skipIf(sys.platform == "win32", "AppImage staging requires POSIX symlinks")
class LinuxStagingTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.prefix = self.root / "pinned"
        self.libdir = self.prefix / "lib"
        self.libdir.mkdir(parents=True)
        self.appdir = self.root / "AppDir"
        (self.libdir / "librtlsdr.so.0").touch()
        self.payload = self.libdir / "libusb-1.0.so.0.5.0"
        self.payload.write_bytes(b"pinned libusb payload")
        self.soname = self.libdir / "libusb-1.0.so.0"
        self.soname.symlink_to(self.payload.name)
        self.reports = {
            "librtlsdr.so.0": " (NEEDED) Shared library: [libusb-1.0.so.0]\n",
            "libusb-1.0.so.0": " (SONAME) Library soname: [libusb-1.0.so.0]\n",
        }

    def stage(self):
        return stage_libusb(self.prefix, self.appdir,
                            lambda path: self.reports[path.name])

    def test_soname_copy_survives_removing_build_prefix(self):
        destination = self.stage()
        self.assertEqual(destination, self.appdir / "usr/lib/libusb-1.0.so.0")
        self.assertFalse(destination.is_symlink())
        self.soname.unlink()
        self.payload.unlink()
        self.assertEqual(destination.read_bytes(), b"pinned libusb payload")

    def test_missing_prefix_never_uses_host_library(self):
        host = self.root / "host"
        host.mkdir()
        (host / self.soname.name).write_bytes(b"host libusb payload")
        with self.assertRaisesRegex(ValueError, "Missing pinned RTL dependency"):
            stage_libusb(self.root / "absent", self.appdir,
                         lambda path: self.reports[path.name])
        self.assertFalse(self.appdir.exists())

    def test_missing_soname_is_not_satisfied_by_versioned_payload(self):
        self.soname.unlink()
        with self.assertRaisesRegex(ValueError, "Missing pinned RTL dependency"):
            self.stage()

    def test_source_must_stay_inside_pinned_prefix(self):
        host = self.root / "host-libusb.so"
        host.write_bytes(b"host libusb payload")
        self.soname.unlink()
        self.soname.symlink_to(host)
        with self.assertRaisesRegex(ValueError, "Missing pinned RTL dependency"):
            self.stage()

    def test_driver_and_library_sonames_must_match(self):
        for binary in self.reports:
            with self.subTest(binary=binary):
                original = self.reports[binary]
                self.reports[binary] = original.replace("libusb-1.0.so.0", "libusb-1.0.so.1")
                with self.assertRaisesRegex(ValueError, "must require|does not match"):
                    self.stage()
                self.reports[binary] = original
        self.assertFalse(self.appdir.exists())


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
