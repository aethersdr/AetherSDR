# NVIDIA BNR — GPU AI Noise Removal

BNR is AetherSDR's NVIDIA-powered client-side noise-removal engine, built on the
NVIDIA Maxine **Audio Effects (AFX)** denoiser. It lives behind the **BNR**
button in the AetherDSP panel alongside the other client NR modules
(NR2 / NR4 / DFNR / RN2 / MNR), and is mutually exclusive with them.

**One button, in-process, no container.** BNR runs the Maxine denoiser directly
inside AetherSDR on your local NVIDIA GPU — lowest latency, nothing to install,
no Docker, no microservice. Requires an NVIDIA RTX / GeForce GPU (Turing or
newer) and is supported on **Linux and Windows** (built with
`-DENABLE_NVIDIA_AFX=ON`). On macOS or a machine with no supported NVIDIA GPU,
use **DFNR** (the bundled DeepFilterNet denoiser) instead.

> Earlier builds also offered a "Service (NIM)" gRPC backend that talked to a
> Maxine microservice in a container. That path was removed — ham operators
> shouldn't have to stand up a container to denoise audio — leaving the single
> local AFX path documented here.

---

## Download-on-demand

The AFX runtime (the NVIDIA AFX libs, the CUDA/TensorRT runtime, and the
per-GPU denoiser model) is **not shipped** in the app — it's fetched on first
use via the **Download** button in the panel and cached under the app data dir:

- **Linux:** `~/.local/share/AetherSDR/nvidia-afx/current/`
- **Windows:** `%LOCALAPPDATA%\AetherSDR\nvidia-afx\current\`

The arch is auto-detected (`nvidia-smi` compute capability → `sm_XX`), and the
matching pack is fetched as a GitHub Release asset pinned by sha256.

### Linux — split download

So AetherSDR hosts almost nothing:

- **CUDA libs** (cuBLAS / cuFFT / cuRT / nvRTC) come straight from **NVIDIA's
  PyPI wheels**, anonymously — pinned by version, with the wheel URL + sha256
  resolved from the PyPI JSON API at fetch time.
- The **AFX proprietary libs + TensorRT runtime libs + the per-arch denoiser
  model** come from a small (~335 MB for sm_89) `.tar.zst` published as a
  GitHub Release asset and pinned by sha256.

The core lib (`nvafx/lib/libnv_audiofx.so`) resolves its CUDA/TensorRT deps and
the denoiser feature lib via `DT_RPATH`; the feature lib is symlinked onto that
RPATH dir during install.

### Windows — single self-contained zip

Windows resolves a DLL's dependencies from its own directory, so the Windows
pack is one **self-contained `.zip`**: the AFX core (`bin/NVAudioEffects.dll`),
the CUDA + TensorRT DLLs, and the denoiser feature DLL all sit together in
`bin/`, with the model under `features/`. No PyPI wheels, no zstd tooling, no
symlinks — one download, extracted with the built-in `tar` (bsdtar reads zip).
At load time the pack's `bin/` is pinned on the process DLL search path
(`SetDefaultDllDirectories` + `AddDllDirectory`) and the core is loaded with
`LOAD_WITH_ALTERED_SEARCH_PATH`, so it finds its siblings.

Total one-time download is ~1.2 GB; subsequent launches use the cache.

> **Offline / air-gapped:** a pre-assembled pack can be imported instead of
> downloaded — see `NvidiaAfxPack::installFromFile()`.

## Intensity

The **Intensity** slider (0–100 %) maps to the AFX denoiser strength
(0 = passthrough, 1.0 = maximum). Persisted as `NvAfxIntensity`.

---

## Behavior

- **Mutually exclusive** with NR2 / RN2 / NR4 / DFNR / MNR — enabling BNR
  disables the others and vice-versa.
- **ADSP launcher** accents (green "active") whenever BNR is running.
- **Auto-disabled in digital/CW modes** (DIGU/DIGL/RTTY/CW/CWL) like the other
  speech denoisers — it would corrupt data / suppress CW tones.

## Licensing

BNR runs NVIDIA-provided software and a denoiser model. The first time BNR is
enabled, AetherSDR shows a one-time **NVIDIA license acceptance** dialog and
records it (`BnrNvidiaLicenseAccepted`); declining leaves BNR off. This flows
NVIDIA's terms down to the end user.

- **Software (AFX + TensorRT runtime libs)** — distributed as part of AetherSDR
  under the NVIDIA Software License Agreement + Product-Specific Terms for
  NVIDIA AI Products (which grant distribution as part of a Compatible
  Application). The required Works Notice ships in the pack's `NOTICE.txt`.
- **Denoiser model** — under the NVIDIA Community Model License (General
  Downloadable Grant); the license text is bundled in the pack's `licenses/`.
- **Scope** — licensed for use on **NVIDIA RTX / GeForce RTX GPUs on a
  single-user PC/workstation**.
- **CUDA libraries** — on Linux they're obtained at download time from NVIDIA's
  own PyPI distribution (AetherSDR does not redistribute them); on Windows the
  CUDA redistributable DLLs ride in the pack zip under NVIDIA's CUDA
  redistribution terms.

The downloaded pack carries the full NVIDIA license texts (`licenses/*.pdf`) and
`NOTICE.txt`.

## Build

- Gated by `-DENABLE_NVIDIA_AFX=ON`, supported on **Linux and Windows**. The
  wrapper declares its own minimal `NvAFX_*` API and loads the downloaded
  runtime at runtime (Linux `dlopen`, Windows `LoadLibrary`), so the app links
  none of the ~2 GB GPU stack. No NVIDIA headers are vendored.

## Source map

| Piece | File |
|---|---|
| In-process AFX filter (Linux + Windows) | `src/core/NvidiaAfxFilter.{h,cpp}` |
| AFX download / cache / assemble | `src/core/NvidiaAfxPack.{h,cpp}` |
| Panel (download, intensity, status) | `src/gui/AetherDspWidget.cpp` (`buildBnrPage`) |
