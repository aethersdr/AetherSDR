# Compile-time build options

This is the reference for AetherSDR's project-defined CMake options. Start with
the [source-build instructions](README.md#building-from-source) for dependencies
and the [build guide](docs/BUILDING.md) for platform setup.

Defaults below apply to a fresh configuration. An existing build directory
keeps its cached choices, and release workflows can override the defaults.
An option set to `ON` requests a feature; missing dependencies or an unsupported
platform can still disable it. Read CMake's configure messages for availability.

## Setting and inspecting options

Pass Boolean switches as `-DNAME=ON` or `-DNAME=OFF` when configuring, then
rebuild. For example, request the RTL-SDR backend:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_RTL=ON
cmake --build build --parallel
```

Use a separate build directory for an experiment if you want to retain your
normal configuration. Print the cache values and help for an already configured
build without configuring or building it again:

```bash
cmake -LAH -N build
```

That cache also includes standard CMake settings, detected library paths and
vendored-library options. This guide lists AetherSDR's own selectable options
and additional project cache values. Changing them requires a source rebuild;
they are not runtime settings for a downloaded binary.

## Radio backends and decoder experiments

| Option | Default | Purpose and prerequisites |
|---|---|---|
| `ENABLE_DEEPFIST_EXPERIMENT` | OFF | Build the experimental DeepFist CW receive decoder. Requires ONNX Runtime at configure time and a separate verified model bundle at runtime; see below. |
| `ENABLE_RTL` | ON | Build the experimental receive-only RTL-SDR USB backend when both `librtlsdr` and single-precision FFTW (`fftw3f`) are found. Missing either disables the backend. |
| `AETHER_HL2_TX_TXA` | ON | Select WDSP's TXA chain for the Hermes-Lite 2 SSB transmit modulator. OFF builds the in-tree phasing modulator. This choice has no runtime toggle. |

**DeepFist:** enabling `-DENABLE_DEEPFIST_EXPERIMENT=ON` alone does not supply its
model. The configured model-download URL defaults to empty. For developer
qualification, the runtime environment variable `AETHER_DEEPFIST_MODEL_DIR`
can point to the exact verified bundle; it is not a CMake switch. The required
assets and distribution prerequisite are described in
[the DeepFist guide](docs/deepfist-cw-backend.md). ggmorse remains the default
CW receive decoder.

**Other radio families:** Flex, Hermes-Lite 2, Icom, ANAN-G2 and the synthetic
demo backend are included in normal builds without separate enable switches.
Choose the radio or demo mode when connecting. Hermes-Lite 2 and ANAN-G2 are
experimental; Icom support depends on the model. See
[Supported Hardware](README.md#supported-hardware) for the current coverage and
receive/transmit limits. Compiling a backend does not establish hardware support.

## Audio, digital voice, speech and display features

| Option | Default | Purpose and prerequisites |
|---|---|---|
| `ENABLE_RADE` | ON | RADE digital voice using the vendored RADE/Opus sources. |
| `ENABLE_DSTAR` | ON | Local D-STAR waveform helper using vendored smartsdr-dsp/ThumbDV support. |
| `ENABLE_SPECBLEACH` | ON | NR4 spectral noise reduction. MSVC builds also need `clang-cl`; otherwise CMake disables it. |
| `ENABLE_DFNR` | ON | DFNR DeepFilterNet3 noise reduction. Run `scripts/setup/setup-deepfilter.sh` (Windows: `setup-deepfilter.ps1`) before configuring. A missing library disables it. |
| `ENABLE_NVIDIA_AFX` | ON | BNR NVIDIA Maxine AFX GPU denoiser wrapper on x86-64 Linux/Windows. Operation needs a compatible NVIDIA GPU and the runtime/model pack, downloaded on demand. |
| `ENABLE_ASR` | ON | On-device speech recognition through whisper.cpp. Speech model weights are downloaded separately. |
| `ENABLE_ASR_METAL` | ON | ASR Metal acceleration on macOS, when ASR is enabled. |
| `ENABLE_ASR_VULKAN` | ON | ASR Vulkan acceleration on non-macOS platforms, when ASR is enabled and the GPU build dependencies are found. |
| `ENABLE_ASR_METAL_PRECOMPILE` | ON | Precompile Metal kernels on macOS for vendored whisper builds. Needs the offline Metal toolchain; a missing compiler warns and falls back to shader source unless `REQUIRE_ASR_GPU` is ON. |
| `AETHER_GPU_SPECTRUM` | ON | QRhi spectrum/waterfall rendering. Needs Qt ShaderTools and private QtGui headers; missing private headers select the CPU build. See [GPU spectrum rendering](docs/BUILDING.md#gpu-spectrum-rendering). |
| `ENABLE_MQTT` | ON | MQTT client support using bundled libmosquitto or the selected system library. |
| `MQTT_TLS` | ON | MQTT TLS support. The bundled library uses OpenSSL when found; OFF omits TLS. |

## Packaging and dependency selection

| Option | Default | Purpose |
|---|---|---|
| `AETHER_USE_PINNED_QT` | ON | Prefer the cached release Qt when present. An explicit Qt path takes precedence. |
| `AETHER_FETCH_QT` | OFF | Run the Qt setup script at configure time if the pinned Qt is missing. This can download the Qt toolchain. |
| `AETHER_EMBED_DFNR_MODEL` | ON on Windows; OFF elsewhere | Embed the DFNR model payload in application resources instead of deploying a loose archive. Relevant when DFNR is built. |
| `ASR_USE_PREBUILT_WHISPER_GPU` | OFF | Windows fallback that consumes the pinned prebuilt whisper GPU pack instead of building the vendored sources. The pack predates local source patches; leave OFF for the normal source build. |
| `LOWER_CASE_BINARY_NAME` | OFF | Use a lowercase executable name on Linux. |
| `USE_SYSTEM_ZLIB` | OFF | Use system zlib instead of the bundled snapshot. |
| `USE_SYSTEM_MSPACK` | OFF | Use system libmspack instead of the bundled snapshot. |
| `USE_SYSTEM_LIBMOSQUITTO` | OFF | Use system libmosquitto when MQTT is enabled. |
| `USE_SYSTEM_RTMIDI` | OFF | Use system RtMidi instead of the bundled snapshot. |
| `USE_SYSTEM_LIBWHISPER` | OFF | Use system libwhisper for ASR. Its GPU support and shader packaging are determined by that library's build. |
| `USE_SYSTEM_SQLITE` | OFF | Use system sqlite3 instead of the bundled snapshot. |

The `USE_SYSTEM_*` choices are primarily for distro packaging. Dependencies
enabled in the build use bundled, pinned snapshots by default. Qt setup and
custom Qt paths are documented in [the build guide](docs/BUILDING.md).

## Required-dependency checks

These switches turn missing optional capabilities into configure errors. They
are useful for release builds that must include those capabilities.

| Option | Default | Fail configuration when… |
|---|---|---|
| `REQUIRE_SERIALPORT` | OFF | Qt SerialPort is missing. |
| `REQUIRE_KEYCHAIN` | OFF | QtKeychain is missing. |
| `REQUIRE_ASR_ONNX` | OFF | ONNX Runtime is missing for Silero VAD, speaker labeling and the signal classifier. |
| `REQUIRE_ASR_GPU` | OFF | No ASR GPU backend is built, system libwhisper's GPU support cannot be verified, or requested Metal precompilation cannot run. |
| `REQUIRE_ASR_SHERPA` | OFF | sherpa-onnx is missing for the non-whisper ASR backend. |

Some capabilities are enabled by dependency detection rather than a dedicated
enable switch: Qt SerialPort, Qt WebSockets, QtKeychain, PortAudio, USB HID
support, ONNX Runtime, sherpa-onnx and native Linux PipeWire support. Setup scripts for
ONNX Runtime and sherpa-onnx are under `scripts/setup/`. CMake generates compiler
definitions such as `HAVE_SERIALPORT`, `HAVE_WEBSOCKETS`, `HAVE_KEYCHAIN` and
`HAVE_ONNX`; these are outputs of configuration, not user-facing `-D` switches.

## Diagnostics and opt-in tests

These options are for development and qualification.

| Option | Default | Purpose and prerequisites |
|---|---|---|
| `RADE_WAV_TAP` | OFF | Write diagnostic WAV files at RADE transmit tap points when RADE is built. Output directory: `RADE_TAP_DIR`. |
| `AETHER_ENABLE_HL2_RECEIVER_CHURN_TEST` | OFF | Build/register the fake-EP6 receiver-churn test used by sanitizer runs. |
| `AETHER_ENABLE_HL2_TX_LOOPBACK_TEST` | OFF | Build/register HL2 transmit-loopback and DSP-readback tests against an external `hpsdrsim` peer. The loopback test keys the simulated transmitter; a missing simulator produces a skip. |
| `AETHER_ENABLE_HL2_SIGNAL_STOP_TEST` | OFF | Build the HL2 signal-stop child process. The loopback-UDP process test is registered on non-Windows hosts with Python 3. |
| `AETHER_ENABLE_RADAR_GL_TEST` | OFF | Build/register the native-GPU weather-radar texture test. Needs a real OpenGL 3.2 context. |

Additional cache values accept a value rather than ON/OFF:

| Setting | Default | Values and purpose |
|---|---|---|
| `AETHERSDR_SANITIZER` | `none` | `none`, `address`, `undefined`, `address,undefined`, or `thread`. Instruments the main CMake tree with a GNU-driver GCC/Clang build; MSVC and clang-cl are rejected. ExternalProject children need separate sanitizer flags. |
| `DEEPFIST_MODEL_BASE_URL` | Empty | Published, versioned HTTPS directory for the exact DeepFist assets. See the distribution prerequisite in [the DeepFist guide](docs/deepfist-cw-backend.md). |
| `RADE_TAP_DIR` | `<build-directory>/rade_taps` | Directory for RADE WAV diagnostics; available when RADE and its taps are enabled. |
| `AETHER_TEST_FFTW_TIMELIMIT` | `0.001` | Seconds FFTW may spend measuring each plan under test; an empty value allows unbounded measurement. |

## Keeping this reference current

Option definitions live in [CMakeLists.txt](CMakeLists.txt),
[cmake/AetherQtPin.cmake](cmake/AetherQtPin.cmake) and
[tests/tests.cmake](tests/tests.cmake). Update this reference in the same change
that adds, removes or changes a project option or its default.
