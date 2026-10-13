# tests/

Automated unit tests — `*_test.cpp` files declared in `tests.cmake`,
compiled by CMake and run in CI. A few targets are opt-in (an `option()`
defaulting OFF) or manual (`EXCLUDE_FROM_ALL`); "Network-fixture boundary"
below lists them and how to enable each. To run the suite locally:

```sh
cmake -B build -S .
cmake --build build --target test
ctest --test-dir build --output-on-failure
```

## Adding a test

Drop `<feature>_test.cpp` into this directory, then declare its
`add_executable` + `add_test` in **[`tests/tests.cmake`](tests.cmake)** — *not*
in the top-level `CMakeLists.txt`, where these declarations used to live. There
is **no glob**: every test is declared explicitly, so copy the block of a
neighbouring target.

Write source paths **relative to the repository root**, exactly as you would
have in the root file — `tests/my_new_test.cpp` and `src/gui/Bar.cpp`, not
`my_new_test.cpp` and `../src/gui/Bar.cpp`. `tests.cmake` is pulled in with
`include()` rather than `add_subdirectory()` precisely so that stays true; the
header of that file explains why, and why it should not be "tidied up" into a
subdirectory later.

A test that touches `AppSettings` without linking `aethercore` adds
`$<TARGET_OBJECTS:aether_test_settings>` to its sources and needs
`aether_sqlite3` — add its target name to the `AETHER_SETTINGS_CONSUMERS` list
at the bottom of `tests.cmake`. `LogManager` and `AsyncLogWriter` come the same
way, from `aether_test_log_manager` and `aether_test_async_log_writer`. Each is
compiled once rather than once per test; configure fails if a test compiles
them from source again.

Putting a test target in the root `CMakeLists.txt` instead fails two ways, on
purpose: `tests.cmake` aborts the CMake configure step with a message pointing
here, and `tools/check_test_registration.py --strict` fails the PR in CI.

## Network-fixture boundary

Backend and automation behavior that depends on a local synthetic network peer
is retired from the default graph along three lines: deterministic protocol,
codec, model, and policy assertions stay socket-free in this suite; dropped
packets, refusals, disconnect edges, and TX interlocks are expressed as
injected transport/state-machine tests rather than socket fixtures; and
positive convergence against real firmware is proven through the automation
bridge and `radiocert`, which certify by effect and cannot prove a non-event.

Their deterministic checks run socket-free: `icom_session_lease_test`,
`icom_backend_seam_test`, `icom_civ_stall_test`, `icom_power_clamp_model_test`,
the `hl2_backend_*_seam`/`hl2_link_stats_*_seam`/`hl2_metis_link_counters`
tests, and `automation_boundary_core_test`/`automation_boundary_widgets_test`.

Three socket fixtures remain registered until their negative assertions have
socket-free replacements: `vkamp_connection_test` (bypass/antenna interlocks),
`automation_server_gesture_test` (TX-keying refusals and cleanup), and
`hl2_receiver_count_restart_test` (dropped Metis-start retry). RadioModel's
application of the IC-9700 band power ceiling is pinned by
`icom_power_clamp_model_test`.

Three HL2 tests are explicit rather than part of the default graph:

- A killed client must still release the radio: enable `hl2_signal_stop_test`
  with `-DAETHER_ENABLE_HL2_SIGNAL_STOP_TEST=ON`. Its Python driver binds an
  ephemeral loopback UDP port and runs a real `MetisClient` in a child
  process, so it is absent from the default graph and from every CI lane; it
  exits 77 if it cannot bind.

- Both weekly sanitizer lanes enable `hl2_receiver_churn_test` with
  `-DAETHER_ENABLE_HL2_RECEIVER_CHURN_TEST=ON` — TSan for the receiver-vector
  race, ASan for the use-after-free class — while a socket-free concurrency
  harness is designed.
- An operator running `./hpsdrsim -hermeslite2 -P1` may enable and run
  `hl2_tx_loopback_test` with `-DAETHER_ENABLE_HL2_TX_LOOPBACK_TEST=ON`.
  The test fingerprints the simulator before it can key. The weekly sanitizer
  lanes build it for compile coverage; without a simulator it skips honestly
  (exit 77, reported by ctest as Skipped).

Other targets outside the default graph:

- `weather_radar_texture_gl_test` needs a real OpenGL 3.2 context, which no CI
  lane has: enable it with `-DAETHER_ENABLE_RADAR_GL_TEST=ON` on a machine
  with a GPU.
- `rigctld_test`, `CAT_TS-2000_test` and `CAT_Flex_test` are manual clients
  for a running AetherSDR (CAT ports enabled for the CAT ones). They are
  `EXCLUDE_FROM_ALL` and never registered; build one by name, e.g.
  `cmake --build build --target rigctld_test`.

**Not to be confused with [`/docs/qa/`](../docs/qa/)**, which holds
*manual* QA checklists and test plans — human procedures for features
that need a real radio to exercise. Different artifact, different
audience: that directory is for procedures; this one is for code.
