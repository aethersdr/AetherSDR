# tests/

Automated unit tests — `*_test.cpp` files compiled by CMake and run
in CI. To run the suite locally:

```sh
cmake -B build -S .
cmake --build build --target test
ctest --test-dir build --output-on-failure
```

Add a new test by dropping `<feature>_test.cpp` into this directory and
declaring its `add_executable` + `add_test` in the test-target block of the
top-level `CMakeLists.txt` (there is **no glob** — every test is declared
explicitly; copy the pattern of a neighboring target). A test that touches
`AppSettings` compiles `${AETHER_SETTINGS_SOURCES}` and needs
`aether_sqlite3` in the `AETHER_SETTINGS_CONSUMERS` list at the bottom of
`CMakeLists.txt`.

**Not to be confused with [`/docs/qa/`](../docs/qa/)**, which holds
*manual* QA checklists and test plans — human procedures for features
that need a real radio to exercise. Different artifact, different
audience: that directory is for procedures; this one is for code.
