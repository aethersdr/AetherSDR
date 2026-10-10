# AetherSDR patches to WDSP 2.10

The source snapshot is pinned to TAPR/OpenHPSDR-wdsp commit
`b02d5bac675dd2f33ec2bab2b339f79a597c47dd` (`Release Version 2.10`).
AetherSDR carries the local changes documented below in the otherwise exact
`Source/*.[ch]` snapshot:

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

   **THE SINGLE POST IS NOT ENOUGH, and that was found in review of #5628.**
   The handshake posts one token to `Sem_BuffReady` so the worker wakes, sees
   `run == 0` and exits. That token can be STOLEN: `flush_iobuffs()`
   (`upstream/iobuffs.c`) drains the same semaphore with
   `while (!WaitForSingleObject (a->Sem_BuffReady, 1));`, and a stop that was
   clocked out leaves `flushChannel` runnable. If the flush thread gets its slot
   while the wait loop is running it consumes the worker's wake-up; the worker
   parks forever, the loop falls through its cap exactly as designed, and
   `destroy_iobuffs()` then closes the semaphore under a live waiter — where
   glibc's `pthread_cond_destroy()` blocks and never returns.

   So the loop now RE-POSTS the token on every iteration. The worker exits on
   `run == 0` however many tokens are outstanding and the `iob` is freed
   immediately afterwards, so the extras cost nothing. Measured by ten9876 on
   #5628: 7 hangs in 16 runs of `wdsp_channel_test` under 8-way parallel load on
   Arch/glibc, 0 in 32 with the re-post. **The hang does not reproduce on
   macOS/arm64 — 16 runs clean with the fix AND 16 clean without it — so this
   platform cannot confirm the fix, only that it causes no regression.**

   The general statement, for whoever refreshes this next: the handshake is
   sound only while nothing else drains `Sem_BuffReady`, and `flush_iobuffs()`
   does.

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

5. `upstream/nnr.c`, `upstream/nnr.h`: a standalone control surface for Neural
   Noise Reduction — `setRun_nnr`, `setPosition_nnr`, `setCmode_nnr`,
   `setMaskFloor_nnr`, `setTestMode_nnr`, `setAlpha_nnr`, `setAlphaKnee_nnr`,
   `setTau_nnr`, `setMaxGain_nnr` and `setSmooth_nnr`.

   Upstream exposes all ten only as `SetRXANNR*` properties, which index
   `rxa[channel]`. `create_nnr()`, `xnnr()` and `destroy_nnr()` touch neither
   `ch[]` nor `rxa[]`, so the block runs perfectly well outside a channel — the
   way `create_anbEXT()`/`xanbEXT()` already run the impulse blanker — but a
   host that does so can reach none of its settings, because `nnr->nets[]` and
   the `NNR_ALL_MODELS` macro are private to `nnr.c`. Only `setModel_nnr()` has
   a standalone form.

   Each added function is the body of its RXA property without the
   `ch[channel].csDSP` section, and each RXA property keeps working unchanged.
   Locking is the caller's, because a standalone block has no channel whose
   critical section to take.

   **Additive only** — no existing function is modified, which is what makes
   this survive a refresh as a clean re-apply rather than a conflict.

   Worth offering upstream: `create_nnr()` already takes `mask_floor` as a
   constructor argument, so the accessor is the setter that argument implies.
   Drop any function a future release provides itself.

6. `upstream/nnet.c`: `setAlpha_nnet()` and `setKnee_nnet()` now check `n->df`
   before writing through it.

   They are the only two of the six NNET tuning setters without that guard —
   `setSmooth_nnet()` checks `if (n->df)`, `setMaxGain_nnet()` and
   `setFloor_nnet()` check `if (n->ready)`, `setTau_nnet()` checks `if (n->cnd)`.
   A slot whose model fails to build never reaches `create_dfhead()`, so `df`
   stays NULL from `malloc0`, and `calc_nnr()` stores slot 0 unconditionally
   (unlike slots 1+, which it validates with `ok_nnet()`).

   **Reachable in a shipping configuration, and reproduced:** a well-formed
   model with different dimensions, named `wdsp_nnr_0.bin` in the process's
   working directory, is loaded in preference to the built-in (RFC #5684 §8
   keeps that lookup). WDSP's designed response is to pass audio through — and
   then the first `setAlpha`/`setKnee` write dereferences NULL. Confirmed as
   SIGSEGV against an unpatched build; exits cleanly with the guard.

   Latent upstream too, via `SetRXANNRAlpha`/`SetRXANNRAlphaKnee`, for any
   console that offers those controls. Reported as TAPR/OpenHPSDR-wdsp#4 with
   a fix in TAPR/OpenHPSDR-wdsp#5; drop this when a release carries the guard.

7. `upstream/channel.c`: `SetChannelState()` case 1 now cancels a pending
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

   **Unchanged by the 2.10 refresh.** Upstream's `channel.c` is byte-identical
   between `Release Version 2.00` and `Release Version 2.10` — as are
   `channel.h`, `iobuffs.c` and `main.c` — so 2.10 neither fixes this nor moves
   the code it is stated against, and the patch carries over verbatim.

   **Reported upstream as TAPR/OpenHPSDR-wdsp#6; drop this when a release
   carries the fix.** It is an upstream defect rather than an AetherSDR
   accommodation — any host that uses `SetChannelState` as its T/R verb, which
   is what `channel.c`'s own contract says it is for, produces stop/start pairs
   spaced by the keying turnaround and hits it.

   Found in review of #5628. Without it, `WdspChannel::setRunning(true)` on a
   channel whose stop has not been clocked out — the T/R edge `docs/HERMES.md`
   §13 row 9a contemplates — silently kills the channel while `isRunning()`
   reports true. `wdsp_channel_test`'s `runRestartDuringRampTest` covers all
   three ways in (no clocking at all, a restart inside the slew window, and a
   start after `reconfigure()` of a stopped channel) and fails on every one
   with this patch reverted.

   **This patch covers the ramp that is still pending, and NOTHING ELSE.** The
   text here first claimed it made stop/start pairs safe at any spacing; that
   was true only inside the ramp, and false just past it. See patch 8.

8. `upstream/channel.c`: `SetChannelState()` case 1 now waits out a flush that
   a *completed* down-ramp already requested, before it arms the up-ramp.

   Patch 7 cancels a ramp that is still **pending**. It cannot cancel a flush
   that a ramp which already **completed** has requested. At that completion
   `fexchange0`/`fexchange2` clear `exchange` and release `Sem_Flush`
   (`iobuffs.c`), and the `flushChannel` thread is left runnable but not
   necessarily scheduled. When it does run it takes `csDSP` then `csEXCH`,
   flushes, and does `InterlockedBitTestAndSet (&a->exec_bypass, 0)`. Arm in
   that window and `flushChannel` sets `exec_bypass` *after* case 1 cleared it;
   `wdspmain` then skips `dexchange`/`xrxa` entirely (`main.c`) and the worker
   produces nothing for a channel whose `state` reads 1.

   Two failure modes, both measured on this tree with a probe that stops,
   clocks N blocks at the 256/48 kHz cadence, starts with **no gap**, and then
   asks for audio:

   | mode | sweep | patch 7 only | with patch 8 |
   |---|---|---|---|
   | non-blocking (what production uses) | spacings 0-10, 40 trials each | 42 of 440 dead — 24 at spacing 3, 18 at spacing 4, **none at 0-2** | 0 of 440 |
   | blocking | spacings 0-6, 20 trials each | 20 of 140 hung — all at spacing 3 | 0 of 140, no hang |

   A control that sleeps 20 ms before each start, giving the flush thread its
   slot, is 0 of 440 with patch 7 alone. The ramp is exactly three blocks here
   (BEGIN 1 + DOWNSLEW `ntdown` + 1 + ZERO `out_size` + 1 = 739 samples at
   `out_size` 256), so spacing 3 is the first at which it completes — which is
   why nothing dies at 0-2, the window patch 7 already covered.

   The blocking hang is a hard one: with `exec_bypass` set the worker never
   releases `Sem_OutReady`, so `fexchange2`'s
   `if (a->bfo) WaitForSingleObject (a->Sem_OutReady, INFINITE)` never returns.
   Six thread samples of six separate stalls all showed that two-thread
   starvation — host parked in `fexchange2` holding `csEXCH`, `flushChannel`
   finished and back on `Sem_Flush`, worker idle on `Sem_BuffReady`. A
   three-way lock cycle (`flushChannel` holding `csDSP` and blocking on the
   `csEXCH` the parked host holds, worker then blocking on `csDSP`) is
   reachable from the same window on a different interleaving; no sample caught
   it, and it is not what the measurements above are evidence of.

   The wait predicate is `exchange` clear **and** `flushflag` set, which names
   the completed-ramp case and only it: a pending ramp leaves `exchange` set;
   case 0's dmode-1 timeout force-clears both; `pre_main_build` clears
   `flushflag`, so `OpenChannel`'s start never waits; and every in-tree restore
   call (`SetDSPBuffsize`, `SetDSPSamplerate`, `RXASetNC`, `TXASetNC`) reaches
   case 1 only after its own `SetChannelState(0, 1)`, which leaves `flushflag`
   clear on both exits. The only caller that can reach the wait is a host that
   stopped with dmode 0 and clocked the ramp out.

   **Outside `csEXCH`, and that is load-bearing.** `flushChannel` needs
   `csEXCH` to finish and clear `flushflag`, so waiting while holding it would
   guarantee the timeout instead of the flush. The waiting thread holds no
   channel lock at all, and the host cannot be inside `fexchange*` on it —
   `WdspChannel::setRunning()` and `open()` both take the control fence, which
   refuses while a `processIq()` callback is in flight — so the wait cannot
   join the cycle above. Nothing that must run to satisfy it can be blocked by
   it either: `csDSP` is never held across an unbounded wait (`dexchange` only
   memcpys and releases), and `flush_iobuffs`'s `Sem_BuffReady` drain is a 1 ms
   -timeout poll.

   **Bounded** by case 0's existing `count`/`timeout`, for the same reason
   patch 4 bounds its handshake: a flush thread that never runs must not hang a
   start forever, and falling through after the cap is exactly today's
   behaviour, no worse. Cost, over 132 starts across spacings 0-10:
   `setRunning(true)` mean 251 us, max 3.1 ms, against mean 0.83 us / max
   3.1 us with the patch reverted. The owner-side stops in `Hl2RxDsp` and
   `AnanRxDsp` are `setRunning(false)` — case 0 — and take no new wait at all.

   **Unchanged by the 2.10 refresh**, for the same reason patch 7 is: the
   `flushChannel`/`Sem_Flush`/`exec_bypass` machinery this is stated against
   lives in `channel.c`, `iobuffs.c` and `main.c`, all three byte-identical
   upstream between 2.00 and 2.10.

   **Reported upstream as TAPR/OpenHPSDR-wdsp#7; drop this when a release
   carries the fix.** Upstream for the same reason patch 7 is, and the blocking
   variant is the one that will bite the next integrator hardest: it parks the
   host in `fexchange2` forever rather than merely silencing it.

   Found by K5PTB in review of #5628, on the shape he suggested.
   `runRestartDuringRampTest`'s scenario table now straddles the ramp:
   spacings 0 and 1 inside it, 3, 4 and 5 at and past its completion, restarted
   with no gap. With this patch reverted it goes red on one of those three rows
   in 5 of 5 runs — which row varies, so all three earn their place. The
   post-restart clocking runs on its own thread under a 20 s deadline, because
   the blocking failure is a hang, and inline it would be a ctest timeout
   rather than a message anyone can read.

   NOT MEASURED ON HARDWARE. The probe is synthetic; no radio has run any of
   this.

9. `upstream/channel.c`, `upstream/iobuffs.c`, `upstream/iobuffs.h`: the
   `flushChannel` exit handshake now runs in `pre_main_destroy()`, before
   `destroy_main()`, instead of in `destroy_iobuffs()` after it.

   **This is a move, not a new mechanism.** Upstream already had the handshake:
   `destroy_iobuffs()` sets `flush_bypass`, releases `Sem_Flush` so a parked
   thread wakes and sees it, and waits for the thread's own reset of
   `flush_bypass` at the tail of `flushChannel()`. What was wrong was where it
   ran. `CloseChannel()` is `pre_main_destroy(); destroy_main();
   post_main_destroy()`, and `destroy_iobuffs()` is reached only from the third
   — so the flush thread was still live across `destroy_main()`, which is
   `destroy_rxa()`/`destroy_txa()`, exactly the chain `flush_main()` ->
   `flush_rxa()` walks. The patch adds a `quiesce_flush()` function holding that
   code and calls it from both ends of the pair; `destroy_iobuffs()` keeps its
   call and normally finds the work already done.

   Patch 4 gave the `wdspmain` worker an exit handshake and said of this thread:
   *"`flushChannel()` has the same detached shape and no handshake; it has not
   surfaced, and gets the same treatment if it does."* It has surfaced.

   **What makes it reachable is a completed down-ramp.** At the completion of
   the ramp, `fexchange0`/`fexchange2` clear `exchange` and release `Sem_Flush`
   (`iobuffs.c`), leaving `flushChannel` runnable. Before
   `WdspChannel::setRunning()` there was no way to reach that state and then
   close: the only stop was `close()`'s own, taken behind the host's control
   fence with nothing left to call `fexchange*`, so the ramp never completed and
   the thread stayed parked through the whole of teardown. A host that stops a
   channel, keeps clocking it — which is what `WdspChannel.h` now documents as
   correct usage, and what the T/R mute of `docs/HERMES.md` §13 row 9a will do —
   and then destroys it, hands `destroy_rxa()` a chain another thread is inside.

   **MEASURED, before it was fixed.** macOS arm64, AppleClang, RelWithDebInfo,
   one trial per process. TWO BUILDS OF THE SAME SOURCE are reported separately
   and deliberately: the rate is a property of the schedule and the heap, not of
   the defect, and quoting one figure would misrepresent it.

   Build A, DFNR not linked:

   | shape | non-blocking | blocking |
   |---|---|---|
   | create -> clock 32 -> destroy (never stopped) | clean 10/10 | clean 10/10 |
   | create -> clock 32 -> `setRunning(false)` -> destroy | clean 10/10 | clean 10/10 |
   | create -> clock 32 -> `setRunning(false)` -> **clock 32** -> destroy | **crashed 30/30** | clean 30/30 |
   | the same, with a 50 ms sleep before the destroy | clean 30/30 | — |

   Build B, DFNR linked (which moves the heap), same source:

   | shape | non-blocking | blocking |
   |---|---|---|
   | never stopped; and stopped with nothing clocked | clean 15/15 each | — |
   | stopped **and clocked** | **crashed 8/30** | **crashed 1/30** |

   The single distinguishing variable is the clocking after the stop, and the
   sleep control is what identifies the flush thread rather than the stop itself.
   Within build A the crashing shape measured 3 of 10, then 16 of 30, then 30 of
   30 across relinks of identical source, so no one figure should be read as
   "the" rate; what is stable is that the shape faults and its two neighbours do
   not. Build B is the one that matters for the blocking mode: 1 of 30 is small
   but not zero, so this is not a mode-specific defect — the suite's own cases,
   which are all `blockForOutput = true`, are exposed, just far less often here
   than the non-blocking form production uses.

   `lldb` on a faulting run shows both halves at once — thread 1 in
   `destroy_fmd` <- `destroy_rxa` <- `destroy_main` <- `CloseChannel` <-
   `WdspChannel::close`, thread 4 in `flush_emnr` <- `flush_rxa` <-
   `flush_main` <- `flushChannel`. With patch 9 the crashing shape is clean
   80/80 in build B, 40 in each output mode.

   @ten9876 reported it against Arch/gcc, where he measured 10 of 10 and a
   `pthread_cond_destroy` hang variant in `wdsp_channel_test` itself, 2 runs in
   3. The crash reproduces here; the hang does not — 35 runs of the unfixed
   suite, 0 hangs. See patch 4's note on `_beginthread` failure and on the
   worker's exit handshake for the part of that difference worth keeping in
   view.

   **BOUNDED, where upstream's spin was not**, at 1000 x `Sleep(1)` to match
   patch 4's handshake and for the reason patch 4 gives: upstream ignores
   `_beginthread()` failure, and an unbounded wait on a thread that was never
   created would hang `CloseChannel()` forever. This is the one respect in which
   the moved code is not verbatim, and it is a deliberate trade rather than an
   oversight.

   **BE PRECISE ABOUT WHAT THE CAP COSTS, because an earlier draft of this entry
   said "no worse than today" and that is not exact** (review of #5628).
   Upstream's wait was `while (InterlockedAnd (&a->flush_bypass, 0xffffffff))
   Sleep(1);` — unbounded, so it could not fall through at all. This one can,
   and on exhaustion `flush_quiesced` is set on the way out regardless, so
   `destroy_iobuffs()`'s own call returns immediately and the `CloseHandle
   (a->Sem_Flush)` five lines later runs under a flush thread that may still be
   parked on it. That is the same `pthread_cond_destroy()`-under-a-live-waiter
   shape patch 4's re-post exists to close, reached by a different door. The
   trade is still right — a guaranteed hang on a thread that was never created
   is worse than a 1 s wait that in practice never exhausts, and the flush
   thread's bypass path is a wake, a flag read and an exit — but the bound
   INHERITS patch 4's failure mode rather than being free of it, and that is
   worth knowing before anyone shortens the cap.

   **IDEMPOTENT**, via a new `flush_quiesced` bit in `struct _iobuffs`, because
   both ends of the `pre_main_destroy()`/`post_main_destroy()` pair call it and
   a second pass must not re-arm a `flush_bypass` that no thread is left to
   acknowledge — that would burn the whole cap on every close. Per-`iob`, not
   per-channel-slot, so patch 4's generation hazard does not arise: the `iob` is
   freed and reallocated on every build.

   **AFTER patch 4's worker wait, not before.** `flushChannel()` takes `csDSP`,
   which the worker holds across `dexchange()`, so quiescing the flush thread
   first could make `pre_main_destroy()` wait out the worker's block through a
   second thread. With the worker already gone both channel sections are free.

   **Unchanged by the 2.10 refresh**, for the same reason patches 7 and 8 are:
   `channel.c`, `iobuffs.c`, `iobuffs.h` and `main.c` are byte-identical
   upstream between `Release Version 2.00` and `Release Version 2.10`.

   **Reported upstream as TAPR/OpenHPSDR-wdsp#8; drop this when a release
   carries the fix.** Upstream, and the most serious of the three — it is a
   use-after-free reachable by any host that stops a channel, keeps clocking it
   and then closes, which is the natural shape for a T/R mute.

   Found by @ten9876 in review of #5628. Pinned by `wdsp_channel_test`'s
   `runCloseAfterStoppedClockingTest`, which drives the crashing shape 24 times
   in one process and goes red — as a signal, not a message, because there is no
   assertion that catches a use-after-free from inside the process committing it
   — in 5 of 5 runs with this patch reverted.

   NOT MEASURED ON HARDWARE. The probe is synthetic; no radio has run any of
   this.

10. `upstream/firmin.c`: `plan_fircore()` builds the minimum-phase workspace
    only when `a->mp` is set, and `calc_fircore()` builds it on first use
    (`ensure_minphase()`) so `setMp_fircore()` can still turn minimum phase on at
    any time. `deplan_fircore()` tolerates the absent object.

    ONE NEW PROPERTY THIS GIVES `setMp_fircore()`: it can now build FFTW plans,
    which it never could before, because `ensure_minphase()` reaches
    `create_minphase()`'s four plans (`FFTW_ESTIMATE` since patch 12). FFTW
    planning is not thread-safe. Both callers of `RXASetMP()` hold the setup
    mutex: `WdspChannel::open()`, and `WdspChannel::setMinimumPhase()`, which
    `Hl2RxDsp` calls on a mode change into or out of CW (#5498).
    **Anything that toggles minimum phase outside that mutex is the line to
    revisit.** Raised by the reviewer on #5697 and recorded here rather than
    left in a review thread.

    Upstream ends `plan_fircore()` with an unconditional
    `a->pminphase = create_minphase (a->nc, a->pfactor)`, with no reference to
    `a->mp`. The only consumer is the `if (a->mp)` branch of `calc_fircore()`.
    `WdspChannel::Config::minimumPhase` defaults to `false`, and only the HL2 in
    CW keeps it there (`Hl2RxDsp::rxMinimumPhaseFor`); at `mp == 0` every
    minimum-phase workspace an RX channel builds would be dead on arrival.

    `create_minphase()` (`fir.c`) allocates six `complex` buffers and one
    `double` buffer at `N * pfactor` elements -- 104 bytes per element in total --
    and creates **four plans** of that length (`FFTW_PATIENT` upstream,
    `FFTW_ESTIMATE` under patch 12).

    **The standing cost is the resident workspace, not the planning.** An earlier
    draft of this note led with plan-construction time; that overstated it.
    `WdspChannel::open()` calls `loadWisdomOnce()` before the first plan and
    exports afterwards, and FFTW wisdom is keyed on the (transform kind, size)
    pair, so on a warm cache `FFTW_PATIENT` imports instead of re-measuring and
    the planner cost is already largely amortised. What is NOT amortised is the
    memory: the buffers are allocated on every open, held for the life of the
    channel, and never read. Plan time is the **cold-cache** case -- a first run,
    a cleared cache, a CI container, a session whose export never ran -- and
    there it does bite, because these are among the largest geometries the
    channel plans. Treat it as the cold-start half of the argument, not the
    headline.

    Counted, not assumed, by instrumenting `create_fircore()` and
    `create_minphase()` and opening one channel: **an RX channel builds 13
    fircores, 12 of them `mp == 0`.** The one that is not is `rxa[].eqp`
    (`nc = 16384`, `pfactor` 4), which upstream creates with the minimum-phase
    flag set; its workspace is real and this patch keeps it. `RXASetNC()`
    re-plans six of the twelve, so a single RX open at
    `Hl2RxDsp::kRxFilterTaps` = 8192 makes **19 `create_minphase()` calls, 18 of
    them for `mp == 0` cores -- 72 dead `FFTW_PATIENT` plans**, at lengths
    131072 (x3), 65536, 32768 (x8), 8192 (x5) and 4096.
    A TX channel builds 7 fircores, 6 of them `mp == 0`.

    Measured against `wdspPortOutstandingAllocations()` /
    `wdspPortAllocationSequence()` (macOS arm64, one RX open at 8192 taps,
    `mp == 0`): live WDSP allocations while the channel is open fall from 1300 to
    1204, and allocations made during the open from 1518 to 1374. `create_minphase()`
    makes 8 allocations, so that is exactly 12 fewer objects held and 18 fewer
    built -- the counted inventory, independently. Before the patch, `mp == 1`
    and `mp == 0` hold the identical 1300; after it, `mp == 1` holds 1252, the
    48 allocations of the six cores `RXASetMP()` reaches.

    **Two different totals come out of those counts, and they must not be
    confused.** `RXASetNC()` reaches its six cores through `setNc_fircore()`,
    which calls `deplan_fircore()` BEFORE `plan_fircore()` -- so those six
    workspaces are built at `nc = 2048`, freed, and built again at 8192. They
    appear twice in the built list and once in the resident one. Showing the
    working, in `N * pfactor` elements at 104 bytes each:

    | quantity | elements | bytes |
    |---|---|---|
    | Built, dead, across one RX open (18 calls: 131072 x3, 65536, 32768 x8, 8192 x5, 4096) | 765,952 | **~79.7 MB** |
    | less the six pre-`RXASetNC` workspaces `setNc_fircore()` frees (32768 x3 + 8192 x3) | -122,880 | -~12.8 MB |
    | **Resident, dead, once the channel is open** (12 cores: 131072 x3, 65536, 32768 x5, 8192 x2, 4096) | **643,072** | **~66.9 MB** |

    The figure this note quotes is the **resident** one. Because the frees
    precede the allocations and every core only grows, resident is also the peak
    concurrent figure: **~66.9 MB of never-executed workspace per RX channel at
    8192 taps**. The ~79.7 MB is allocation churn across the open, not memory
    ever held at once. At WDSP's own `max(2048, dsp_size)` default the two
    coincide at ~28.5 MB, because `RXASetNC(2048)` changes no `nc` and re-plans
    nothing; likewise ~16.2 MB per TX channel.

    Every byte figure above is arithmetic on the counted inventory and
    `create_minphase()`'s own allocation sizes. **No memory was measured**, here
    or on hardware, and nothing in this work ran a radio.

    **Since patch 14 the `mp == 1` case holds none of it either.** Patch 10
    stops building the workspace for cores that never design at minimum phase;
    `Hl2RxDsp` now runs its bandpass cores at minimum phase outside CW (#5498),
    and for those cores the workspace is used -- but only during a design.
    Patch 14 frees it after each one, so no core holds it between designs.
    Measured there with `wdspPortOutstandingAllocations()`: an 8192-tap RX
    channel holds 1200 live WDSP allocations at either phase; without patch 14
    the minimum-phase channel held 48 more, ~49 MB by heap measurement.

    This is shared DSP -- **every backend that opens a `WdspChannel` pays it**,
    not only the HL2, and the HL2's tap count only sets how much.

    **Drop condition.** This is the one entry here that is a performance change
    rather than a correctness fix, so it is also the one a maintainer may simply
    decline to carry. Drop it if upstream makes the construction conditional
    itself; drop it if this tree ever sets `Config::minimumPhase` true on every
    path, since the workspace would then be wanted everywhere and the laziness
    would buy nothing. **Not reported upstream yet** -- unlike patch 3, which is
    TAPR/OpenHPSDR-wdsp#2, this one has no upstream issue behind it.

Without the first two, opening and closing one RX channel leaks one `notchdb`
object and two NURBS objects. `wdsp_channel_test` detects that deterministically.
Without the third, every channel open reads and writes freed memory; ASan fails
`wdsp_channel_test` immediately.
Without the fourth, every channel close is a use-after-free race on the
worker thread; `wdsp_channel_test` and the HL2 backend tests show it under
ThreadSanitizer.
Without the fifth, the only NNR setting a host outside an RXA channel can
change is the model slot.
Without the sixth, `nnr_controls_test`'s scenario segfaults: a model file with
the wrong dimensions in the working directory leaves slot 0 not-ready, and the
first alpha or knee write goes through a null `df`.
Without the seventh, a stop immediately followed by a start — before the host has
clocked the down-ramp out — silently and permanently disables the channel;
`wdsp_channel_test` shows it deterministically.
Without the eighth, a start taken just *after* that ramp completes does the same
thing by a different route, and in the blocking form hangs the host outright;
`wdsp_channel_test` shows it, and the probe behind the patch measures it.
Without the ninth, closing a channel that was stopped and then clocked frees the
RXA chain under a live `flushChannel` thread; `wdsp_channel_test` crashes.

The tenth changes no filter output: at `mp == 0` the removed object was never
read, and at `mp == 1` the same workspace is built, one call later. It is the
only one of the ten that makes a pointer legitimately null, so the risk it
carries is a null dereference in `mp_imp_exec()` or a double teardown in
`deplan_fircore()`, not a leak. `wdsp_channel_test`'s new
`runMinimumPhaseWorkspaceTest` is the cover: it is the first test in this tree
to set `Config::minimumPhase`, and because `WdspChannel::open()` calls
`RXASetNC()` before `RXASetMP()` it is the case that walks the lazy build. An
ASan build of the vendored library alone, cycling `open` / `RXASetNC` /
`RXASetMP(1)` / `RXASetNC` / `RXASetMP(0)` / `close` twice plus a TX open,
reported nothing on macOS arm64.

**Marking convention.** Patches 4, 7, 8 and 9 carry `// AetherSDR patch N:` comments
at every edited site, so `grep -rn "AetherSDR patch" third_party/wdsp/` finds
them all. Patches 1-3 carry no marker — 1 and 2 are single added `_aligned_free`
lines and 3 is a single added assignment — and are findable only from this file.
New patches use the marker.
 Patch 10 is the exception: its sites are marked `// AetherSDR:` with no
number, so the grep above does not find them; it is findable from this file
and from `ensure_minphase` in `upstream/firmin.c`.

When refreshing WDSP, first check whether upstream has made each change
itself. They are different shapes, so grep for the shape, not for a free:

- **patches 1-2** -- a trailing `_aligned_free()` of the object itself at the
  end of `destroy_notchdb()` / `destroy_nurbs()`.
- **patch 3** -- the `a->pfcimp = build_fcimp (...)` **assignment** in
  `SetRXAFMNCde()` and `SetTXAFMEmphNC()` (TAPR/OpenHPSDR-wdsp#2). This one is
  a use-after-free fixed by capturing a return value, **not a free**: looking
  for an added free will report the fix absent when it has landed, or have you
  reapply a patch upstream already carries.
- **patch 4** -- the `mainGen` / `mainRunGen` / `mainExited` exit handshake
  between the DSP worker and `pre_main_destroy()`.
- **patch 5** -- standalone `set*_nnr()` accessors alongside the `SetRXANNR*`
  properties.
- **patch 6** -- an `n->df` guard in `setAlpha_nnet()` and `setKnee_nnet()`
  (TAPR/OpenHPSDR-wdsp#4, fix in TAPR/OpenHPSDR-wdsp#5).
- **patch 7** -- `SetChannelState()` case 1 clearing a still-pending
  `slew.downflag` / `iob.ch_upslew` down-ramp before it arms the up-ramp
  (TAPR/OpenHPSDR-wdsp#6).
- **patch 8** -- `SetChannelState()` case 1 waiting, outside `csEXCH` and under
  case 0's existing bound, for `exchange` clear **and** `flushflag` set
  (TAPR/OpenHPSDR-wdsp#7).
- **patch 9** -- a `quiesce_flush()` holding the `flush_bypass` / `Sem_Flush`
  handshake, called from `pre_main_destroy()` rather than only from
  `destroy_iobuffs()` (TAPR/OpenHPSDR-wdsp#8).
- **patch 10** -- an `a->mp` guard on the `create_minphase()` call at the end of
  `plan_fircore()`, and a build-on-first-use in `calc_fircore()`.
- **patch 13** -- in `dexchange()`, the `memcpy` out of `r1` (and its
  `r1_outidx` advance) sits **before** the `Sem_OutReady` release, not after
  it; plus the one-line `wdspPortHandoffPauseForTest()` call after the release.
  Look at the ORDER, not for an added line: upstream's fix, if it comes, is a
  move, and the pause call is ours alone.
- **patch 15** -- in `dexchange()`, the `memcpy` into `r2` (and its
  `r2_inidx` advance) sits **before** the `r2_havesamps` increment; plus the
  added read-only `GetChannelOutputReady()` in `iobuffs.c` (declared in
  `iobuffs.h` and `include/aether_wdsp.h`).
- **patch 16** -- `SetTXABandpassFreqs()` in `TXA.c` holding `ch[].csDSP`
  around its body.
- **patch 17** -- in `analyzer.c`: `stop` / `end_dispatcher` read and written
  through `Interlocked*` (and `volatile LONG` in `analyzer.h`); the
  `input_busy` release in `spectra()` / `Cspectra()` sitting **after**
  `stitch(disp)`, not before it; and `sendbuf()`'s `IQO_idx` hand-off and
  `IQout_index` advance inside its existing `BufferControlSection`.
- **patch 18** -- in `destroy_calcc()`, the `SemsPSCorr` and
  `hCorrChangeExited` closes. Patch 22 replaced its `== WAIT_OBJECT_0` check:
  the closes now follow the unbounded exit wait.
- **patch 22** -- in `calcc.c`: the `corrThreadStarted` field set from
  `_beginthread()`'s result in `create_calcc()`, and in `destroy_calcc()` the
  `hCorrChangeExited` wait (clearing iqc's `busy` bit each pass, unbounded
  once patch 4 saw the worker exit) with the six `util` spline frees moved
  **after** it, not before.

Drop any local patch upstream now carries. Otherwise reapply only these minimal
changes and run the lifecycle test under AddressSanitizer on every supported
platform.

## Patch 11 — explicit transmit-buffer discard (#5747)

`upstream/channel.c::DiscardTXAChannelData`, declared in the host-facing
`include/aether_wdsp.h`, serves hosts that stop clocking audio on unkey.
`SetChannelState(0, 0)` cannot flush such a channel: its down-ramp needs further
`fexchange` calls and restart cancels the pending ramp. This previously carried
85 ms of old TX audio into the next transmission.

The caller excludes concurrent exchange/control operations. The discard takes
`csDSP` then `csEXCH`, matching `flushChannel`, disables exchange and processing,
flushes the rings and TX chain, and leaves the channel stopped with plans and
configuration retained. `flush_iobuffs` refreshes its output semaphore; this is
not an allocation-free audio operation. No planner lock, thread, sleep/retry
loop or FFTW planning is added. RX channels and an outstanding asynchronous
flush request are refused, so its worker cannot race a later restart.

Coverage: `hl2_txdsp_test` checks every post-reset sample after immediate and
500 ms restarts, repeated reset, and subsequent tone recovery. The assertion
must fail when discard is removed. `wdsp_channel_test` checks the stopped state,
channel retention, repeated discard, RX/pending-flush refusal, and leaks.
When updating WDSP, retain this local entry point unless upstream supplies an
equivalent synchronous discard with the same locking and refusal contract.

## Host portability allocation accounting

The host-owned `port/wdsp_port.c` retains process-wide allocation/resource
counters and additionally exposes `wdspPortThreadAllocationSequence()`.
`WdspChannel::processIq()` compares the calling thread's counter: preparing a
new channel on the registry pool must not falsely report callback allocation
on a different, already-running receiver. Successful aligned allocations
advance both counters; failed allocations advance neither. RTL materializes
platform TLS on its acquisition thread before entering USB callbacks.
`wdsp_allocation_scope_test` pins local detection and foreign-thread isolation.
This changes host instrumentation, not the upstream DSP snapshot.
## Patch 12 — `create_minphase()` plans with `FFTW_ESTIMATE` (#5498)

`upstream/fir.c::create_minphase()` plans its four transforms with
`FFTW_ESTIMATE` instead of `FFTW_PATIENT`. They run only in `mp_imp_exec()`,
which designs a minimum-phase filter when a core's impulse changes (a filter
edge, a tap count, a mode into or out of minimum phase); nothing executes them
per sample. A measured plan therefore buys nothing, and measuring one is
expensive at these sizes: `nc * pfactor` is 131072 points for the bandpass cores
at `Hl2RxDsp::kRxFilterTaps` = 8192.

Measured on x86_64 with an empty wisdom cache and no planner time limit, one RX
`WdspChannel::create()` at 8192 taps: 36.4 s linear, 103.9 s minimum phase; a
cold `setMinimumPhase(true)` on an open linear channel took 69.3 s. A bare
`fftw_plan_dft_1d(131072, FFTW_PATIENT)` measures ~30 s per direction there;
`FFTW_ESTIMATE` plans the same transform in 0.4 ms and executes four in 2.8 ms.
With this patch, same machine, same empty cache: 10.9 s linear, 10.9 s minimum
phase, and 27 ms for the cold `setMinimumPhase(true)`. The linear open gains too,
because the equaliser core below stops measuring its 65536-point plans. Without
it every HL2 install upgrading to minimum phase pays the difference once on its
next connect, and a first-ever connect on a slow host approaches the HL2
connect's 600 s DSP-setup limit.

It applies to every caller, so it also covers the equaliser core (`eqp`, built
minimum-phase by upstream on every RX and TX channel, `pfactor` 4). The designed
taps are the same transform computed by a different FFT algorithm: they differ
at rounding level only. `hl2_rxdsp_unmute_return_test` and
`hl2_noise_blanker_test` read the same latencies and peaks with it as without.

When updating WDSP, keep this unless upstream stops planning these transforms
with a measuring flag.

## Patch 13 — `dexchange()` takes its input before it releases the host (#5734)

`upstream/iobuffs.c::dexchange()` now copies the worker's input slot out of
`r1` **before** it releases `Sem_OutReady`. Upstream (2.10, and TAPR 1.20 and
Thetis today) does it the other way round: write `r2`, release the host, *then*
read `r1`.

**Why that order is a race.** With `bfo` set, the `Sem_OutReady` release is what
lets the host's blocked `fexchange*` return — and the host's next call writes
the next input block into `r1` before it blocks again. With `in_size ==
dsp_insize` the ring holds `DSP_MULT` = 2 slots, so worker iteration *w* reads
the slot host call *w* wrote and host call *w + 2* writes the same slot again.
Host call *w + 2* can start as soon as call *w + 1* has been released, which is
worker iteration *w*'s release. So between that release and its `memcpy` the
worker is racing the host for the slot it is about to read. A worker preempted
there processes block *w + 2*, or a torn mix of *w* and *w + 2*, in place of
block *w*. Nothing reports it: `*error` is 0 and the output looks like audio.
(The same shape holds for other size ratios. A release of *n* tokens lets the
host write *n* more blocks while the worker has not yet read its own.)

**Measured, #5734.** `wdsp_channel_test::runVector()` clocks two identical
channels in lockstep and requires bit-identical output. Under CPU load on Linux
it diverged in 3 of 6 runs (ten9876, Arch, 32 cores, up to 0.058). On
macOS/arm64 it did not reproduce under load in 64 instrumented `runVector` cases
(24 spinners). The race is real there, but Darwin does not preempt the waker
for the woken host, so the window almost never loses. Two instruments settled
it anyway:

- **Instrumentation** of `fexchange2`/`dexchange` (temporary, not committed)
  counting host `r1` writes around the worker's copy. At the first diverging
  block, the diverging channel's worker had read slot 0 while the host's write
  count went 1 → 3: block 2 landed in the slot during the copy. The other channel
  read cleanly. `exchange`, `exec_bypass`, `flushflag`, `upflag`, `downflag`
  and the `SetChannelState` sequence were identical on both channels at every
  event, so patches 7 and 8 are not involved.
- **Widening the window** reproduces #5734's first diagnostic line exactly:
  RX block 3, `dL=0.00205633 dR=0.00139192 rmsA=0.000593078 rmsB=0`, the same
  digits ten9876 printed on Arch. The `rmsB=0` "channel that wrote nothing" is
  the *correct* channel: block 3 falls inside the 10 ms mute delay-up and is
  exact zeros (0 of 256 samples nonzero). The other channel is the wrong one. It
  processed a later block that was already on the up-ramp.

A standalone harness of the vendored C library in a Linux container (arm64,
kernel 6.4, 16 `nice 5` spinners on 8 vCPUs), two identical RX channels driven
exactly as `runVector` drives them: **33 of 600 trials diverged before this
patch, 0 of 1300 after.** Idle, 0 of 200 each way. The FFTW planner was bounded
at 0.001 s in both builds, so the bounded-planner theory raised on #5734 does
not account for the divergence either.

**Cost.** None measurable. The `memcpy` is the same bytes, done a few hundred
nanoseconds earlier. `out` (the chain's `inbuff`) and `in` (its `outbuff`) are
separate allocations in both `RXA.c` and `TXA.c`, so the reorder changes no
value. `r1_outidx` is touched only by the worker and by `flush_iobuffs()`,
which runs under `csDSP` exactly as `dexchange()` does.

**The test hook.** `port/wdsp_port.c` gains `wdspPortSetHandoffPauseForTest()`
(public in `include/aether_wdsp.h`) and `wdspPortHandoffPauseForTest()`, called
once in `dexchange()` straight after the release. At 0, the shipping value, it
is one relaxed atomic load per DSP block. `wdsp_channel_test`'s
`runWorkerHandoffTest` sets 5 ms and compares against an unpaused channel. With
the reorder reverted it fails on every run (RX block 3 by 0.00205633, TX block
6 by 1.0e-5). With the reorder in place it passes. Keep the call immediately
after the release. Anywhere else, the test stops pinning the ordering.

**Who it reaches: the tests, not the radio.** `dexchange()` releases
`Sem_OutReady` only when `bfo` is set, and every production channel in this
tree is opened non-blocking: `Hl2RxDsp`, `AnanRxDsp` and the RTL registry
default `blockForOutput` to false, and `Hl2TxDsp` sets it false explicitly. So
this patch does not change live audio. What it changes is every **offline,
blocking-mode** test, whose whole premise is that a burst feed gives
deterministic output: `wdsp_channel_test`, `hl2_rxdsp_test`,
`hl2_rxdsp_rate_test`, `hl2_rxdsp_unmute_return_test`, `hl2_cw_bfo_test`,
`hl2_noise_blanker_test` and `hl2_adc_sampling_seam_test` all open with
`blockForOutput = true`, and all of them could process a substituted block
under load. `runVector` is the one that noticed, because it is the only one
that compares two channels rather than one channel against a tolerance.

**Not addressed here:** the non-blocking path (`bfo == 0`), where a host that
outruns the worker overwrites `r1` by design; upstream marks that with its own
`*error += -1` TODO in `fexchange0`/`fexchange2`. The reorder is inert there:
no release, so nothing moves relative to it.

When updating WDSP, keep this unless upstream's `dexchange()` reads `r1` before
it releases `Sem_OutReady`. Keep the pause call (and its declaration at the top
of `iobuffs.c`) regardless: it is AetherSDR's own test surface.

## Patch 14 — the minimum-phase design workspace is freed after each design (#5954)

The HL2 phase policy keeps CW linear and every other mode minimum phase, as
[confirmed by the maintainer](https://github.com/aethersdr/AetherSDR/pull/5954#discussion_r4110668837).
A new `WdspChannel::Mode` takes minimum phase unless the policy lists it.

`upstream/firmin.c`: `plan_fircore()` no longer builds a core's
`create_minphase()` workspace, and `calc_fircore()` frees it with
`destroy_minphase()` straight after `mp_imp_exec()` has used it.
`ensure_minphase()` (patch 10) rebuilds it for the next design.

The workspace is design scratch: seven buffers of `nc * pfactor` elements and
four FFTW plans, read only by `mp_imp_exec()`. Upstream held it for the life of
the core. After #5498 put `Hl2RxDsp`'s bandpass at minimum phase outside CW,
that was the six cores `RXASetMP()` reaches -- three of them 131072 points -- for
every non-CW HL2 receiver. The original measurements on an x86 Linux host,
for one 8192-tap RX channel, heap in use:

| | linear phase | minimum phase |
|---|---|---|
| before this patch | 37.8 MB | 86.7 MB |
| with this patch | **31.3 MB** | **31.3 MB** |
| `setFilter()` at minimum phase, mean of 20 | -- | 9.1 ms -> **14.6 ms** |

The linear channel shrinks too, because the equaliser core (`eqp`, built
minimum-phase by upstream on every RX and TX channel) releases its workspace as
well. The cost is the rebuild on every design: the allocation and zeroing of the
buffers plus four `FFTW_ESTIMATE` plans (patch 12), about +5 ms per passband
change at 8192 taps. It is paid on the control path, never per sample.
Tuning can also pay this cost: `WdspChannel::setShift()` and
`setNotchTuneFrequency()` reach `calc_nbp_lightweight()`, which redesigns the
mask when the enabled notch stage had or has a notch in the passband. Tuning
without those notches does not redesign the filter.

Every caller of `calc_fircore()` is a WDSP control function (`Set*` / `RXA*Set*`,
the notch-database edits in `nbp.c`, `RXAbpsnbaCheck`); none is reached from the
per-block exchange. The host makes every such call under
`WdspChannel`'s `g_setupMutex`, the process-wide FFTW planner lock, so the
re-plan is serialised like any other. A host that calls those functions without
that lock would now plan FFTW concurrently; keep that in mind before adding one.

**The re-plan must not pick up the aligned DSP plans' wisdom.** This patch
adds `FFTW_UNALIGNED` to patch 12's four `create_minphase()` plans.
`FFTW_ESTIMATE` still uses wisdom when a matching plan exists. Re-planning on every design meant an
aligned request could pick up wisdom another plan recorded in between, and
design the same filter with a different FFT algorithm: two identical channels
then differed at rounding level. Measured under `ctest -R 'anan|wdsp' -j16`,
15 loops each: current `main` 0 failing loops; this patch without
`FFTW_UNALIGNED` 15 of 15 (all `wdsp_channel_test`, including 3 × "identical
WDSP channels produced different vectors" and 3 intermittent group-delay
checks, alongside the deterministic patch-10 check restated below); with
`FFTW_UNALIGNED` 0 of 15. No other plan in this process is unaligned, so the key
does not match the aligned DSP plans' wisdom. These design plans can reuse
their own heuristic wisdom; `FFTW_UNALIGNED` also disables SIMD. See
[FFTW planner flags](https://www.fftw.org/fftw3_doc/Planner-Flags.html) and
[wisdom accumulation](https://www.fftw.org/fftw3_doc/Words-of-Wisdom_002dSaving-Plans.html). The
`setFilter()` figure in the table is with it.

Coverage: `wdsp_minphase_workspace_test` requires a minimum-phase channel to
hold the same live WDSP allocations as a linear one after opening, after a
filter change, and after twenty more; removing the free fails it (1256 against
1208), and so does reverting patch 10 (1248 against 1296). `wdsp_channel_test`'s
patch-10 check now counts allocations MADE by each open rather than held after
it -- the minimum-phase open builds and frees the workspace, the linear open
never builds it -- and still fails when patch 10 is reverted.
The workspace test also checks every repeated edit is accepted, that tuning
through an active notch builds and releases scratch, and that teardown returns
the live-allocation counter to its starting value.
When updating WDSP, keep this unless upstream stops holding the workspace.

## Patch 15 — a non-blocking host can ask whether an output block is ready

`upstream/iobuffs.c` gains `GetChannelOutputReady(channel)`: 1 when
`r2_havesamps >= out_size`, i.e. when the next `fexchange*` would find a whole
output block and not underrun. Read-only, under `r2_ControlSection`, which is
the lock `fexchange*` itself takes to read the same count. And `dexchange()`
now copies a produced block into `r2` **before** counting it into
`r2_havesamps`; upstream counts first and copies second.

**Why a non-blocking host needs it.** Every production channel here is opened
with `bfo = 0`, so a host cannot wait inside `fexchange*`. `Hl2TxDsp` is handed
audio in the capture backend's delivery quantum, and Qt 6.8's macOS
`QAudioSource` flushes 4096 bytes (1024 frames of 16-bit stereo) whatever the
device rate. At 24 kHz -- a Bluetooth headset microphone -- that is two TXA
blocks per delivery. Exchanging them back-to-back underruns the second:
`fexchange2` zeroes it, advances `r2_outidx` without consuming, and from then
on the two-slot (`DSP_MULT` = 2) output ring is read out of step with the
worker, so every later block boundary is a splice of a stale or torn block.
The fault counter sees the one underrun and nothing after it. On the simulator
(hl2-lab d161) a 1 kHz tone through a 24 kHz input had a phase discontinuity at
every 21.3 ms block boundary -- 252 in 5.5 s -- with `modulatorFaultBlocks` at 2;
by ear, "chopped" speech. A time-based spacing between exchanges was tried
first and failed under `ctest -j6`: a loaded worker outlasted it. The worker's
own count is the only reliable signal, so the host asks for it.

**Why the reorder has to come with it.** Upstream publishes the count before
the data. That is harmless to a host that runs a whole block behind the worker
(the normal paced case: it reads the slot written a cycle earlier). It is
exactly wrong for a host that exchanges the moment the count says ready: it
could read the slot the worker is still copying. With the copy first, "ready"
means "in the ring". The reorder moves no value and changes no count; it only
moves when the count becomes visible.

**Who it reaches.** `Hl2TxDsp` (TXA build) calls `GetChannelOutputReady()`
through `WdspChannel::outputReady()` before every exchange; nothing else calls
it. The reorder applies to every channel, blocking and non-blocking; in
blocking mode the host is released by `Sem_OutReady`, which is signalled after
both the copy and the count either way. `hl2_txdsp_capture_burst_test` pins
the QUERY and the gate built on it: a 24 kHz capture delivered as Qt delivers
it gives zero faults and a phase-continuous tone with the patch, and 1 fault and
~120 splices in two seconds without it. It does NOT pin the `dexchange()`
reorder: that closes a narrow window between the count and the copy on another
thread, which no deterministic test here reaches. The reorder rests on the
argument above and on one measurement: a scratch host (not committed) that
spins on `GetChannelOutputReady()` and exchanges the instant it reads 1, 4000
blocks a run, saw torn blocks (phase splices) in 3 of 15 runs with upstream's
order and in 0 of 20 with the reorder.

**What the reorder does to RX.** Every RX channel is non-blocking too, but none
asks before it exchanges. The reorder makes the count visible one `memcpy` of
`r2_insize` complex samples later (microseconds) and moves nothing else: the
priming, the ring indices and `Sem_OutReady` are untouched, so no channel gains
latency. The only behaviour that differs is a host exchange landing inside that
window, which can happen only when the worker is already a whole block late: it
now underruns (counted) where upstream's order handed it a torn block (silent).
`wdsp_*` and `hl2_rxdsp*` tests pass unchanged.

**Upstream status.** Not reported. TAPR/OpenHPSDR-wdsp 2.10 `dexchange()` still
counts before it copies and has no readiness query.

When updating WDSP, keep this unless upstream grows an equivalent query; keep
the reorder unless upstream's `dexchange()` already copies before it counts.

## Patch 16 — the TX bandpass setter takes the DSP lock (#6156)

`upstream/TXA.c`: `SetTXABandpassFreqs()` now holds `ch[channel].csDSP` around
its body, as `SetTXAMode()` already does around the same `TXASetupBPFilters()`
call. So do `SetTXACompressorRun()` and `SetTXAosctrlRun()`;
`SetTXABandpassFreqs()` was the only caller of it that took no lock.

**Why it matters on air.** `TXASetupBPFilters()` clears `bp1` and `bp2`'s
`run` first and sets them again only after `CalcBandpassFilter()` has
redesigned the FIR. `xbandpass()` with `run` clear copies input to output. The
DSP worker runs `xtxa()` under `csDSP`, so before this patch a filter change
made while transmitting with the compressor (and CESSB) on could pass one or
more TX blocks with the post-compressor bandpass bypassed: a burst of
out-of-band splatter. The FIR coefficients themselves were never torn, since
`fircore` double-buffers them under `a->update`; the `run` flags were the hole.

**Found by** the TSan lane once it could run the full tree: `wdsp_channel_test`
reported `TXASetupBPFilters` (`TXA.c` writes of `bp0/1/2.p->run`) racing
`xbandpass` on the worker, with no lock in common. The port's critical
sections are plain `pthread_mutex`, so the report was not a shim artefact.

**Cost and lock order.** The worker waits for one FIR redesign per TX filter
change, the cost upstream already accepts in `SetTXAMode()`. No new lock edge:
`WdspChannel::setFilter()` holds `g_setupMutex` and now takes `csDSP` inside
it, the same order `setMode()` and `open()` already use; `csDSP` is recursive.

**Upstream status.** Not reported.

When updating WDSP, keep this unless upstream's `SetTXABandpassFreqs()` takes
`csDSP` itself.

## Patch 17 — the panadapter analyzer's dispatcher hand-offs are synchronised (#6156)

Three races in `upstream/analyzer.c`, all reported by the TSan lane against
`AnanPanAnalyzer` (`anan_rxdsp_handedness_test`, `spectrum_sequence_gap_test`,
`wdsp_process_tally_test`). The port's `Interlocked*` are `__atomic` seq_cst and
its threads are `pthread_create`, so TSan saw these accurately.

1. **`stop` and `end_dispatcher` are atomic.** Both were plain `int`, written
   by `SetAnalyzer()` / `DestroyAnalyzer()` and polled by `sendbuf()`'s loop and
   the workers' early-outs. The drain itself was already sound (the
   `dispatcher` bit and `pnum_threads` are atomic), so this one is a formal
   race rather than a symptom; it is fixed because it was most of the lane's
   noise and an unsynchronised polled flag is undefined behaviour the compiler
   may hoist. Now `volatile LONG`, written with `InterlockedExchange()` and
   read with `InterlockedAnd(&x, 1)`, the idiom upstream already uses for
   `dispatcher`.
2. **`input_busy` is released after the stitch, not before.** When the last
   FFT of a frame finished, `spectra()` / `Cspectra()` cleared every
   `input_busy` bit and then called `stitch()`. `sendbuf()` could therefore
   dispatch the next frame's workers while the stitch was still reading
   `result[]` and `ss_bins` — `Celiminate()` writing what `stitch()` was
   copying, i.e. a torn panadapter frame — and could let two `stitch()` calls
   overlap on `w_pix_buff` / `last_pix_buff`. Clearing after `stitch()` makes
   the next dispatch wait for it. **This is the only part that changes
   timing:** the next frame's FFTs start one stitch later (tens of
   microseconds). `stitch()` waits on nothing the dispatcher holds, so it
   cannot deadlock.
3. **`sendbuf()` hands off the read index under the buffer lock.**
   `Spectrum0()` moves `IQout_index` under `BufferControlSection` when it skips
   ahead on an overrun; `sendbuf()` read it into `IQO_idx` and advanced it
   unlocked, a lost update that points one FFT at the wrong span of the
   sample ring. The read, the advance and the existing `have_samples`
   decrement now share one `BufferControlSection`, taken before the worker is
   queued so the worker still sees its `IQO_idx` through the thread start.

The `AnanPanAnalyzer` teardown (a `SetAnalyzer()` drain before
`DestroyAnalyzer()`) is unchanged and was already correct.

**Upstream status.** Not reported.

When updating WDSP, keep all three unless upstream's analyzer synchronises the
same hand-offs.

## Patch 18 — the PureSignal correction thread's handles are closed (#6154)

`upstream/calcc.c`: `create_calcc()` (called by `create_txa()` for every TX
channel) creates five `SemsPSCorr` semaphores for the `doPSCorrChange()`
thread, and `destroy_calcc()` never closed them. That leaked five port handles,
each a mutex, condition variable and allocation, on every TXA channel
destroy. LeakSanitizer reports 86 leak blocks across 17 tests (`wdsp_channel_test`,
the `hl2_*` TX/DSP tests, the `radiomodel_*_null` tests).

`destroy_calcc()` now closes the five semaphores and the exit event **only when
the thread-exit wait succeeds**. `doPSCorrChange()`'s last act is
`SetEvent(hCorrChangeExited)` and `return`, so once that event is seen nothing
touches either again. If the 500 ms wait times out, the thread may still be
blocked in `WaitForMultipleObjects` on those semaphores, so they are not freed
under it; upstream closed the event unconditionally, which had the same hazard
for a thread that later reached `SetEvent()`, and it is now under the same check.

**This does not make the timeout path safe, and does not claim to.** After the
wait, `destroy_calcc()` goes on to free `a` and its members, which a correction
thread still running past the timeout would use. That use-after-free predates
this patch (upstream frees `a` on the same path) and is tracked separately
(#6179). Patch 18 only stops the normal, successful-exit path from
leaking and avoids adding a second hazard on the timeout path. Patch 22 closes
the timeout path by removing it.

**Upstream status.** Not reported.

When updating WDSP, keep this unless upstream's `destroy_calcc()` closes the
semaphores itself.

## Patch 19 — opt-in WFM reception and live decoder controls (RFC #5468)

`upstream/wbfm.c` and `upstream/wbfm.h`, against the pinned 2.10 revision above.

`SetRXAWBFMdmph()` previously changed only the parent fields used during
construction. Existing `dmphL` and `dmphR` kept their original run flag, time
constant and coefficients. A generated 1kHz/15kHz vector measured identical
15kHz amplitude after requesting75us then50us (`0.0413053`, ratio1.0) on
Nobara before this patch. The expected analog ratio at15kHz is approximately
1.482. The setter now updates both active filters under the existing DSP
critical section. It resets their history when the setting changes; no
allocation, FIR planning or third-party dependency is introduced.

The existing stereo-indicator getter acquired the DSP mutex. The RTL sample
path must observe decoder state without waiting on that worker. A separate
interlocked `stereoPublished` value is published after `xwbfm()` completes its
L/R output and read with the port's existing atomic-load idiom. This is a
latest completed decoder-block observation, not an exact timestamp for audio
already buffered in the output ring. The caller still owns channel lifetime.

`flush_wbfm()` previously cleared the nested pilot detector but retained its
parent stereo/magnitude fields and internal squelch noise/gain histories. It
now resets those histories and publishes no stereo immediately, so a new
station cannot inherit the old station's pilot observation or squelch state.
The decoder's existing automatic-squelch thresholds and50ms gain slew are
unchanged; no new user threshold mapping is added.

The optional `WdspChannel::WbfmReceive` recipe fixes its geometry at 384 kHz
input (2048 complex frames), 192 kHz DSP (1024 frames), and paired 48 kHz audio
(256 frames). It applies an existing 2049-tap Blackman-Harris overlap-save
`BPS` FIR before WDSP's input conversion. WBFM disables RXA's normal RF
bandpass, so configuring that dormant filter did not provide selectivity.
The facade only exposes the existing BPS functions; vendor filter code is
unchanged. The preallocated FFT output holds twice the input frame count.
Filter setup, changes, and destruction use the existing planner/control fence;
processing adds no acquisition allocation or mutex. Mathematical complex-IQ
edges are negated/swapped to match WDSP's filter convention. A 3 kHz transition
allowance is tested on both sides and on an asymmetric passband.

`WbfmReceive::outputGain`, default 0.25, scales both output channels before
receiver taps. WBFM bypasses RXA's output panel, so the usual panel-gain API
could not supply this normalization. The pre-fix 90% mono fixture peaked at
3.25825 with 76000 clipped frames; with normalization it peaked at 0.814467
and no frames reached unity. This is scalar normalization, not a hidden clipper.

`SetRXAWBFMDiscriminatorCompensation()` defaults off and is called only by
this recipe. Adjacent phase differences average the continuous instantaneous
frequency over one sample, producing sinc(omega/2) droop. At 38 kHz in the
192 kHz decoder, that attenuated L-R enough to give 29.218/29.385 dB separation
on analytically integrated 1/2 kHz stereo. A rectangular phase-sum fixture
at the DSP rate accidentally cancels this error and must not be used as proof.

The opt-in correction is a 13-tap linear-phase inverse-sinc approximation.
Its coefficients are the polynomial sum for k=0..6 of
`C(2k,k)/(4^k*(2k+1)) * ((2-z-z^-1)/4)^k`. It has unity DC gain, six samples
of common delay, and less than 0.0047 dB residual response error through the
53 kHz multiplex band. Its maximum gain is 1.356417, so it also raises
high-frequency noise by up to 2.65 dB; this is the bounded noise tradeoff,
not additional recovered information. The implementation uses only fixed
history and arithmetic. Before the subsequent DC correction, measured 1/2 kHz
separation increased to 42.23/47.95 dB and 14/15 kHz reached 51.16/47.87 dB.

The same opt-in flag moves the existing DC blocker after the L/R matrix,
with independent identical histories and the original 0.9995 pole. Leaving it
before the split rotated low-frequency L+R while barely rotating L-R near
38 kHz. The generated 300/500 Hz vector measured 31.8839/36.3027 dB before
this change, matching the calculated phase error. Legacy owners retain the
original MPX placement. Both paired histories reset on calculation, flush,
and recipe selection.

The 192 kHz pre-discriminator IQ conversion still limits the effective RF
passband. A requested +/-100 kHz FIR does not mean a flat 200 kHz signal path:
the generated carrier response was flat through +/-90 kHz, approximately
-0.195 dB at 93 kHz and -6.02 dB at 96 kHz. Near-full stereo modulation loses
some sidebands beyond that range. Its measured separation is reported by the
test as a limitation; the RFC's >=40 dB requirement remains pinned on the
explicit reference vectors. Moving discrimination before IQ conversion would
address a different, broader change and is not part of this patch.

The socket-free `wdsp_wbfm_test` exercises RF selection and envelope, normalized
headroom, live regional response, reference separation at low/mid/high audio
frequencies, pilot acquisition/loss, mono fallback, restart, per-channel
isolation, and allocation-free processing. Additional diagnostic vectors cover
near-full modulation, +/-100 ppm signal clock, coherent pilot/subcarrier
offsets, seeded weak/marginal noise, and pilot-absent reception. They are
synthetic DSP evidence, not hardware, on-air, or universal reception claims.
The old WFM mode and other channel owners keep their existing configuration,
gain, and discriminator/DC behavior.

On refresh, retain the active-filter update until upstream applies both live
filters, retain the complete flush, and retain atomic stereo publication unless
upstream supplies an equivalent nonblocking observation. Do not restore a DSP
mutex to the acquisition-side getter. Keep the discriminator/DC changes
explicitly opt-in unless a separately reviewed upstream equivalent preserves
both continuous-waveform response and low-frequency stereo phase. Regenerate
the FIR from its polynomial and rerun analytic fixtures if its geometry changes.


## Patch 20 — prepared WFM exchange headroom and output publication (RFC #5468)

`upstream/channel.{h,c}` and `upstream/iobuffs.c` add an immutable prepared
`exchangeDepth` per channel. `OpenChannelWithExchangeDepth()` rejects channel
indices outside the table and depths outside 2..8 before modifying any state,
then shares the existing construction body. Its other arguments retain the
ordinary `OpenChannel()` contract. Every legacy `OpenChannel()` explicitly
selects `DSP_MULT` (still 2), including a reused former WFM slot. Existing
size/rate rebuilds retain the prepared field. Both creation and flush seed the
same output prefix from `r2_active_buffsize - r2_size`.

The facade selects 8 only for its opt-in nonblocking `WbfmReceive` recipe:
384 kHz / 2048 IQ input, 192 kHz / 1024 DSP, 48 kHz / 256 paired output.
Blocking WFM, legacy WBFM, other RX, and TX retain depth 2. Measured continuous
8192-sample extraction callbacks realize a maximum of seven complete IQ
exchanges at the lowest approved capture rates (225001 and 250000 Hz). The
composed converter bound is seven; no wall-clock scheduler assumption is used
to derive it. The tests-only RED against `2e44eb04` accepted two exchanges with
the worker held after its first handoff, then reported the actual `Underrun`.
Live first-fault telemetry had independently observed `Underrun` with continuous
capture, so a missing capture packet is not needed to produce this failure.

At this geometry the two rings occupy 288 KiB per depth-8 channel, versus
72 KiB at depth 2: 216 KiB additional prepared memory. Seven seeded output
blocks plus the existing previous-chain-output worker stage delay audio by
2048 frames / 42.667 ms, exactly six blocks / 1536 frames / 32 ms more than
blocking depth 2. These are exchange-stage costs only; RF filtering, rate
conversion, decoder, mixer waiting and device buffering are additional. One
callback's bound does not promise survival of arbitrary host starvation or
multiple queued callbacks without worker progress. Depth 8 is not a relaxed
mixer deadline and does not extend the existing 2048-frame hole-wait policy.

For equal exchange/DSP transfers, S successful host calls and P worker
handoffs satisfy S-P <= D-1. The worker has copied each input before publishing
its corresponding credit. The next host write therefore cannot overwrite an
unread input before the first failure. The failing call can occupy the final
slot, so no further call is safe: every non-OK still withdraws that receiver,
and repair prepares a new epoch off acquisition. No retries or substitute silence on underrun,
callback allocations, new waits or new threads are introduced.

A separate existing publication race incremented `r2_havesamps` before copying
the output bytes. `dexchange()` now copies the output and advances its private
write index first, then publishes credit under the existing count lock. The
copy remains outside that lock. This shared correctness repair is not claimed
as the established cause of the observed underrun.

The default-inactive port rendezvous immediately before `copy_output_to_ring`
copies bytes and the test/control-only locked output-count readback provide
scheduler-independent regression barriers. A test-only `csDSP` rendezvous,
after count readiness and before arming a hold, also lets the previous block's
post-publication hook finish, so it cannot claim the next block's hold. The
count query takes `csEXCH` then the count lock to cover asynchronous flush.
Production calls none of these readiness helpers. Fixtures serialize the global
holds, keep other workers idle, and release before teardown/reprepare.
`wdsp_wbfm_test` compares nonzero paired output against the unchanged blocking
reference through a warm eight-exchange held burst, wraparound, and release,
with the exact six-block displacement removed. It checks repeated preparation,
clocked flush, legacy slot reuse, bounded API refusal, and genuinely unpublished
output: seven seeded calls succeed, the eighth must underrun while the copy is
held. Moving publication before the helper must fail that assertion.
`rtl_wfm_pipeline_test` acknowledges the first completed handoff, accepts eight
calls total, then requires the ninth's actual underrun, one typed first fault,
receiver withdrawal, no stale PCM, and a fresh nonzero stereo PCM repair epoch.
These are socket-free deterministic DSP tests, not hardware latency proof.

On upstream refresh, retain the explicit legacy depth reset, prepared field on
all rebuilds, matching create/flush prefix, and bytes-before-credit edge unless
upstream supplies equivalent semantics. Do not increase global `DSP_MULT`,
weaken non-OK withdrawal, or move the test readiness query onto acquisition.


## Patch 21 — selected WFM mono and bounded pilot reception snapshot

Local changes to `upstream/wbfm.c`, `upstream/wbfm.h`, the public WDSP header,
and the narrow `include/aether_wdsp.h` / `aether_wbfm_observation.h` facade,
against the same pinned upstream 2.10 revision. This is an AetherSDR extension,
not an upstream PLL or a calibrated signal-quality measurement.

`SetRXAWBFMForceMono` selects the existing L+R-only matrix branch while leaving
INDY and automatic squelch running. The default remains Auto Stereo. RTL sets
this only while preparing an immutable receiver recipe; accepted state and
persistence follow the matching receiver revision, including failure/rollback.
A changed recipe retires both slice and speaker PCM epochs. Other WDSP owners
retain the false default, and rebuilding a configured receiver reapplies its
selection. Mono is real identical-channel audio, not merely a changed badge. Packet/model
stereo status describes the actual selected output: forced mono remains Mono
even when the independent reception diagnostics report an acquired pilot.

`GetRXAWBFMReception` publishes one coherent latest-completed-block snapshot.
The worker publishes a fixed sequence plus 17 interlocked 32-bit payload words;
each exact double is copied into two words, never read nonatomically across
threads. A reader makes at most four attempts and returns unavailable on a
collision. No DSP mutex, wait, allocation, wall clock or logging enters the
acquisition path. Both publication and reading use the port's existing
sequentially consistent interlocked operations. The sequence advances only on
completed blocks or an explicitly invalidating reset, allowing backend freshness
to reject repeated cached observations while exchange credits are consumed.

The amplitude is INDY's actual `sqrt(filtIstable^2 + filtQstable^2)` after the
19 kHz filter and before pilot AGC, in FM-discriminator units normalized to
75 kHz peak deviation. It is not RF power, dBm, dBFS, SNR, a quality percentage,
or PLL phase/frequency/lock. Exact low/high thresholds and consecutive-low/high
block counters plus their configured off/on block counts are included. At
192 kHz / 1,024 frames those thresholds are 0.03/0.06 and the integer hysteresis
counts are 187/93; readback uses the actual fields rather than duplicated UI
constants. Absent pilot is ordinary mono fallback.

Lock duration, observation duration and time since either indicator transition
are computed from processed DSP samples. The recent stable duration is capped
at 5,000 ms. Loss and reacquisition counts cover every completed block between
UI updates; the first acquisition is excluded from reacquisitions. Durations
and counts saturate at `INT32_MAX`. Construction, flush, rate/size rebuild and
new receiver lifetimes invalidate/reset the measurements. These durations do
not include audio-ring/device latency or assert wall-clock continuity.

The RTL owner publishes diagnostics at most four times per second plus actual
indicator transitions/invalidation, under the accepted PCM session/revision.
Only a new valid decoder sequence refreshes its 500 ms freshness clock. Retune,
park, mode/session changes and expiry clear the model's reception claim; raw
measurements are never persisted. Processing health remains a separate surface.

Generated `wdsp_wbfm_test` vectors cover exact nonzero paired Mono output with
an honestly retained pilot, Auto Stereo restoration, measured hysteresis,
loss/reacquisition and flush reset. `rtl_wfm_pipeline_test` checks both the
independent tap and speaker with new epochs. Settings/model fixtures cover
request-versus-adoption, strict boolean decoding, old-document defaults, replay
freshness and retired observations. These tests are added validation obligations;
their results must come from a subsequent exact-source Nobara run. On upstream
refresh retain this extension unless equivalent typed, bounded, race-free
semantics replace it, and do not label INDY as a PLL.

## Patch 22 — `destroy_calcc()` frees nothing the correction thread still uses (#6179)

`upstream/calcc.c`. `destroy_calcc()` waited at most 500 ms for the
`doPSCorrChange()` thread to signal `hCorrChangeExited`, then freed `a` and its
members anyway. The thread reads `a`, the six `util` splines, `txa[].iqc` and
`txa[].calcc.cs_update`, all freed by `destroy_calcc()` / `destroy_txa()`
right after. A thread in `calc()` or in correction-file I/O past the 500 ms
went on using freed memory. Upstream also freed the six `util` splines
**before** the wait, under a thread in `PSSaveCorr` / `PSRestoreCorr` work.

Now:

- `create_calcc()` records whether `_beginthread()` started the thread
  (`corrThreadStarted`). A thread that never started is not waited for, which
  is the reason patches 4 and 9 bound their waits.
- `destroy_calcc()` waits for `hCorrChangeExited`, clearing iqc's `busy` bit
  on each 1 ms pass, because `SetTXAiqcStart()` / `SetTXAiqcSwap()` spin on
  it for a DSP worker that `CloseChannel()` has already stopped. With the four
  work semaphores drained first, the exit token is the next one the thread
  takes once its current work ends.
- The wait has **no time limit only when patch 4's worker wait succeeded**
  (`mainExited == mainGen`). Those iqc setters take `ch[].csDSP`, which is
  free only once the worker has exited (patch 9). If patch 4's wait fell
  through its cap, a wedged worker may still hold `csDSP`, an unbounded wait
  could then never end, and a bound of 500 one-millisecond passes stands
  instead: 500 ms on POSIX, up to ~8 s under Windows' default 15.6 ms timer
  tick, since nothing in AetherSDR calls `timeBeginPeriod`. That path
  is already undefined on `main` (`post_main_destroy` deletes `csDSP` under a
  live holder); this patch does not make it worse.
- A `WAIT_FAILED` return (an exit event `CreateEvent` failed to make) ends
  the wait rather than spinning.
- The `SemsPSCorr` / exit-event closes (patch 18) and the six `util` spline
  frees follow the exit.

**What can still stall teardown.** The thread's own work, on the normal path:
`calc()` is bounded CPU work. Correction-file I/O can block as long as the file system does.
AetherSDR calls neither `PSSaveCorr` nor `PSRestoreCorr` today; a caller that
adds them owns that stall.

**Reproduced before fixing**, locally: no `ci.yml` filter runs the test, and
it covers the wait, not the reordering of the frees after it (that half is the
sanitizer lanes'). `wdsp_calcc_teardown_test` holds the thread in
`PSRestoreCorr()` on a FIFO with no writer, closes the channel on another
thread, and opens the writer 1 s later. Before this patch `CloseChannel()`
returned after ~500 ms while the thread was still blocked. With it,
`CloseChannel()` returns only after the writer opens. POSIX only (`mkfifo`);
skipped on Windows. The test queues the restore through
`WdspChannel::restorePureSignalCorrectionForTest()`, so it links under
`AETHER_SHARED_CORE` and the sanitizer lanes build it.

**Upstream status.** Not reported.

When updating WDSP, keep this unless upstream's `destroy_calcc()` joins the
correction thread before freeing.
