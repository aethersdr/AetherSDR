# Node: build — build the PR head once, run its tests, invert what was asked

Read `stance.md` first. You are the only node allowed to build. Everything
else reads. Inputs: the PR-head worktree path, optionally a merge-base
worktree path, the coordinator's preflight result, and the union of
`testsToInvert` requests from the audit nodes (they may arrive empty — the
audits run concurrently with you, so the coordinator passes only what it
gathered up front; the drive node gets the rest).

Rules that override everything below:

- **Preflight is binding.** The coordinator already inspected the PR's test
  sources for socket ownership (`QTcpServer`, `QUdpSocket`, `QLocalServer`,
  `bind()`, `listen()`, `connectToHost()`, `Fake*` peers). Any target named
  in `preflight.socketTargets` is **not to be built or run** by you. Do not
  reason your way around this; record `skipped: preflight` for it.
- **Never touch the shared checkout.** Configure and build only inside the
  worktree paths you were given, in `build/` under each. Export
  `TMPDIR=<the tmp path you were given>` before configuring; GCC writes to
  it and the default is a tmpfs shared with other agents.
- Cap parallelism at `-j$(($(nproc)/2))` if `pgrep -f ninja` shows another
  build running; otherwise use all cores.
- `ccache` is on; do not disable it.

Do, in order:

1. Configure and build the PR head:
   `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo` then
   `ninja -C build AetherSDR <PR's own test targets>`. Build only what the
   review needs — the app binary (for the drive node) plus the tests the PR
   adds or modifies. A full build is not required and is discouraged.
2. Run the PR's own tests with `ctest --test-dir build -R <name>` per
   target. Record pass/fail and the tail of any failing output.
3. **Invert each requested test**: apply the named mutation (revert the fix
   hunk, or delete the guard) in the PR-head worktree with `git stash`
   **forbidden** — use `git diff > /tmp/…patch` + `git apply -R` on the hunk,
   or edit and then `git checkout -- <file>` inside *your* worktree only —
   rebuild the target, rerun the test, restore the file, and record whether
   the test noticed. A test that still passes with the fix reverted is a
   `blocker` finding.
4. If a merge-base worktree was given, configure and build **only the app
   binary** there (the drive node reproduces "before" against it).
5. **"CI is green" is not "the suite passes."** Every `ctest` call in
   `ci.yml` is `-R`-filtered, so only a few of the ~240 tests gate a merge.
   Do not run the whole suite; if you did run something broad and it failed,
   run the same test on the merge-base build before attributing it to the
   PR. If the base fails the same way, it is environment or test-layer
   boundary, not a PR finding — say so.

Return `buildDir` for the head (and `baseBuildDir` if built), `ok`, the
per-target results, the inversion results, and the tail of any build error.
Never delete the worktrees; the coordinator owns cleanup.
