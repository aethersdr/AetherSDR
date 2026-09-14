# AetherSDR patches to WDSP 2.10

The source snapshot is pinned to TAPR/OpenHPSDR-wdsp commit
`b02d5bac675dd2f33ec2bab2b339f79a597c47dd` (`Release Version 2.10`).
AetherSDR carries four fixes in the otherwise exact `Source/*.[ch]` snapshot:

1. `upstream/nbp.c`: `destroy_notchdb()` now frees the `notchdb` object after
   its member allocations.
2. `upstream/nurbs.c`: `destroy_nurbs()` now frees the `nurbs` object after its
   member allocations.
3. `upstream/fmd.c` (`SetRXAFMNCde`) and `upstream/emph.c`
   (`SetTXAFMEmphNC`): `a->pfcimp = build_fcimp (...)`. Both functions tear
   down the filter-curve object and then rebuild it, but 2.10 discards the
   pointer `build_fcimp()` returns, so `a->pfcimp` still holds the address of
   the object `teardown_fcimp()` just freed. The two statements that follow —
   `exec_fcimp()` and `get_pfcpulse()` — then read and write through it, and
   the newly built object leaks.

   This is a **use-after-free on a live path, not a teardown-only leak**:
   `RXASetNC()` calls `SetRXAFMNCde()`, and `WdspChannel::open()` calls
   `RXASetNC()`, so every channel this host opens hits it. ASan reports it as
   a 4-byte read of freed memory in `exec_fcimp` (`fcurve.c:67`). It is new in
   2.10 — 2.00's `SetRXAFMNCde()` used `fc_impulse()` and owned the impulse
   buffer directly, so the object-lifetime mistake did not exist to make.

   `SetTXAFMEmphNC()` is the same mistake in the TX chain. This host does not
   currently call it, and it is fixed anyway: it is one line, it is the same
   refactor, and a known use-after-free left in a vendored tree is a trap for
   whoever calls it next.

   Reported upstream as TAPR/OpenHPSDR-wdsp#2; drop both lines when a release
   contains the assignment.
4. `upstream/channel.h`, `upstream/channel.c`, `upstream/main.c`,
   `upstream/iobuffs.c`, `upstream/iobuffs.h`: an exit handshake between the DSP worker and
   `pre_main_destroy()`. Upstream's only barrier between the detached worker's
   exit and `destroy_main()` / `post_main_destroy()` freeing the semaphore,
   mutex and buffers it still touches was `Sleep(25)` — a scheduling bet, not
   synchronization, and under load or a sanitizer the worker is still in
   `pthread_cond_wait()` on freed memory.

   `struct _ch` gains `mainGen`, `mainRunGen` and `mainExited`.
   `start_thread()` increments `mainGen` before every `_beginthread` (so the
   `SetInputBuffsize` / `SetDSPBuffsize` / `SetInputSamplerate` /
   `SetDSPSamplerate` rebuilds are covered too); `wdspmain()` publishes that
   value in `mainRunGen` at entry and stores it into `mainExited` as its last
   statement; `pre_main_destroy()` polls until `mainExited == mainGen`, with a
   1 s cap and then falls through, because upstream ignores thread-creation
   failure and an unbounded wait would hang `CloseChannel()`. The port's
   Interlocked shims are seq_cst `__atomic_*` builtins, so the edge is real to
   TSan, not merely quiet.

   Three details are not obvious and were all found in review of #5411:

   - **The worker had two exits; it now has one.** `dexchange()` (`iobuffs.c`)
     began `if (!_InterlockedAnd (&ch[channel].run, 1)) _endthread();`, so a
     worker inside the DSP switch when `run` cleared terminated there: with
     `csDSP` held, since `_endthread()` does not unwind and `wdspmain()` calls
     `dexchange()` inside the section, leaving `post_main_destroy()` to call
     `DeleteCriticalSection` on a locked section — and without ever reaching
     the exit handshake. `dexchange()` now **returns** non-zero instead
     (`int` rather than `void`, two call sites, both in `main.c`) and
     `wdspmain()` unlocks and leaves the loop, so the tail is the single exit.
     Making it single is what lets the handshake store a generation held in a
     **local**: an abandoned worker must not read its generation back out of
     `ch[]`, because by then that slot can belong to its successor and the
     acknowledgement would be made on the successor's behalf.
   - **`pre_main_destroy()` sets `exec_bypass` BEFORE clearing `run`**, the
     reverse of upstream's order, so a worker that has not yet read the bypass
     takes the bypass branch rather than unwinding through `dexchange()`. That
     narrows the window and saves a wakeup; correctness does not rest on it,
     because either route now leaves through `wdspmain()`'s tail.
   - **The flag is generation-valued, not 0/1.** If a wait ever falls through
     its cap the old worker is still alive and will store eventually. With a
     0/1 flag that late store would land on the *next* worker's slot and
     satisfy the following wait for free, silently disabling the handshake for
     the rest of the channel's life. A stale generation never equals the
     current `mainGen`, so it is inert.

   `flushChannel()` has the same detached shape and no handshake; it has not
   surfaced, and gets the same treatment if it does.

Without the first two, opening and closing one RX channel leaks one `notchdb`
object and two NURBS objects. `wdsp_channel_test` detects that deterministically.
Without the third, every channel open reads and writes freed memory; ASan fails
`wdsp_channel_test` immediately.
Without the fourth, every channel close is a use-after-free race on the
worker thread; `wdsp_channel_test` and the HL2 backend tests show it under
ThreadSanitizer.

When refreshing WDSP, first check whether upstream contains equivalent frees.
If it does, drop the corresponding local patch. Otherwise reapply only these
minimal fixes and run the lifecycle test under AddressSanitizer on every supported
platform.
