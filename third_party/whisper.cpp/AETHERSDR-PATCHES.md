# AetherSDR patches to whisper.cpp / ggml

The source snapshot is pinned to ggml-org/whisper.cpp commit
`080bbbe85230f624f0b52127f1ae1218247989f9` (version 1.9.1, see [`COMMIT`](COMMIT)).
The tree is otherwise an exact — if trimmed — upstream snapshot; see
[`AETHER_VENDORING.md`](AETHER_VENDORING.md) for what was removed.

AetherSDR carries three local changes: two in the ggml Metal backend, from the
same fix (#4535, PR #4553), and one in the ggml CPU backend, a MinGW build fix
(#4406) landed separately.

1. `ggml/src/ggml-metal/CMakeLists.txt`: adds `GGML_METAL_EMBED_LIBRARY_COMPILED`.
   Upstream's `GGML_METAL_EMBED_LIBRARY` embeds the merged kernel **source** and
   lets the runtime compile it on first use. The new option compiles the merged
   source to a `.metallib` at build time (`xcrun metal | metallib`) and embeds
   that binary instead. The `XC_FLAGS` block was hoisted out of the `else()`
   branch so both build-time compile paths share it.

   Deliberately *not* here: whether the offline toolchain is required, what
   happens when it is missing, which deployment target and which shader language
   version to build for. Those are AetherSDR release policy and live in the
   top-level `CMakeLists.txt`; this file only consumes
   `GGML_METAL_EMBED_LIBRARY_COMPILED`, `GGML_METAL_MACOSX_VERSION_MIN`,
   `GGML_METAL_STD` and `GGML_METAL_EMBED_LIBRARY_NO_BF16`.
2. `ggml/src/ggml-metal/ggml-metal-device.m`: loads that embedded binary via
   `dispatch_data_create` + `newLibraryWithData:` (a no-op destructor block
   suppresses the copy — the payload is static `__DATA`), and clamps the device
   props to the kernels the prebuilt library actually contains:
   `props.has_tensor` is always cleared (the tensor API is not compiled in, and
   clearing it also skips upstream's tensor dummy-kernel probes, which are a
   second runtime-compile site), and `props.has_bfloat` is cleared under
   `GGML_METAL_EMBED_LIBRARY_NO_BF16` — set when the deployment target keeps the
   library below Metal 3.1, where `ggml-metal.metal` drops every bf16 kernel
   while the runtime device query would still answer `true`.

Both exist so Apple's **runtime** shader compiler (`newLibraryWithSource`) is
never invoked. It re-compiles on every cold-cache launch, and on Intel-GPU Macs
it can live-lock indefinitely and freeze the GUI — that is #4535, measured at no
completion in 75 minutes on a Radeon Pro 560X.

3. `ggml/src/ggml-cpu/ggml-cpu.c`: narrows the Windows-11 core-parking
   throttle guard in `ggml_thread_apply_priority()` from
   `#if _WIN32_WINNT >= 0x0602` to
   `#if defined(THREAD_POWER_THROTTLING_CURRENT_VERSION) && _WIN32_WINNT >= 0x0602`.

   Under MinGW-w64, `winbase.h` defines the `ThreadPowerThrottling` enum value
   (so the `SetThreadInformation(..., ThreadPowerThrottling, ...)` call below
   still resolves) but not the `THREAD_POWER_THROTTLING_STATE` struct or the
   `THREAD_POWER_THROTTLING_EXECUTION_SPEED` / `THREAD_POWER_THROTTLING_CURRENT_VERSION`
   macros this block fills in — those are MSVC-Windows-SDK-only in this
   toolchain's header set, so the bare `_WIN32_WINNT` guard fails to compile
   under MinGW GCC 13.1.0 (Qt's `mingw_64` kit):

   ```
   ggml-cpu.c:2533:10: error: request for member 'StateMask' in something not a structure or union
   ```

   MSVC (CI/installer path) is unaffected either way, since the macro the guard
   now also checks is already defined there. Feature-detecting the actually-
   missing macro, rather than gating on the toolchain (`_MSC_VER`), means the
   guard self-resolves the day mingw-w64 adds the struct, and also covers
   MinGW-clang. Under MinGW this only forgoes a Windows-11 core-parking
   performance hint — the unconditional `SetThreadPriority()` immediately below
   still applies, so it's a narrower optimization loss, not a correctness
   change.

   Originally proposed as PR #4406, which fixed the same compile error the
   same way but was closed as an in-place vendored edit before this file's
   exception convention existed for this tree. Re-landed once #4553 documented
   that convention here. Checked upstream at the time of #4406: the bare guard
   was (and remains, as of that check) present unfixed at HEAD of both
   `ggml-org/ggml` and `ggml-org/whisper.cpp`, with no existing issue or PR —
   so a `COMMIT` bump alone would not have picked up a fix.

## Refreshing

When refreshing whisper.cpp, first check whether upstream has adopted an
equivalent compiled-embed option for the two Metal changes; if it has, drop
the corresponding local patch in favour of it. Otherwise reapply both changes
and confirm with

```bash
ctest --test-dir build -R asr_gpu_probe_test -V
```

run with `AETHER_ASR_EXPECT_PRECOMPILED=1` on an Apple Silicon host — that
asserts a Metal device initialized *and* that ggml logged the precompiled
branch, which is what catches a silent reversion to the source embed.

For the `ggml-cpu.c` guard, first check whether the exact line still reads
`#if _WIN32_WINNT >= 0x0602` at `ggml_thread_apply_priority()` — if upstream
has since fixed this itself (feature-detect or otherwise), drop the local
patch. Otherwise reapply the guard change and confirm with a MinGW
configure + build (`cmake --build` from a MinGW-w64 Ninja toolchain); a
regression here only shows up as a MinGW compile failure, not a test failure,
since MSVC and non-Windows builds never exercise this branch.

The authoritative diff for any of these files is its git history
(`git log -p -- third_party/whisper.cpp/ggml/src/ggml-metal/<file>` or
`.../ggml-cpu/ggml-cpu.c`). No checked-in `.patch` copy is kept for any of
them: it would need hand-syncing on every edit, and its context would not
apply cleanly across an upstream bump anyway.
