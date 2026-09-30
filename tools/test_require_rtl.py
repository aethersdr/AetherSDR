#!/usr/bin/env python3
"""Exercise release fail-closed versus deliberate optional developer builds."""
import itertools
from pathlib import Path
import subprocess
import tempfile
module = Path(__file__).resolve().parents[1] / "cmake/AetherRequireRtl.cmake"
with tempfile.TemporaryDirectory() as tmp:
    script = Path(tmp) / "probe.cmake"
    script.write_text(f'include("{module.as_posix()}")\naether_require_rtl()\n')
    for required, enabled, rtl, fftw in itertools.product((False, True), repeat=4):
        values = dict(REQUIRE_RTL=required, ENABLE_RTL=enabled, RTLSDR_FOUND=rtl, RTL_FFTW3F_FOUND=fftw)
        args = ["cmake"] + [f"-D{k}={'ON' if v else 'OFF'}" for k, v in values.items()]
        result = subprocess.run(args + ["-P", str(script)], capture_output=True, text=True)
        expected = not required or (enabled and rtl and fftw)
        assert (result.returncode == 0) == expected, (values, result.stdout, result.stderr)
print("16 RTL requirement combinations passed")
