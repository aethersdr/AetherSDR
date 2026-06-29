# BNR / NVIDIA AFX — Windows continuation handoff

> **Purpose:** load this into a fresh Claude session on the Windows machine to continue the NVIDIA BNR work. It captures everything from the Linux session that produced the feature so far. This file lives on branch `feature/nvidia-afx-denoiser`; delete it before the PR merges.

---

## Who / project

- **User:** Jeremy, callsign **KK7GWY**. Owns a FLEX-8600. Primary dev box is Arch Linux; this continuation is on his **Windows dual-boot** (same laptop, **RTX 4090 Laptop GPU, compute capability 8.9 → `sm_89`**, NVIDIA arch name **"ada"**).
- **Project:** **AetherSDR** — a Linux-native (also Windows/macOS) Qt6/C++20 FlexRadio SDR client.
- **Repo / remote:** `aethersdr/AetherSDR` (the GitHub account `ten9876` is Jeremy/the team, not a third party).
- **Branch:** `feature/nvidia-afx-denoiser`. **PR #3902** is open against `main`.
- **Read `AGENTS.md`** in the repo for the full project guide/conventions (the canonical source; `CLAUDE.md` is a thin pointer to it).

## Working conventions (important)

- **Commit + push in one step**, no extra confirmation. Commit message footer line:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`
- **Test/compile every commit before proposing.** **Never open a PR until explicitly told** (PR #3902 is already open — pushing to the branch updates it).
- PR body footer: `🤖 Generated with [Claude Code](https://claude.com/claude-code)`
- **Never** create/push a `v*` tag without explicit instruction.
- **`pgrep`/check for a running AetherSDR before launching** — Jeremy often has his own instance up; a running app also re-saves settings on exit (see Reset below).
- Features must be **cross-platform** unless solving a platform-specific problem.
- Jeremy iterates fast: he'll say "rebuild so I can test", screenshots the UI, asks for tweaks. Build into **his** build dir and tell him the path; let him launch.

---

## The goal on Windows (Phase 2d — the ONLY thing left)

Everything else (the whole AFX-only BNR rework) is **done and CI-green on Linux/Windows/macOS**. What remains can ONLY be done on a Windows + NVIDIA machine:

1. **Build the Windows AFX-bits `.zip` asset** (the downloadable runtime pack).
2. **Validate** AFX actually loads + denoises on the Windows GPU — this exercises the Windows `LoadLibrary` DLL-resolution path, the whole reason for the dual-boot.
3. **Pin its sha256** into `kWinTarballSha` in `src/core/NvidiaAfxPack.cpp` (currently `""`).
4. **Publish** the zip to the GitHub release `afx-bits-2.1.0`.

---

## What the feature is (architecture)

**BNR** = the AetherDSP panel's NVIDIA noise-removal button. It runs the **NVIDIA Maxine Audio Effects (AFX) denoiser in-process** on a local NVIDIA RTX/GeForce GPU (Turing+). **No container, no NIM, no gRPC** (an earlier NIM/container backend was removed). macOS / non-NVIDIA fall back to DFNR.

The ~2 GB GPU runtime is **not shipped** — it's **downloaded on demand** and cached, then `dlopen`'d (Linux) / `LoadLibrary`'d (Windows) at runtime. The app links none of the GPU stack and vendors no NVIDIA headers (the filter declares its own minimal `NvAFX_*` API).

### Key source files
| File | Role |
|---|---|
| `src/core/NvidiaAfxFilter.{h,cpp}` | In-process denoiser. 24 kHz stereo ↔ 48 kHz mono. **Cross-platform load**: Linux `dlopen`/`dlsym`; Windows `LoadLibraryEx(LOAD_WITH_ALTERED_SEARCH_PATH)`/`GetProcAddress` + `SetDefaultDllDirectories`/`AddDllDirectory(packDir/bin)`. Same `NvAFX_*` contract. |
| `src/core/NvidiaAfxPack.{h,cpp}` | Download/cache/assemble + resume + receipt + update-check. Emits `planReady`/`componentProgress`/`componentFinished`/`finished`. |
| `src/gui/AetherDspWidget.{h,cpp}` | The BNR panel: per-component download rows, intensity, status, Download/Resume/Update button, license gate, `buildBnrPage()`/`updateBnrStatus()`. |
| `src/core/AudioEngine.{h,cpp}` | `setNvAfxEnabled`/`nvAfxEnabled`/`setNvAfxIntensity`, mutual exclusion with NR2/RN2/NR4/DFNR/MNR, digital/CW auto-disable. |
| `scripts/build/build-afx-bits-windows.ps1` | **Assembles the Windows pack zip** (see below). |
| `docs/nvidia-bnr.md` | Feature doc. |

### Build gating
`-DENABLE_NVIDIA_AFX=ON` compiles on Linux + Windows (defines `HAVE_NVIDIA_AFX`; macOS excluded). Already enabled in the `check-windows` CI job and `windows-installer.yml`. On Windows no extra link lib is needed (kernel32's LoadLibrary family).

---

## Windows pack design (what you're building)

**Self-contained `.zip`** (unlike Linux's split download). Windows resolves a DLL's deps from its own directory, so everything sits flat in `bin/`:

```
afx-bits-2.1.0-windows-x86_64-sm_89.zip
  bin/
    NVAudioEffects.dll              (AFX core — Windows name; Linux is libnv_audiofx.so)
    nvafx_denoiser*.dll             (denoiser feature DLL, from NGC)
    cublas64_12.dll cublasLt64_12.dll cufft64_11.dll nvrtc64_120_0.dll   (CUDA, from SDK)
    nvinfer_10.dll                  (TensorRT, from SDK)
    libcrypto-3-x64.dll             (OpenSSL, from SDK)
  features/denoiser/models/sm_89/denoiser_48k.trtpkg   (model, from NGC; app expects THIS path/name)
  licenses/                         (NVIDIA SWLA + product terms + model license PDFs)
  NOTICE.txt
```

- The app's `NvidiaAfxFilter` (Windows) loads `bin/NVAudioEffects.dll` with `LOAD_WITH_ALTERED_SEARCH_PATH` and pins `packDir/bin` on the DLL search path, so the core resolves its CUDA/TRT/feature siblings.
- `findModel()` looks for `features/denoiser/models/sm_XX/denoiser_48k.trtpkg` — the assembly script renames the NGC model file to exactly `denoiser_48k.trtpkg`.
- Manifest URL (in `NvidiaAfxPack::manifest`): `https://github.com/aethersdr/AetherSDR/releases/download/afx-bits-2.1.0/afx-bits-2.1.0-windows-x86_64-<arch>.zip`. Single component on Windows (no PyPI wheels).
- `kWinTarballSha` in `NvidiaAfxPack.cpp` is currently `""` (no verification) → **pin the real sha256 after building the zip**.

### Windows Maxine AFX SDK layout (from inspecting v2.1.0)
Download the **Windows** Maxine AFX SDK on the Windows box. It contains:
- `bin/NVAudioEffects.dll`, `lib/NVAudioEffects.lib`, `include/nvAudioEffects.h`
- `bin/external/cuda/bin/{cublas64_12,cublasLt64_12,cufft64_11,nvrtc64_120_0}.dll`
- `bin/external/nvtrt/bin/nvinfer_10.dll`
- `bin/external/openssl/bin/libcrypto-3-x64.dll`
- `features/download_features.ps1` — fetches the **denoiser feature DLL** (→ `<root>/bin`) and the **model** (→ `<root>/models/<arch>`) from NGC.

### NGC (for the feature DLL + model)
- Org **`nvidia`**, team **`maxine`**, `MODEL_VERSION = 2.1.0`, effect **`denoiser-48k`**, arch **`sm_89` → "ada"**.
- Requires NGC auth (an NGC API key). **Jeremy pasted an NGC key in the earlier chat — it should be rotated/deleted at https://org.ngc.nvidia.com/account/api-keys** if reused. `download_features.ps1` handles the auth flow on Windows.

---

## The assembly script

`scripts/build/build-afx-bits-windows.ps1` (already in the repo). It:
1. Runs the SDK's `download_features.ps1` for `denoiser-48k` / `ada` (NGC `nvidia/maxine`) to fetch the feature DLL + model.
2. Flattens `bin/NVAudioEffects.dll` + all `bin/external/*/bin/*.dll` + the feature DLL into the pack's `bin/`.
3. Places the model at `features/denoiser/models/sm_89/denoiser_48k.trtpkg`.
4. Bundles license PDFs + writes `NOTICE.txt`.
5. Zips to `afx-bits-2.1.0-windows-x86_64-sm_89.zip` and prints the **sha256**.

Run:
```powershell
# authenticate to NGC once (nvidia/maxine), then:
pwsh scripts/build/build-afx-bits-windows.ps1 -SdkDir C:\path\to\extracted\maxine-afx-sdk
```
**The script is unvalidated** (written on Linux). Expect to tweak it on first run — e.g. the exact feature DLL glob, the model filename, or the NGC variant/arch tag. Verify its output zip matches the layout above before trusting it.

---

## Step-by-step Phase 2d plan

1. `git pull` the `feature/nvidia-afx-denoiser` branch; build with `-DENABLE_NVIDIA_AFX=ON` (or grab the `windows-installer` CI artifact). `pgrep`-equiv check for a running instance first.
2. Download + extract the Windows Maxine AFX SDK; authenticate to NGC.
3. Run `build-afx-bits-windows.ps1`; fix any layout mismatches; get a valid zip + sha256.
4. **Test the pack BEFORE publishing:** extract it into the cache dir (see paths below) OR set `AETHER_NVAFX_DIR` to it, launch AetherSDR, open AetherDSP → **BNR**, accept the license, and confirm Status goes **● Active (green)** and audio is denoised. This validates the `LoadLibrary` sibling-DLL resolution — the key Windows unknown.
5. If it works: pin the sha256 into `kWinTarballSha`, commit+push, then **upload the asset**: `gh release upload afx-bits-2.1.0 afx-bits-2.1.0-windows-x86_64-sm_89.zip`.
6. Optionally test the in-app **Download** button end-to-end (now that the asset exists and the sha matches).

---

## Windows cache + reset (for testing)

**Cache path double-nests** because org == app == "AetherSDR", so `AppLocalDataLocation` on Windows is:
```
%LOCALAPPDATA%\AetherSDR\AetherSDR\nvidia-afx\
   current\        (installed pack)
   .staging\       (resumable partial download + .progress.json)
```
Settings file: `%APPDATA%\AetherSDR\AetherSDR.settings` (XML).

**Reset BNR to first-run** (app MUST be closed first — a running app re-saves on exit): delete the `nvidia-afx` dir, and in the settings XML remove `<BnrNvidiaLicenseAccepted>`, `<LastClientNr>`, `<NvAfxIntensity>` (+ any legacy `<BnrBackend>`), and delete `AetherSDR.settings.bak`.

---

## State / reference facts

- **CI:** PR #3902 is green on `build` (Linux), `check-windows`, `check-macos` as of HEAD `407ca542`.
- **Stable component keys** (cache matching is keyed on these, NOT display names): `afx`, `cuda-runtime`, `cublas`, `cufft`, `nvrtc`. Display labels are free to change.
- **Linux** AFX-bits asset is published + working; its pinned sha256 is `0bfe85b0faeb322958303c145996350d0fea8a203899f9215fc0d3a341395b67`. CUDA libs on Linux come from NVIDIA's PyPI wheels (pinned: cuda-runtime 12.8.90, cublas 12.8.4.1, cufft 11.3.3.83, nvrtc 12.8.93); AFX/TRT/model from the tarball.
- **Verified on Linux (RTX 4090):** filter loads, builds the TensorRT engine, denoises (100% attenuation of pure noise), ~52× realtime; the full download→install→enable→denoise path + resume/caching + restart persistence all work.

## Not-blocking follow-ups (after Windows, or whenever)
- Refresh the **PR #3902 description** — it's stale (predates resume, update-check, per-component bars, stable keys, the crash/license fixes).
- Minor `docs/nvidia-bnr.md` polish for the new UX (resume, update check, per-component bars).
- Optional Linux edge checks not yet exercised: NR mutual-exclusion + digital/CW auto-disable; download failure/retry.

## Recent commit trail (newest first)
```
407ca542 ui(dsp): Status above Intensity, Download button bottom-left
f033a5d7 ui(dsp): green Active indicator when BNR is running
aeab3774 ui(dsp): widen BNR component column spacing to 24px
1d9c2037 fix(dsp): key the BNR download cache on a stable id, not the display name
2bffff7f ui(dsp): move BNR Intensity slider above the Status/Download row
... (earlier: per-component bars, resumable download, update check, crash fix,
    license-gate-on-download, NIM removal, Windows AFX backend port)
```
