---
title: "GPU Rendering"
slug: "/gpu-rendering"
description: "AetherSDR draws the spectrum and waterfall on the graphics card through Qt's QRhi layer, using each platform's native graphics API."
status: "Supported"
applies_to: ["All radios"]
platforms: ["Windows", "Linux", "macOS (Apple Silicon)"]
---

:::info[Status]

**Status:** Supported · **Applies to:** All radios · **Platforms:** Windows, Linux, macOS (Apple Silicon)

:::

AetherSDR draws the spectrum and waterfall on the graphics card through Qt's QRhi layer, using each platform's native graphics API. This keeps the display smooth at high frame rates while leaving the CPU free for audio and DSP. No setup is needed: the right backend is chosen at startup.

## Requirements

- Every AetherSDR build requires **Qt 6.12**, which always includes GPU rendering support.
- Whether a particular binary uses the GPU is decided **when it is built**: the `AETHER_GPU_SPECTRUM` build option (on by default) selects the GPU renderer or the CPU `QPainter` renderer. The CPU renderer is a build-time alternative, **not** an automatic fallback at runtime. Source builds also need Qt's private GUI headers; without them CMake turns the option off.

For build dependencies see [Building from Source](./building-from-source.md).

## Using GPU rendering

### Choosing the GPU

On computers with more than one graphics adapter, the panadapter **Display** panel has a **SYSTEM → GPU:** selector.

- The choice takes effect on the next launch ("Restart to apply").
- On Windows hybrid-graphics laptops the discrete GPU is used by default. The integrated GPU is listed but greyed out ("disabled (#1921)") because it crashes when a panadapter is popped out or docked.
- Adapters whose selection path hasn't been tested on real hardware are marked "(experimental)".
- Selecting the discrete GPU on a hybrid laptop also works under Wayland on Linux.

### Heat Map mode

**Display → Heat Map** (on by default) colours the spectrum trace and fill by signal strength, so strong signals stand out. Turn it off for a single solid colour chosen with the **FFT Fill** colour button, with the fill slider setting its opacity.

### Forcing software rendering (`AETHER_NO_GPU`)

If your GPU or driver draws the spectrum wrongly, force software OpenGL without rebuilding:

```bash
AETHER_NO_GPU=1 ./AetherSDR-*.AppImage
```

This is worth trying first on a Raspberry Pi or another system whose Mesa driver is newer than its hardware. On Windows, `AETHER_NO_GPU=1` (or `QT_OPENGL=software`) switches every panadapter to software OpenGL instead of Direct3D 11.

### Wayland and XWayland (Linux)

On a Wayland session with a display connected, AetherSDR uses native Wayland when it can and XWayland otherwise. On a headless Wayland session (VNC or wayvnc with no monitor) it starts on XWayland automatically. To override:

```bash
QT_QPA_PLATFORM=xcb ./AetherSDR-*.AppImage            # force XWayland
QT_QPA_PLATFORM='wayland;xcb' ./AetherSDR-*.AppImage  # force native Wayland
```

A value you set yourself always wins.

## Reference

### Platform backends

| Platform | Backend | Notes |
|----------|---------|-------|
| Windows | Direct3D 11 | `AETHER_NO_GPU` switches to software OpenGL (below). |
| macOS (Apple Silicon) | Metal | |
| macOS (Intel) | — | The Intel DMG is deliberately built with the CPU renderer, because the GPU path misbehaves on older Metal/OpenGL hardware. |
| Linux x86_64 | OpenGL | Mesa or vendor drivers. |
| Linux aarch64 (Raspberry Pi and other ARM) | OpenGL | The aarch64 AppImage renders on the GPU and needs glibc 2.38 or newer (Raspberry Pi OS Trixie; Bookworm can't run it). The 3D view works on the Raspberry Pi 5. |

**Help → About AetherSDR** shows which renderer is in use. If the "GPU" is really a software rasteriser (llvmpipe, WARP, SwiftShader and similar), the renderer line says `CPU QRhi (…)` instead of `GPU QRhi (…)`; the display is correct, just slower.

### How it works

- **Waterfall** — a GPU texture used as a ring buffer. Each new row uploads one scanline; scrolling is done in the shader, with no memory copies.
- **Spectrum trace** — drawn per pixel on the GPU from a single column texture of FFT data, so there is no per-frame vertex rebuild on the CPU. Trace width is the **FFT Line** setting (default 1.0 px).
- **Overlays** — grid, scales, band plan, passbands, slice markers, notches and spots are painted once into a cached texture and only repainted when something changes (retune, zoom, resize, mode change).
- Repaints are coalesced to at most one per 16 ms, so the display tops out at about **60 fps**. The spectrum rate itself is **Display → FFT FPS** (5–60, default 25).
- The **3D Stacked Trace** view (see [Panadapter Controls](./panadapter-controls.md)) also runs on the GPU.

## Known issues

- On Linux under native Wayland, the GPU spectrum can freeze the whole window at startup; it happens on some launches and not others ([#4704](https://github.com/aethersdr/AetherSDR/issues/4704)).
- On some Linux Wayland desktops the panadapter and waterfall update very slowly ([#4725](https://github.com/aethersdr/AetherSDR/issues/4725)).
- On some Linux systems (reported with an NVIDIA RTX 5080 on an X11 session) the spectrum and waterfall area stays empty and the connection window does not appear ([#4998](https://github.com/aethersdr/AetherSDR/issues/4998)).
- With the CPU renderer on a slow computer such as a Raspberry Pi 5, the **Heat Map** fill can use almost all of the main thread and leave the window unresponsive ([#5192](https://github.com/aethersdr/AetherSDR/issues/5192)).
- On Windows, popping out a panadapter has crashed AetherSDR with an older AMD Radeon HD 7000 series graphics card ([#5990](https://github.com/aethersdr/AetherSDR/issues/5990)).

## Troubleshooting

### A "Spectrum renderer unavailable" card covers the panadapter

The GPU renderer failed to start. The card names the reason, and the failure is also written to the log.

1. Quit AetherSDR and launch it with `AETHER_NO_GPU=1` (see [Forcing software rendering](#forcing-software-rendering-aether_no_gpu)).
2. On macOS the card suggests rebuilding with `-DAETHER_GPU_SPECTRUM=OFF` instead.

### The spectrum is blank but controls and audio work

QRhi could not start on this system: there is no usable OpenGL, Direct3D or Metal, as on some virtual machines and headless hosts.

1. Launch with `AETHER_NO_GPU=1`.
2. To see what Qt tried, launch with `QT_LOGGING_RULES="qt.rhi.*=true"` and look for the QRhi initialisation lines in the log.

### The panadapter is black under remote desktop on Linux

The session is running on native Wayland.

1. Launch with `QT_QPA_PLATFORM=xcb` to force XWayland (see [Wayland and XWayland](#wayland-and-xwayland-linux)).

### The display is correct but slow, and About says `CPU QRhi`

The graphics adapter AetherSDR found is a software rasteriser (llvmpipe, WARP, SwiftShader and similar), not a real GPU.

1. If your computer has more than one adapter, open **Display** and pick the real GPU under **SYSTEM → GPU:**.
2. Restart AetherSDR; the choice takes effect on the next launch.

## See also

- [Panadapter Controls](./panadapter-controls.md)
- [Troubleshooting](./troubleshooting.md)
- [Building from Source](./building-from-source.md)
- [Linux](./linux.md)
- [docs/BUILDING.md: GPU spectrum rendering](https://github.com/aethersdr/AetherSDR/blob/main/docs/BUILDING.md#gpu-spectrum-rendering)
