# NVIDIA BNR — GPU AI Noise Removal

BNR is AetherSDR's NVIDIA-powered client-side noise-removal engine, built on the
NVIDIA Maxine denoiser. It lives behind the **BNR** button in the AetherDSP
panel alongside the other client NR modules (NR2 / NR4 / DFNR / RN2 / MNR), and
is mutually exclusive with them.

**One button, two backends.** The panel below the button row picks how BNR runs:

| Backend | Runs | Needs a local NVIDIA GPU? | Container? |
|---|---|---|---|
| **Local (AFX)** | In-process, in AetherSDR | **Yes** (RTX/GeForce, Turing+) | No |
| **Service (NIM)** | A Maxine BNR microservice over gRPC | No — the service can be on another machine | Yes (the service is a container) |

Both apply the same Maxine denoiser to the 24 kHz RX audio. Because the **NIM**
backend is just a gRPC client, a user with no NVIDIA GPU can still use BNR by
pointing it at a service running on a different machine — so BNR is **always
compiled in**, on every platform.

---

## Local (AFX)

Runs the denoiser engine directly inside AetherSDR on the local NVIDIA GPU
(lowest latency, no service to manage). Requires an NVIDIA RTX / GeForce GPU
(Turing or newer) and is **Linux-only** (built with `-DENABLE_NVIDIA_AFX=ON`).

### Download-on-demand

The AFX runtime (the NVIDIA AFX libs, the CUDA/TensorRT runtime, and the
per-GPU denoiser model) is **not shipped** in the app — it's fetched on first
use via the **Download** button in the panel and cached under:

```
~/.local/share/AetherSDR/nvidia-afx/current/
```

The download is a **split** so AetherSDR hosts almost nothing:

- **CUDA libs** (cuBLAS / cuFFT / cuRT / nvRTC) come straight from **NVIDIA's
  PyPI wheels**, anonymously — pinned by version, with the wheel URL + sha256
  resolved from the PyPI JSON API at fetch time.
- The **AFX proprietary libs + TensorRT runtime libs + the per-arch denoiser
  model** come from a small (~335 MB for sm_89) `.tar.zst` published as a
  GitHub Release asset and pinned by sha256.

Total one-time download is ~1.2 GB; subsequent launches use the cache. The
arch is auto-detected (`nvidia-smi` compute capability → `sm_XX`).

> **Offline / air-gapped:** a pre-assembled pack can be imported instead of
> downloaded — see `NvidiaAfxPack::installFromFile()`.

### Intensity

The **Intensity** slider (0–100 %) maps to the AFX denoiser strength
(0 = passthrough, 1.0 = maximum). Persisted as `NvAfxIntensity`.

---

## Service (NIM)

A gRPC client to an **NVIDIA Maxine BNR NIM** microservice
(`MaxineBNR/EnhanceAudio`, bidirectional audio streaming).

- **Address** — `host:port` of the service (default `localhost:8001`). It can
  point at a container on this machine **or any remote host**.
- **Status** — live connection indicator (connecting / connected / not
  connected), with automatic retry.
- **Intensity** — maps to the service's `intensity_ratio`. Persisted as
  `BnrIntensity`; the address as `BnrAddress`.

Running the NIM container itself (Docker + the NVIDIA Container Toolkit + the
Maxine image) is outside AetherSDR — the app is purely the client. App-managed
container start/stop is a possible future addition.

---

## Behavior

- **Mutually exclusive** with NR2 / RN2 / NR4 / DFNR / MNR — enabling BNR
  disables the others and vice-versa. Switching the backend radio while BNR is
  on swaps backends live.
- **ADSP launcher** accents (green "active") whenever either backend is running.
- **Auto-disabled in digital/CW modes** (DIGU/DIGL/RTTY/CW/CWL) like the other
  speech denoisers — it would corrupt data / suppress CW tones.

## Build

- **NIM (gRPC)** is always compiled. grpc/protobuf are resolved via CMake
  config-mode (vcpkg on Windows, Homebrew on macOS) with a pkg-config fallback
  (Debian/Ubuntu system grpc). See `vcpkg.json`.
- **Local AFX** is gated by `-DENABLE_NVIDIA_AFX=ON` (Linux only). The wrapper
  declares its own minimal `NvAFX_*` API and **dlopen**s the downloaded runtime,
  so the app links none of the ~2 GB GPU stack.

## Source map

| Piece | File |
|---|---|
| Local AFX in-process filter | `src/core/NvidiaAfxFilter.{h,cpp}` |
| AFX download / cache / assemble | `src/core/NvidiaAfxPack.{h,cpp}` |
| NIM gRPC client | `src/core/NvidiaBnrFilter.{h,cpp}` + `src/core/proto/bnr.proto` |
| Panel (backend selector, download, intensity, status) | `src/gui/AetherDspWidget.cpp` (`buildBnrPage`) |
| RX NR selector + enable/exclusion | `src/core/AudioEngine.cpp` |
