# tests/

Automated unit tests — `*_test.cpp` files compiled by CMake and run
in CI. To run the suite locally:

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

A test that touches `AppSettings` compiles `${AETHER_SETTINGS_SOURCES}` and
needs `aether_sqlite3` — add its target name to the `AETHER_SETTINGS_CONSUMERS`
list at the bottom of `tests.cmake`.

Putting a test target in the root `CMakeLists.txt` instead fails two ways, on
purpose: `tests.cmake` aborts the CMake configure step with a message pointing
here, and `tools/check_test_registration.py --strict` fails the PR in CI.

**Not to be confused with [`/docs/qa/`](../docs/qa/)**, which holds
*manual* QA checklists and test plans — human procedures for features
that need a real radio to exercise. Different artifact, different
audience: that directory is for procedures; this one is for code.
