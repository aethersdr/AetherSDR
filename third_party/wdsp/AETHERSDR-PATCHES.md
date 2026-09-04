# AetherSDR patches to WDSP 2.00

The source snapshot is pinned to TAPR/OpenHPSDR-wdsp commit
`584e8aca5ba1c4c6bc66fc0cc164ce567c8ba1e3` (`Release Version 2.00`).
AetherSDR carries four teardown fixes in the otherwise exact `Source/*.[ch]`
snapshot:

1. `upstream/nbp.c`: `destroy_notchdb()` now frees the `notchdb` object after
   its member allocations.
2. `upstream/nurbs.c`: `destroy_nurbs()` now frees the `nurbs` object after its
   member allocations.
3. `upstream/cfir.c`: `cfir_impulse()` now frees its temporary transition table
   before returning the generated impulse.
4. `upstream/channel.h`, `upstream/main.c`, `upstream/channel.c`: an exit
   handshake between `wdspmain()` and `pre_main_destroy()`. Upstream's only
   barrier between the detached worker's exit and `destroy_main()` /
   `post_main_destroy()` freeing the semaphore, mutex and buffers it still
   touches was `Sleep(25)` — a scheduling bet, not synchronization, and under
   load or a sanitizer the worker is still in `pthread_cond_wait()` on freed
   memory. `struct _ch` gains `mainExited`; `wdspmain()` stores 1 to it as its
   last statement; `start_thread()` resets it before every `_beginthread`
   (so the `SetInputBuffsize` / `SetDSPBuffsize` / `SetInputSamplerate` /
   `SetDSPSamplerate` rebuilds are covered too); `pre_main_destroy()` polls it
   with a 1 s cap and then falls through, because upstream ignores thread-
   creation failure and an unbounded wait would hang `CloseChannel()`. The
   port's Interlocked shims are seq_cst `__atomic_*` builtins, so the edge is
   real to TSan, not merely quiet. `flushChannel()` has the same detached
   shape and no handshake; it has not surfaced, and gets the same treatment
   if it does.

Without these lines, opening and closing one RX channel leaks one `notchdb`
object and two NURBS objects, while one TX channel leaks one transition table.
`wdsp_channel_test` detects both paths deterministically.
Without the fourth, every channel close is a use-after-free race on the
worker thread; `wdsp_channel_test` and the HL2 backend tests show it under
ThreadSanitizer.

When refreshing WDSP, first check whether upstream contains equivalent frees.
If it does, drop the corresponding local patch. Otherwise reapply only these
minimal fixes and run the lifecycle test under AddressSanitizer on every supported
platform.
