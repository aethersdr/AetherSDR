# tests/

Automated unit tests — `*_test.cpp` files compiled by CMake and run
in CI. To run the suite locally:

```sh
cmake -B build -S .
cmake --build build --target test
ctest --test-dir build --output-on-failure
```

Add a new test by dropping `<feature>_test.cpp` into this directory;
CMake picks it up automatically via the glob in the test-target block
of the top-level `CMakeLists.txt`.

**Not to be confused with [`/docs/qa/`](../docs/qa/)**, which holds
*manual* QA checklists and test plans — human procedures for features
that need a real radio to exercise. Different artifact, different
audience: that directory is for procedures; this one is for code.
