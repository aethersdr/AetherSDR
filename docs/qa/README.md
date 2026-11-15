# docs/qa/

Manual QA procedures — checklists and test plans for features that
don't have full automated coverage and need a human at a keyboard
with a real radio to verify.

- [`audio-test-plan.md`](audio-test-plan.md) — end-to-end RX/TX
  audio pipeline verification across modes, sample rates, and DSP
  toggles.
- [`profile-import-export-checklist.md`](profile-import-export-checklist.md)
  — manual QA for FlexRadio `.ssdr_cfg` profile import/export.

**Not to be confused with [`/tests/`](../../tests/)**, which holds
automated unit-test source code (`*_test.cpp` files compiled by CMake
and run in CI). Different artifact, different audience: this directory
is for procedures; `/tests/` is for code.
