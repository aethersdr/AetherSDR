# RTL release dependency contract

Official AppImage (both architectures), macOS DMG (both architectures), and
Windows installer/portable/MSIX pipelines configure `ENABLE_RTL=ON` and
`REQUIRE_RTL=ON`. Missing librtlsdr or float FFTW is a configure error. An
intentional developer-only build can still set `ENABLE_RTL=OFF` with the
requirement left OFF.

`scripts/setup/rtl-dependencies.json` pins RTL-SDR Blog (the existing qualified
Blog V4 driver variant), libusb, and FFTW sources by URL and SHA256. Unix setup
builds shared RTL/libusb; AppImage also builds float FFTW. macOS builds double
and float FFTW at its declared deployment floor. Windows builds RTL/libusb
with MSVC and prepares the official double/float FFTW DLLs and import libs.
The pthreads source/build is a Windows upstream configure prerequisite; it is
not an imported runtime of the library-only RTL target.

Setup retains corresponding archives, copyright notices and the actual build
recipes in `share/aethersdr-rtl-sources`. Those travel inside each payload.
Source archives include their original license/copyright files.
No setup script installs a USB driver, changes udev rules, or opens a radio.

The pinned Blog source receives `rtl-async-cancellation.patch` before compiling.
Its SHA256 is part of `rtl-dependencies.json`, so release dependency caches
change with the patch. `apply-rtl-patches.py` verifies that digest and refuses
a source tree that cannot accept the patch. Setup ships the patch and helper
with the corresponding source/build recipes.

Async acquisition owns each submitted transfer until its terminal callback.
Cancellation requests, `LIBUSB_ERROR_NOT_FOUND` (also returned for an already
cancelling transfer), device loss and event errors never authorize early free.
Callbacks stop resubmitting once cancellation begins, and acquisition drains
all submitted transfers before returning to hardware control or teardown.
If a transport cannot deliver its terminal callbacks, acquisition retains the
transfers; the worker's bounded stop can then retire without destroying live
USB state.

`tools/check_rtl_package.py` checks that the app imports RTL and float FFTW,
that RTL imports libusb, and that those libraries exist in the payload. Mac
checks reject external non-system load paths; Windows requires these DLLs
beside the executable and recursively resolves their normal and delay DLL
imports from that same directory or Windows system libraries. A dependency
hidden elsewhere in the staging tree or available only on a build-tool PATH
does not satisfy the check. The existing complete deployment audit also runs.
The AppImage pipeline also extracts the completed image after the final
linuxdeploy pass, reruns the RTL dependency check on that payload and verifies
its libusb bytes against the final AppDir. This is packaging evidence, not
physical USB or RF qualification. CI builds
and tests that only upload logs are not release producers and keep optional
RTL behavior. Dependency-only build workflows do not build AetherSDR.
