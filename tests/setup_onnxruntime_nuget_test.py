"""Socket-free staging regressions using an injected package digest and download."""

import hashlib
import importlib.util
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile


spec = importlib.util.spec_from_file_location(
    "stage_onnxruntime", Path(__file__).resolve().parents[1] / "scripts/setup/_stage_onnxruntime_nuget.py")
stager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stager)


class NugetStagingTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.package = self.root / stager.PACKAGE
        self.destination = self.root / "third_party/onnxruntime"

    def package_fixture(self, omit=(), extra=None):
        entries = {"build/native/include/" + name: b"fixture header" for name in (
            "onnxruntime_cxx_api.h", "onnxruntime_cxx_inline.h", "onnxruntime_c_api.h")}
        entries.update({"LICENSE": b"fixture license", "ThirdPartyNotices.txt": b"fixture notices",
            "runtimes/win-x64/native/onnxruntime.lib": b"fixture import library",
            "runtimes/win-x64/native/onnxruntime.dll": b"fixture x64 DLL",
            "runtimes/win-x64/native/onnxruntime_providers_shared.dll": b"fixture provider DLL",
            "runtimes/osx-arm64/native/libonnxruntime.dylib": b"fixture ARM64 library",
            "runtimes/win-arm64/native/onnxruntime.dll": b"wrong architecture"})
        entries.update(extra or {})
        with zipfile.ZipFile(self.package, "w") as archive:
            for name, data in entries.items():
                if name not in omit:
                    archive.writestr(name, data)
        self.digest = hashlib.sha256(self.package.read_bytes()).hexdigest()

    def stage(self, platform):
        with patch.object(stager, "SHA256", self.digest):
            stager.stage(self.package, platform, self.destination)

    def test_windows_selects_only_x64_and_keeps_licenses(self):
        self.package_fixture()
        self.stage("win-x64")
        self.assertEqual((self.destination / "bin/onnxruntime.dll").read_bytes(), b"fixture x64 DLL")
        self.assertTrue((self.destination / "lib/onnxruntime.lib").is_file())
        self.assertEqual((self.destination / "ThirdPartyNotices.txt").read_bytes(), b"fixture notices")
        self.assertFalse((self.destination / "runtimes").exists())

    @unittest.skipIf(os.name == "nt", "macOS library alias requires symlink support")
    def test_mac_keeps_versioned_dylib_and_relative_alias(self):
        self.package_fixture()
        self.stage("osx-arm64")
        link = self.destination / "lib/libonnxruntime.dylib"
        self.assertTrue(link.is_symlink())
        self.assertEqual(link.read_bytes(), b"fixture ARM64 library")
        self.assertTrue((link.parent / f"libonnxruntime.{stager.VERSION}.dylib").is_file())
        self.assertEqual((link.parent / "libonnxruntime.1.dylib").read_bytes(), link.read_bytes())

    def test_tampered_cached_package_never_replaces_existing_stage(self):
        self.package_fixture()
        self.destination.mkdir(parents=True)
        marker = self.destination / "previous"
        marker.write_bytes(b"preserve")
        with self.package.open("ab") as stream:
            stream.write(b"tampered")
        with self.assertRaisesRegex(ValueError, "SHA256"):
            self.stage("win-x64")
        self.assertEqual(marker.read_bytes(), b"preserve")

    def test_missing_required_member_preserves_existing_stage(self):
        for member in ("LICENSE", "build/native/include/onnxruntime_c_api.h",
                       "runtimes/win-x64/native/onnxruntime.lib"):
            with self.subTest(member=member):
                self.package_fixture(omit=(member,))
                self.destination.mkdir(parents=True, exist_ok=True)
                marker = self.destination / "previous"
                marker.write_bytes(b"preserve")
                with self.assertRaises((ValueError, KeyError)):
                    self.stage("win-x64")
                self.assertEqual(marker.read_bytes(), b"preserve")

    def test_archive_paths_are_not_extracted(self):
        self.package_fixture(extra={"../../escape": b"bad", "build/native/include/../../escape.h": b"bad",
                                    "build/native/include/..\\escape.h": b"bad"})
        self.stage("win-x64")
        self.assertFalse((self.root / "escape").exists())
        self.assertEqual(sorted(path.name for path in (self.destination / "include").iterdir()),
                         ["onnxruntime_c_api.h", "onnxruntime_cxx_api.h", "onnxruntime_cxx_inline.h"])

    def test_oversized_member_is_refused_before_publication(self):
        self.package_fixture()
        with patch.object(stager, "MAX_BYTES", self.package.stat().st_size + 1):
            with patch.object(zipfile.ZipFile, "getinfo", return_value=type("Entry", (), {"file_size": 10**9})()):
                with self.assertRaisesRegex(ValueError, "member"):
                    self.stage("win-x64")
        self.assertFalse(self.destination.exists())

    def test_failed_publication_restores_previous_directory(self):
        self.package_fixture()
        self.destination.mkdir(parents=True)
        marker = self.destination / "previous"
        marker.write_bytes(b"preserve")
        original = stager.os.replace

        def replace(source, target):
            if Path(source).name == "prepared":
                raise OSError("injected publication failure")
            original(source, target)

        with patch.object(stager.os, "replace", side_effect=replace):
            with self.assertRaisesRegex(OSError, "publication"):
                self.stage("win-x64")
        self.assertEqual(marker.read_bytes(), b"preserve")

    def test_cached_download_is_always_verified_without_network(self):
        self.package_fixture()
        with patch.object(stager, "urlopen", side_effect=AssertionError("unexpected network")):
            with self.assertRaisesRegex(ValueError, "SHA256"):
                stager.download(self.root)
            with patch.object(stager, "SHA256", self.digest):
                self.assertEqual(stager.download(self.root), self.package)

    def response(self, payload):
        response = io.BytesIO(payload)
        response.status = 200
        response.url = stager.URL
        return response

    def test_verified_download_publishes_only_the_checked_cache_file(self):
        payload = b"download fixture"
        with patch.object(stager, "SHA256", hashlib.sha256(payload).hexdigest()):
            with patch.object(stager, "urlopen", return_value=self.response(payload)):
                self.assertEqual(stager.download(self.root), self.package)
        self.assertEqual(self.package.read_bytes(), payload)
        self.assertEqual(list(self.root.iterdir()), [self.package])

    def test_wrong_digest_or_oversized_download_leaves_no_partial_file(self):
        for oversized in (False, True):
            with self.subTest(oversized=oversized):
                with patch.object(stager, "MAX_BYTES", 1 if oversized else 100):
                    with patch.object(stager, "urlopen", return_value=self.response(b"untrusted download")):
                        with self.assertRaisesRegex(ValueError, "limit" if oversized else "SHA256"):
                            stager.download(self.root)
                self.assertEqual(list(self.root.iterdir()), [])

    def test_transport_failure_closes_partial_file_before_removing_it(self):
        opened = []
        original = stager.tempfile.NamedTemporaryFile
        unlink = Path.unlink

        def temporary(**kwargs):
            stream = original(**kwargs)
            opened.append(stream)
            return stream

        def remove(path, **kwargs):
            self.assertTrue(all(stream.closed for stream in opened))
            unlink(path, **kwargs)

        with patch.object(stager.tempfile, "NamedTemporaryFile", side_effect=temporary):
            with patch.object(stager, "urlopen", side_effect=OSError("injected transport failure")):
                with patch.object(Path, "unlink", remove):
                    with self.assertRaisesRegex(OSError, "transport"):
                        stager.download(self.root)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_non_https_response_is_not_cached(self):
        response = self.response(b"unexpected redirect")
        response.url = "http://example.invalid/package"
        with patch.object(stager, "urlopen", return_value=response):
            with self.assertRaisesRegex(ValueError, "HTTPS"):
                stager.download(self.root)
        self.assertEqual(list(self.root.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
