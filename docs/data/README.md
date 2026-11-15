# docs/data/

Reference data fixtures used during development:

- `SSDR.settings`, `SmartSDR.exe.config` — sample SmartSDR configs used
  as a structural reference when implementing AppSettings parity.
- `*.csv` — research data captured while reverse-engineering mode/filter
  defaults across FlexRadio firmware versions.

These are not bundled with the build and not used at runtime. If a file
in this directory becomes a test fixture (consumed by `tests/`), move
it to `tests/fixtures/`.