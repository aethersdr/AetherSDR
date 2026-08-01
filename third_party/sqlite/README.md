# SQLite amalgamation (vendored)

- **Version:** 3.53.4 (`SQLITE_VERSION` in `sqlite3.h`)
- **Source:** https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip
- **Zip SHA-256:** `1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d`
- **License:** public domain (https://www.sqlite.org/copyright.html)
- **RFC:** #4603 — client settings store (AppSettings → SQLite)

Only `sqlite3.c` + `sqlite3.h` are vendored (no shell, no extension header —
extension loading is compiled out). Compile options are set in the
`aether_sqlite3` target in the top-level `CMakeLists.txt`, not here.

## Updating

1. Download the new amalgamation zip from https://www.sqlite.org/download.html
2. Replace `sqlite3.c` / `sqlite3.h`, update the version + URL + SHA-256 above
3. Build + run the settings test suite (`ctest -R 'settings|app_settings'`)

Update for upstream CVEs affecting the library proper; routine version chasing
is not required. The only consumer is `src/core/SettingsDatabase.cpp` — nothing
else may include `sqlite3.h` directly (keeps the seam swappable and the
compile-option surface single-point).
