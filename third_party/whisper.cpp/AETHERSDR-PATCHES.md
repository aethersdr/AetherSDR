# AetherSDR patches to whisper.cpp / ggml

The source snapshot is pinned to ggml-org/whisper.cpp commit
`080bbbe85230f624f0b52127f1ae1218247989f9` (version 1.9.1, see [`COMMIT`](COMMIT)).
The tree is otherwise an exact — if trimmed — upstream snapshot; see
[`AETHER_VENDORING.md`](AETHER_VENDORING.md) for what was removed.

AetherSDR carries two local changes, both in the ggml Metal backend and both
from the same fix (#4535, PR #4553):

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

## Refreshing

When refreshing whisper.cpp, first check whether upstream has adopted an
equivalent compiled-embed option; if it has, drop the corresponding local patch
in favour of it. Otherwise reapply both changes and confirm with

```bash
ctest --test-dir build -R asr_gpu_probe_test -V
```

run with `AETHER_ASR_EXPECT_PRECOMPILED=1` on an Apple Silicon host — that
asserts a Metal device initialized *and* that ggml logged the precompiled
branch, which is what catches a silent reversion to the source embed.

The authoritative diff for either file is its git history
(`git log -p -- third_party/whisper.cpp/ggml/src/ggml-metal/<file>`). No
checked-in `.patch` copy is kept: it would need hand-syncing on every edit, and
its context would not apply cleanly across an upstream bump anyway.
