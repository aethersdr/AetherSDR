# SQLite amalgamation (vendored)

- **Version:** 3.53.4 (`SQLITE_VERSION` in `sqlite3.h`)
- **Source:** https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip
- **Zip SHA-256:** `1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d`
- **License:** public domain (https://www.sqlite.org/copyright.html)
- **RFC:** #4603 — client settings store (AppSettings → SQLite)

Only `sqlite3.c` + `sqlite3.h` are vendored (no shell, no extension header —
extension loading is compiled out). Compile options are set in the
`aether_sqlite3` target in the top-level `CMakeLists.txt`, not here.

## Using a system SQLite

`-DUSE_SYSTEM_SQLITE=ON` links against the distro `libsqlite3` via pkg-config
instead of the amalgamation here. **Minimum version 3.33.0** — `SettingsDatabase`
uses `sqlite_schema` (3.33.0) and `VACUUM INTO` (3.27.0); the pkg-config floor
enforces it, so an older distro fails at configure rather than at first launch.

A distro build does not carry the compile options set on the `aether_sqlite3`
target above. Two of them matter, and both are re-established at runtime in
`SettingsDatabase::open()` so the flag does not weaken the store:

| Vendored option | Effect | System-path equivalent |
|---|---|---|
| `SQLITE_DEFAULT_FILE_PERMISSIONS=0600` | store is owner-only | `QFile::setPermissions` on the db, `-wal` and `-shm` |
| `SQLITE_DQS=0` | double-quoted string literals rejected | `sqlite3_db_config(SQLITE_DBCONFIG_DQS_DML/DDL, 0)` |

`SQLITE_DEFAULT_WAL_SYNCHRONOUS=1` is already re-established by
`PRAGMA synchronous=NORMAL` on both paths, and `SQLITE_OMIT_LOAD_EXTENSION` is
defence-in-depth only — the C API gate defaults off upstream.

## Updating

1. Download the new amalgamation zip from https://www.sqlite.org/download.html
2. Replace `sqlite3.c` / `sqlite3.h`, update the version + URL + SHA-256 above
3. Build + run the settings test suite (`ctest -R 'settings|app_settings'`)

Update for upstream CVEs affecting the library proper; routine version chasing
is not required. The only consumer is `src/core/SettingsDatabase.cpp` — nothing
else may include `sqlite3.h` directly (keeps the seam swappable and the
compile-option surface single-point).
