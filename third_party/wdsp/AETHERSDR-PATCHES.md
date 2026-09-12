# AetherSDR patches to WDSP 2.10

The source snapshot is pinned to TAPR/OpenHPSDR-wdsp commit
`b02d5bac675dd2f33ec2bab2b339f79a597c47dd` (`Release Version 2.10`).
AetherSDR carries eight local changes in the otherwise exact `Source/*.[ch]`
snapshot — three teardown corrections, two null/lifetime fixes, one added
accessor set, and two channel-state fixes:

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

**Marking convention.** Patches 4, 7 and 8 carry `// AetherSDR patch N:` comments
at every edited site, so `grep -rn "AetherSDR patch" third_party/wdsp/` finds
them all. Patches 1-3 carry no marker — 1 and 2 are single added `_aligned_free`
lines and 3 is a single added assignment — and are findable only from this file.
New patches use the marker.

When refreshing WDSP, first check whether upstream contains equivalent frees.
If it does, drop the corresponding local patch. Otherwise reapply only these
minimal fixes and run the lifecycle test under AddressSanitizer on every supported
platform.
