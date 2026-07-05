# aetherd step 1 dry run — `libaethercore` split rehearsal

**Date:** 2026-07-04 · **Platform proven:** Linux (Arch, GCC, Ninja,
RelWithDebInfo) · **Status:** throwaway rehearsal, nothing committed.
The modified `CMakeLists.txt` + two-file `AutomationServer` edit live in
the git worktree `worktree-agent-a1f77800bf9381e8b`
(`.claude/worktrees/agent-a1f77800bf9381e8b`) for the step-1 implementer
to consult; the worktree is disposable — this document is the durable
artifact.

## Verdict

**The split works**: `libaethercore.a` (235 MB, 277 objects — src/core +
src/models + vendored rnnoise/ggmorse/RADE/specbleach/mosquitto/rtmidi)
plus `AetherSDR` (363 MB, 173 objects — src/gui + main.cpp + qrc/shaders)
linking it. Full test suite passes: **56/56** (`QT_QPA_PLATFORM=offscreen`).
But the RFC's step-1 claim needs one correction: it is **95% CMake,
not 100%** — one source edit was unavoidable (below).

## The headline finding: the moc jumbo-TU trap

`AutomationServer.h` has an inline setter assigning into
`QPointer<ConnectionPanel>` with only a forward declaration. The monolith
builds this **by accident**: AUTOMOC aggregates every moc into one
`mocs_compilation.cpp`, and `moc_ConnectionPanel.cpp` happens to complete
the type earlier in that same TU, so the template instantiation at
end-of-TU succeeds. Split the targets and the type is never completed →
hard error. Fix: out-line the setter into the .cpp (+6/−1, behavior
identical).

Consequences:
- **Step 1 requires a (tiny) source change**, and the monolith cannot
  even detect this class of latent bug — expect more as boundaries
  tighten. GCC already warns (`-Wsfinae-incomplete`) about the same
  pattern core-internally in `RadioConnection`/`SmartCatSession`.
- The dry run left `AutomationServer.cpp`'s `gui/ConnectionPanel.h`
  include in place (EB1, tracked): the archive carries four **undefined
  `ConnectionPanel::automation*` symbols** (verified via `nm`) that only
  resolve because the panel is an exe object. The real step-1 PR must
  invert this: an `IConnectionAutomation` facade defined in core,
  implemented by `ConnectionPanel` in gui. Relocating AutomationServer
  wholesale to gui would also work but is wrong for aetherd — its
  model-facing half is the control-plane seed (RFC §4.1 scope note).

## What the real step-1 PR must contain

1. The mechanical CMake split (target lists, dep retargeting, warning
   flags over both targets) — see the worktree diff.
2. The `AutomationServer` out-lined setter + `IConnectionAutomation`
   dependency inversion (the only true refactor).
3. Judgment-call items, each a review point:
   - **All `HAVE_*` feature defines PUBLIC on aethercore** — gui TUs test
     every one (HAVE_MIDI/MQTT/RADE/HIDAPI/SERIALPORT/WEBSOCKETS/
     KEYCHAIN/NVIDIA_AFX/PIPEWIRE/SPECBLEACH). A missed one **compiles
     silently and drops UI features** — the nastiest failure mode here.
     Recommend a CI assert comparing define sets or built-in feature
     lists before/after.
   - `AETHER_GPU_SPECTRUM` must be PUBLIC too (AutomationServer's
     QRhiWidget grab path) — a GUI render flag reaching the engine is
     itself a boundary smell to fix when the class splits.
   - Third-party include dirs PUBLIC (gui includes `RtMidi.h`/`hidapi.h`
     directly); Qt6::SerialPort/WebSockets PUBLIC (gui uses them
     directly).
   - **qrc/shaders/DFNR resources stay on the exe** — rcc registration is
     process-global; putting resources in a static lib is a linker-elision
     trap.
   - `UlanziDialBackend.h` (header-only Q_OBJECT) must ride the engine
     target's sources or its metaobject vanishes.

## Dependency surfaces (measured, Linux)

- **libaethercore:** Qt6 Core, Concurrent, Gui (QImage/QColor —
  load-bearing everywhere), Network, Multimedia (AudioEngine),
  SerialPort, WebSockets, DBus, Qt6Keychain, **and Widgets** (the tracked
  legacy files; drops to Widgets-free once the step-1 relocations/splits
  land). Effectively the *entire* third-party pile: zlib, mspack,
  mosquitto+OpenSSL, opus/RADE, fftw3(+f), specbleach, DFNR, rnnoise,
  ggmorse, liquid-dsp, rtmidi+alsa, hidapi, portaudio, pipewire, modem
  libs.
- **AetherSDR exe:** just qgeoview, Qt6::GuiPrivate/ShaderTools (QRhi),
  dl — everything else arrives transitively. The exe surface is tiny,
  which is exactly what the RFC wants.

## Timings (32 threads, no ccache)

| Step | Time |
|---|---|
| Configure | 8.1 s |
| Full build (1,297 edges incl. tests) | ≈131 s |
| Null build | 0.07 s |
| Incremental, gui .cpp touched | 19.0 s (dominated by 363 MB debug link) |
| Incremental, core .cpp touched | 18.0 s (13.7 s compile+ar, 4.4 s relink) |

Full-build cost unchanged vs monolith (same TUs + one `ar`). The win is
**isolation** — gui edits no longer re-scan the 277-object engine, and a
future `aetherd` builds core without gui/QtPrivate/qgeoview — not raw
speed. Incrementals are link-dominated either way.

## Ranked risks for the move-window PR

1. EB1 dependency inversion (AutomationServer ↔ ConnectionPanel) —
   touches the automation surface agents themselves use; needs behavior
   tests.
2. PUBLIC/PRIVATE define misplacement — silent feature loss; add the CI
   define-set assert.
3. Widgets-in-engine relocations/splits (the tracked legacy set; e.g.
   ThemeManager splits into token resolution vs QApplication styling).
4. macOS/Windows platform blocks retargeted by reasoning only — this dry
   run proves Linux; CI matrix must prove the rest.
5. Latent moc-completeness landmines (`RadioConnection`,
   `SmartCatSession`) — any further target splitting can trip them.
6. Test-target drift — 70+ test executables compile core sources
   directly; they should eventually link libaethercore or they'll mask
   boundary regressions.

## Checker follow-ups from this run

- `QRhiWidget` added to `tools/check_engine_boundary.py`'s QtWidgets
  class set (AutomationServer.cpp includes it; the file was already
  tracked, the class name wasn't detected).
- Future ratchet idea once the split lands: `nm`-audit `libaethercore.a`
  in CI for undefined symbols in the `AetherSDR::` gui namespace.
