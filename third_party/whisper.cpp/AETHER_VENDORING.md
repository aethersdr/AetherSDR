# Vendored whisper.cpp — AetherSDR ASR engine

Upstream: <https://github.com/ggml-org/whisper.cpp>
Version: **1.9.1** — commit pinned in [`COMMIT`](COMMIT)
License: MIT (see [`LICENSE`](LICENSE)) — GPL-v3-compatible; attributed as entry
20 of the repo-root [`THIRD_PARTY_LICENSES`](../../THIRD_PARTY_LICENSES). The
upstream `LICENSE` covers the bundled `ggml/` tree too — upstream ships one MIT
file for both.

Keep this a pristine mirror: do **not** modify vendored sources in place unless
the change genuinely cannot live outside the tree. Two files currently do;
every one is recorded in [`AETHERSDR-PATCHES.md`](AETHERSDR-PATCHES.md), and
anything not listed there is a drift bug.

This is the ASR engine adopted in RFC #4333. Weights are **not** vendored — they
are downloaded on first enable (primary Hugging Face, fallback GitHub release
asset, SHA-256 pinned). See the RFC for the model-manager design.

## What was trimmed from the upstream tree

To keep the checkout small, only the **library** is vendored. Removed:

- Non-library top level: `examples/`, `tests/`, `bindings/`, `models/`,
  `media/`, `samples/`, `grammars/`, `ci/`, `scripts/`, `Makefile`,
  `CMakePresets.json`, `*.yml`, `build-xcframework.sh`, README variants.
- **Most GPU / accelerator ggml backends** (not vendored):
  `ggml-cuda`, `ggml-hip`, `ggml-musa`, `ggml-webgpu`, `ggml-sycl`,
  `ggml-opencl`, `ggml-openvino`, `ggml-cann`, `ggml-rpc`,
  `ggml-virtgpu`, `ggml-zdnn`, `ggml-zendnn`, `ggml-hexagon`.

Kept: `include/`, `src/` (whisper), `cmake/`, and `ggml/` with the **CPU**
backend (`ggml-cpu`, plus the un-compiled `ggml-blas` source) **and the two GPU
backends** — `ggml-vulkan` (cross-platform NVIDIA/AMD/Intel) and `ggml-metal`
(native Apple). Each dropped backend is guarded by `if(GGML_<X>)` upstream, so
with those options OFF the missing directories are never referenced.

## GPU acceleration (Vulkan + Metal)

Each platform builds its native GPU backend, gated so machines/CI without the
toolchain build CPU-only:

- **Metal** (Apple): `ggml-metal`, enabled via `ENABLE_ASR_METAL` on `APPLE`.
  Uses the Metal framework + the `metal` shader compiler from full Xcode; no
  external SDK to install. Enabled by default on macOS.
- **Vulkan** (Linux/Windows): `ggml-vulkan`, enabled via `ENABLE_ASR_VULKAN`
  (auto-detected) only when the Vulkan loader+headers, the `glslc` shader
  compiler, and `SPIRV-Headers` are all present.

At runtime, `asrGpuAvailable()` (`ggml_backend_dev_by_type(GPU)`) decides whether
to use the GPU, so a GPU-enabled binary still runs on GPU-less hosts (CPU
fallback). `GGML_NATIVE=OFF` is forced for portable/Pi/CI binaries.

## Local patches (deviations from pristine upstream)

Two vendored files carry AetherSDR-local changes; the pristine-mirror rule above
holds for everything else. Both are described — with their rationale and the
refresh checklist — in [`AETHERSDR-PATCHES.md`](AETHERSDR-PATCHES.md), following
the same convention as `third_party/wdsp` and `third_party/smartsdr-dsp`:

- `ggml/src/ggml-metal/CMakeLists.txt` — build-time kernel compilation
  (`GGML_METAL_EMBED_LIBRARY_COMPILED`). PR #4553, fixes #4535.
- `ggml/src/ggml-metal/ggml-metal-device.m` — loads the embedded compiled
  metallib and clamps `props.has_tensor` / `props.has_bfloat` to the kernels it
  actually contains. PR #4553.

Both are kept as thin as possible: the *policy* around them — required
toolchain, missing-toolchain behaviour, deployment target, shader language
version — lives in the top-level `CMakeLists.txt` and reaches the vendored tree
only as `GGML_METAL_*` variables, so a refresh has less to re-apply.

They must be re-applied after any refresh; the re-vendoring recipe below
otherwise silently reverts them and reintroduces #4535.

## Re-vendoring / adding another GPU backend

To add a different GPU backend (CUDA, Metal, …), **re-copy that backend's
directory** from upstream at the pinned commit and turn its `GGML_<X>` option ON
(with the matching toolchain + CI runner). To refresh: clone upstream at
`COMMIT`, re-run the same trim (keeping `ggml-cpu`, `ggml-blas`, `ggml-vulkan`),
and diff — then re-apply the two local patches (see **Local patches** above); a
clean diff plus exactly those two files is the expected end state.
