# AetherSDR patches to WDSP 2.00

The source snapshot is pinned to TAPR/OpenHPSDR-wdsp commit
`584e8aca5ba1c4c6bc66fc0cc164ce567c8ba1e3` (`Release Version 2.00`).
AetherSDR carries five local fixes in the otherwise exact `Source/*.[ch]`
snapshot — four teardown fixes and one channel-state fix:

1. `upstream/nbp.c`: `destroy_notchdb()` now frees the `notchdb` object after
   its member allocations.
2. `upstream/nurbs.c`: `destroy_nurbs()` now frees the `nurbs` object after its
   member allocations.
3. `upstream/cfir.c`: `cfir_impulse()` now frees its temporary transition table
   before returning the generated impulse.
4. `upstream/channel.h`, `upstream/main.c`, `upstream/channel.c`,
   `upstream/iobuffs.c`: an exit handshake between the DSP worker and
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

5. `upstream/channel.c`: `SetChannelState()` case 1 now cancels a pending
   down-ramp (`flush_slews()` under `csEXCH`) before it arms the up-ramp.

   Upstream's case 1 sets `slew.upflag`, `iob.ch_upslew` and `exchange` and
   clears `exec_bypass`, but never touches `iob.pc->slew.downflag`. The two
   flags are read independently on opposite sides of `fexchange0`/`fexchange2`
   — `upflag` gates the input, `downflag` gates the output — and the ramp only
   advances when the host clocks `fexchange*`. So a stop followed by a start
   before the host has clocked the down-ramp to completion leaves `downflag`
   set on a channel whose `state` is now 1, and the next few blocks finish the
   stale ramp. `downslew0`/`downslew2`'s completion arm does
   `InterlockedBitTestAndReset (&ch[channel].exchange, 0)`, so finishing that
   ramp **clears `exchange`**: every later `fexchange*` fails its opening
   `if (exchange)` test and returns having written nothing and reported no
   error, while `state` still reads 1. The channel is silently dead until it is
   closed and rebuilt, and no flag a host can read says so.

   The asymmetry is the point — only the *down* flag's completion clears
   `exchange`, so the mirror case (a stop taken with `upflag` still pending)
   needs nothing.

   `flush_slews()` rather than a bare clear of `downflag`, because the flag is
   not the whole ramp: `slew.dstate`/`dcount` are the state machine, and
   clearing the flag alone strands `dstate` mid-ramp for the *next* stop to
   resume from. It resets both directions, which is also what the up-ramp being
   armed wants. It clears `upflag`, hence the ordering: flush first, arm
   second. `csEXCH` because `dstate`/`dcount` are plain ints owned by
   `fexchange*`'s critical section — the same reason `SetChannelTDelayUp`/`Down`
   and `SetChannelTSlewUp`/`Down` already take it around their own
   `flush_slews()` — and because it makes the whole of case 1 atomic against
   `fexchange*`. No new lock-order edge: `csEXCH` is the inner of the two
   channel sections (`flushChannel` takes `csDSP` then `csEXCH`), nothing is
   taken inside it and nothing waits there, and the port maps
   `CRITICAL_SECTION` to a **recursive** pthread mutex. `ch[channel].flushflag`
   is deliberately left alone: the flush request belongs to the parked
   `flushChannel` thread, which only a completed ramp can release.

   Found in review of #5628. Without it, `WdspChannel::setRunning(true)` on a
   channel whose stop has not been clocked out — the T/R edge `docs/HERMES.md`
   §13 row 9a contemplates — silently kills the channel while `isRunning()`
   reports true. `wdsp_channel_test`'s `runRestartDuringRampTest` covers all
   three ways in (no clocking at all, a restart inside the slew window, and a
   start after `reconfigure()` of a stopped channel) and fails on every one
   with this patch reverted.

Without these lines, opening and closing one RX channel leaks one `notchdb`
object and two NURBS objects, while one TX channel leaks one transition table.
`wdsp_channel_test` detects both paths deterministically.
Without the fourth, every channel close is a use-after-free race on the
worker thread; `wdsp_channel_test` and the HL2 backend tests show it under
ThreadSanitizer.
Without the fifth, a stop immediately followed by a start silently and
permanently disables the channel; `wdsp_channel_test` shows it
deterministically.

**Marking convention.** Patches 4 and 5 carry `// AetherSDR patch N:` comments
at every edited site, so `grep -rn "AetherSDR patch" third_party/wdsp/` finds
them all. Patches 1-3 predate that convention: each is a single added
`_aligned_free` line with no marker, findable only from this file. New patches
use the marker.

When refreshing WDSP, first check whether upstream contains equivalent frees.
If it does, drop the corresponding local patch. Otherwise reapply only these
minimal fixes and run the lifecycle test under AddressSanitizer on every supported
platform.
