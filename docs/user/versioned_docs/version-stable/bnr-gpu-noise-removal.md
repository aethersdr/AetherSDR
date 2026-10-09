---
title: "BNR GPU Noise Removal"
slug: "/bnr-gpu-noise-removal"
description: "BNR is AetherSDR's NVIDIA-powered receive noise remover."
status: "Supported"
applies_to: ["All radios"]
platforms: ["Linux", "Windows"]
---

:::info[Status]

**Status:** Supported · **Applies to:** All radios · **Platforms:** Linux, Windows

:::

BNR is AetherSDR's NVIDIA-powered receive noise remover. It runs the NVIDIA Maxine **Audio Effects (AFX)** denoiser **inside AetherSDR, on your local NVIDIA GPU** — no Docker, no container, no NGC account and no network service. It is one of the seven client-side noise-reduction methods described in [DSP Noise Mitigation](./dsp-noise-mitigation.md).

> **Upgrading from an older version:** earlier releases ran BNR in a Docker/NIM container reached over gRPC. That backend has been removed. You can stop and delete any old Maxine BNR container; it is no longer used.

## Requirements

| Requirement | Detail |
|---|---|
| **GPU** | NVIDIA GeForce **RTX 40-series or later** (Ada or newer) |
| **Operating system** | **Linux or Windows.** BNR is not available on macOS. |
| **Download** | A one-time download of about 1 GB (the NVIDIA runtime and the denoiser model for your GPU) |

The Linux and Windows release builds include BNR. On a Mac, or a PC without a supported NVIDIA GPU, use **DFNR** instead — it runs on any CPU.

If no runtime pack has been published for your GPU's architecture, BNR disables itself and says so ("No BNR pack for your GPU … — use DFNR") instead of showing an error.

## Setup

1. Open **Settings → AetherRX...** (or click the **AetherRX** button on the VFO flag's DSP tab) and go to the **AetherNR** tab.
2. Select **BNR** in the method row. The BNR page shows the **NVIDIA AFX** panel.
3. Click **Download (~1 GB)**. AetherSDR detects your GPU's architecture and fetches the matching pack, verifying every file by SHA-256. The **Installed components** list shows progress, then the installed versions.
4. The first time you turn BNR on, a one-time **NVIDIA Software License** dialog appears. Click **Accept** to use BNR; **Decline** leaves BNR off.

The download is cached, so later launches start straight away:

- **Linux:** `~/.local/share/AetherSDR/AetherSDR/nvidia-afx/current/`
- **Windows:** `%LOCALAPPDATA%\AetherSDR\AetherSDR\nvidia-afx\current\`

The doubled `AetherSDR` folder is expected.

On Linux the CUDA libraries come straight from NVIDIA's own PyPI distribution; the rest of the pack is a small AetherSDR release asset. On Windows the pack is a single self-contained zip.

When a newer pack is available the button reads **Update**; otherwise it offers **Re-download**.

## Using BNR

- **One client NR method at a time.** Turning BNR on turns off NR2, RN2, NR4, NNR, DFNR or MNR, and vice versa. BNR can be stacked with any radio-side filter (NR, NB, ANF and so on).
- **Voice only.** BNR switches itself off in CW, CWL, RTTY and DIGU/DIGL, where it would suppress CW tones or corrupt data.
- **Stereo.** BNR denoises the left and right channels independently, so pans and diversity reception keep each antenna in its own ear.
- The **AetherRX** launcher on the VFO flag lights up while BNR (or any client NR method) is running.
- Like every client-side method, BNR only affects audio played through your computer. It does not affect the radio's own headphone or speaker outputs.

## Reference

### Controls

| Control | Range | What it does |
|---|---|---|
| **Status** | — | Whether the runtime is installed, downloading, failed, or not available in this build |
| **Intensity** | 0–100 | Denoising strength. 0 passes audio through untouched; 100 is maximum. Remembered across restarts. |
| **Download / Update / Re-download** | — | Fetches or refreshes the runtime pack |

### Licensing

BNR uses NVIDIA software and an NVIDIA denoiser model, governed by the NVIDIA Software License Agreement, the Product-Specific Terms for NVIDIA AI Products and the NVIDIA Community Model License. They are licensed for use on NVIDIA RTX / GeForce RTX GPUs on a single-user PC or workstation. The full license texts are included in the downloaded pack (`licenses/`).

## Troubleshooting

### BNR is greyed out: "BNR requires an NVIDIA RTX 40-series or later GPU"

Your computer has no NVIDIA GPU, or the GPU is older than the RTX 40-series.

1. Use **DFNR** instead; it runs on any CPU.

### BNR is greyed out: "No BNR pack for your GPU … yet"

Your GPU is new enough, but no runtime pack has been published for its architecture.

1. Use **DFNR** instead.

### No BNR option, or "Not available in this build"

You are on macOS, or using a build compiled without BNR (`-DENABLE_NVIDIA_AFX=OFF`).

1. Use **DFNR** instead, or use a Linux or Windows release build.

### The download fails

The connection dropped, or there isn't enough free disk space for the pack.

1. Check your internet connection.
2. Make sure about 1 GB of disk space is free.
3. Click the **Download** button again.

### BNR turns off when you change mode

This is expected. BNR is voice-only and switches off in CW, CWL, RTTY and DIGU/DIGL.

1. Switch back to a voice mode.

### Audio sounds hollow or robotic

BNR is denoising too hard, or a radio-side NR stacked with it is adding its own artefacts.

1. Lower **Intensity**.
2. Lighten any radio-side NR stacked with BNR.

## See also

- [DSP Noise Mitigation](./dsp-noise-mitigation.md) — all noise-reduction methods compared
- [Aetherial Audio](./aetherial-audio.md)
- Developer reference: [docs/nvidia-bnr.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/nvidia-bnr.md)
