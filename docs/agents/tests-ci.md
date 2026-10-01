# Agent guide — tests and CI

Part of [`AGENTS.md`](../../AGENTS.md). Read this before adding, renaming or
deregistering a test, or touching `tests/tests.cmake`, `.github/workflows/`,
`.github/ci-test-gate.txt`, or the CI Docker image.

## Adding a test — declare it in `tests/tests.cmake`, not `CMakeLists.txt`

Drop `<feature>_test.cpp` into `tests/`, then declare its `add_executable` +
`add_test` in **`tests/tests.cmake`**. There is no glob; every test is declared
explicitly, so copy a neighbouring target's block. `tests.cmake` aborts the
configure step on a misplaced declaration, and
`tools/check_test_registration.py --strict` fails the PR in CI.

Paths in `tests.cmake` are relative to the **repository root**
(`tests/foo_test.cpp`, `src/gui/Bar.cpp`) because it is pulled in with
`include()`, not `add_subdirectory()`. Do not convert it to a subdirectory:
`include()` keeps the root's directory scope, which keeps those paths and the
`${CMAKE_CURRENT_SOURCE_DIR}` references to `tools/` and `docs/` resolving.
Under `add_subdirectory` the source paths fail loudly and the others fail
*silently*.

A test that touches `AppSettings` also needs its target name in the
`AETHER_SETTINGS_CONSUMERS` list at the bottom of `tests.cmake`.

**Do not add a `ctest -R` step for it to `.github/workflows/ci.yml`.** The
per-PR gate there is a frozen allow-list (`.github/ci-test-gate.txt`) of tests
kept on the macOS and Windows jobs because the claim each pins is about that
platform's toolchain (Apple Metal, MSVC portability); the Linux job runs no
tests, and the list does not grow. Every test declared in `tests.cmake` that
the default configure builds runs unfiltered on every push to `main`
(`full-suite.yml`) and weekly under the sanitizers (`sanitizers.yml`). (A test
behind a default-OFF option runs in neither unless that lane passes the
option; say so in the PR.) `tools/check_ci_test_gate.py` runs in
`Static checks` and fails the PR if a `-R` pattern in a pull-request workflow
resolves to a name the frozen list does not carry. Removing a test from the
gate is fine: run the script with `--update` and commit the shorter list.
Growing it is a hand edit to the maintainer-owned file, with the reason in the
PR body.

Every unconditional `add_executable(<name>_test …)` must have a matching
`add_test`, or carry a `# not registered: <reason>` marker the registration
checker recognizes (option-gated and manual targets qualify). A test that
compiles but is never registered reads as coverage while running in no job.
This is convention until the `check_test_registration.py` extension from
#5254 lands.

A test for a fixed bug should be mutation-checked before the PR goes up:
break the guard on purpose, watch the test fail, restore it, and say so in the
PR body.

## Test-layer boundary — where an assertion lives

Decide the layer before writing the test (#5232):

| The assertion proves | It lives in |
|---|---|
| Wire encoding, parser bounds, model tables, scheduling, DSP, capability/safety policy | a socket-free CTest in `tests/`, grounded in the official guide or gateware |
| A refusal, a non-event, a dropped/malformed/disconnected input, a TX guard | a socket-free test that **injects the transport** — feed the frame handler or state machine directly; no `QTcpServer`/`QUdpSocket`, no peer process |
| A race or lifetime bug under churn | the sanitizer lane (`sanitizers.yml`) — the sanitizer is the point |
| The app converges with real firmware (session, RX, controls, meter liveness) | the automation bridge + `radiocert` on live hardware. Positive effects only: radiocert is a diagnostic, not pass/fail, and cannot prove an isolated non-event |
| A closed loop that needs a simulator peer (hpsdrsim TX) | an explicit opt-in target, never registered by default |

**No new synthetic peer standing in for third-party radio or amplifier
firmware enters the default graph.** A fake radio proves the client agrees with
our model of the radio, not with the radio. Four legacy exceptions remain, all
tracked for socket-free extraction in #5254: `vkamp_connection_test`,
`hl2_receiver_count_restart_test`, `gui_client_registration_recovery_test`, and
`thumbdv_queue_test` (pty-backed). Mining a retired fake peer's frame tables as
*input data* for injected-transport tests is encouraged; running the fake as a
live socket peer is not. Loopback mocks of documented, versioned HTTP APIs are
a different trade and are allowed.

Socket tests where **our own server is the subject** (rigctld, CAT, the TCI
server, the automation bridge's transport) remain legitimate. The carve-out
exempts a test from the fake-firmware ban, not from visibility: any new
socket-owning test is disclosed in the PR body, its `tests.cmake` block names
the socket it binds, reviewers notify the operator before continuing, and the
test fails fast (or skips, exit 77) when it cannot bind rather than consuming
its timeout.

Prefer behavioral seams over source-text assertions: a test that greps a
source file breaks on behavior-preserving refactors. Applets already link into
unit tests, so the seam is a `tests.cmake` entry, not a missing capability.

## CI image

CI runs in Docker image `ghcr.io/aethersdr/aethersdr-ci:latest`. **If you add a
new `find_package(...)` to CMakeLists.txt, also add the corresponding `-dev`
package to `.github/docker/Dockerfile`.** `docker-ci-image.yml` rebuilds the
image automatically; wait for it before the next CI run can use it. The image
pins Qt 6.12.0 LTS, but a newer distro Qt is a supported build, so never rely
on transitive includes.

## Gate integrity

- The per-PR gate in `ci.yml` is frozen (see above). If a maintainer decides a
  test must join it, the name goes into `.github/ci-test-gate.txt` by hand, in
  the same PR, with the reason in the PR body; the step comment in `ci.yml`
  says what it guards. `--update` will not add a name.
- Every `ctest` invocation in a workflow carries `--no-tests=error`: a `-R`
  filter that matches nothing otherwise exits 0, silently shrinking the gate.
- The frozen-list checker reads `tests.cmake` as text, so it catches a renamed
  or deregistered `add_test` but not a test that stops being registered on one
  platform at configure time. A multi-name step therefore carries a
  `Total Tests: N` pin (the ThumbDV step does); a single-name anchored step
  needs none.
- Deregistering or renaming a test requires grepping `.github/workflows/` for
  its name in the same PR, and running `--update` if it was on the frozen list.
- The weekly lane's sticky failure issues (`[sanitizer] … weekly run failure`)
  list ctest's failed tests first. A plain assertion failing there is a
  regression on `main`; treat it as one.
- A flaky test gets an issue naming the root cause — never empty retrigger
  commits.
