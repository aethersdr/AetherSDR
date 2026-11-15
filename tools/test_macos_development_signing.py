#!/usr/bin/env python3
"""Socket-free build/signature checks; compiled fixture binaries are never run."""

from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest


MODULE = Path(__file__).resolve().parents[1] / "cmake/MacOSDevelopmentSigning.cmake"


@unittest.skipUnless(sys.platform == "darwin", "requires Apple's compiler and codesign")
class MacOSDevelopmentSigningTest(unittest.TestCase):
    def run_command(self, *command, check=True):
        result = subprocess.run(command, text=True, capture_output=True, check=False)
        if check:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def build_fixture(self, architecture, enabled):
        temporary = tempfile.TemporaryDirectory(prefix="aether signing ")
        self.addCleanup(temporary.cleanup)
        source = Path(temporary.name)
        build = source / "build"
        (source / "main.c").write_text("int main(void) { return 0; }\n")
        (source / "library.c").write_text("int fixture_value(void) { return 1; }\n")
        (source / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.25)
project(SigningFixture C)
include("{MODULE.as_posix()}")
add_executable(SigningFixture MACOSX_BUNDLE main.c)
set_target_properties(SigningFixture PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.aethersdr.SigningFixture")
add_executable(helper main.c)
set_target_properties(helper PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${{CMAKE_BINARY_DIR}}/SigningFixture.app/Contents/MacOS")
add_library(nested SHARED library.c)
add_dependencies(SigningFixture helper nested)
set(copied_library "$<TARGET_BUNDLE_CONTENT_DIR:SigningFixture>/Frameworks/libnested.dylib")
add_custom_command(TARGET SigningFixture POST_BUILD
    COMMAND "${{CMAKE_COMMAND}}" -E make_directory
        "$<TARGET_BUNDLE_CONTENT_DIR:SigningFixture>/Frameworks"
    COMMAND "${{CMAKE_COMMAND}}" -E copy "$<TARGET_FILE:nested>" "${{copied_library}}")
aether_sign_macos_development_bundle(SigningFixture "$<TARGET_FILE:helper>" "${{copied_library}}")
''')
        configure = ["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
                     f"-DCMAKE_OSX_ARCHITECTURES={architecture}"]
        if enabled:
            configure.append("-DAETHER_MACOS_ADHOC_SIGN=ON")
        self.run_command(*configure)
        self.run_command("cmake", "--build", str(build), "-j22")
        return build / "SigningFixture.app"

    def check_signed_bundle(self, architecture):
        bundle = self.build_fixture(architecture, enabled=True)
        for code in (bundle / "Contents/MacOS/helper",
                     bundle / "Contents/Frameworks/libnested.dylib", bundle):
            self.run_command("codesign", "--verify", "--deep", "--strict", str(code))
        signature = self.run_command("codesign", "--display", "--verbose=2", str(bundle))
        self.assertIn("Signature=adhoc", signature.stderr)
        with (bundle / "Contents/Info.plist").open("rb") as stream:
            identifier = plistlib.load(stream)["CFBundleIdentifier"]
        self.assertIn(f"Identifier={identifier}\n", signature.stderr)

    def test_signs_intel_nested_code_before_bundle(self):
        self.check_signed_bundle("x86_64")

    def test_signs_arm64_nested_code_before_bundle(self):
        self.check_signed_bundle("arm64")

    def test_default_leaves_production_signing_untouched(self):
        bundle = self.build_fixture("x86_64", enabled=False)
        self.assertFalse((bundle / "Contents/_CodeSignature").exists())
        result = self.run_command("codesign", "--display", str(bundle), check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not signed at all", result.stderr)


if __name__ == "__main__":
    unittest.main()
