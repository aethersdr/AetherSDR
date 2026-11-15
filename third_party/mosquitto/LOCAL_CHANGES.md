# Local changes to vendored libmosquitto

Upstream: https://github.com/eclipse-mosquitto/mosquitto
Vendored version: libmosquitto 2.1.0

This file tracks every local modification we make to the vendored copy
so that future vendor refreshes can retire or reconcile patches
deliberately.

**Rule:** no undocumented modifications to `third_party/mosquitto/`.
Every patch goes in this file with:

- A short description of the change
- The AetherSDR issue/PR that introduced it
- The upstream issue/PR tracking the fix (file one if none exists)
- A retirement plan (when it's safe to drop the local patch)

---

## Patches

### Guard `openssl/engine.h` includes with `OPENSSL_NO_ENGINE`

- **AetherSDR:** [#1483](https://github.com/ten9876/AetherSDR/issues/1483) / [#1484](https://github.com/ten9876/AetherSDR/pull/1484)
- **Upstream:** [eclipse-mosquitto/mosquitto#3570](https://github.com/eclipse-mosquitto/mosquitto/issues/3570)
- **Files:** `src/net_mosq.c`, `src/options.c`, `src/tls_mosq.h`
- **Summary:** Wrap `#include <openssl/engine.h>` with
  `#if !defined(OPENSSL_NO_ENGINE)` so the build works against
  OpenSSL 3.5+ compiled with `no-engine` (e.g. Fedora 43's default
  OpenSSL build). The actual `ENGINE_*` function call sites were
  already correctly guarded; only the includes were not.
- **Retire when:** upstream merges a fix and we refresh the vendor drop
  to a release that contains it.
