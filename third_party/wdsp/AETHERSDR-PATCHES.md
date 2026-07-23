# AetherSDR patches to WDSP 2.00

The source snapshot is pinned to TAPR/OpenHPSDR-wdsp commit
`584e8aca5ba1c4c6bc66fc0cc164ce567c8ba1e3` (`Release Version 2.00`).
AetherSDR carries three teardown fixes in the otherwise exact `Source/*.[ch]`
snapshot:

1. `upstream/nbp.c`: `destroy_notchdb()` now frees the `notchdb` object after
   its member allocations.
2. `upstream/nurbs.c`: `destroy_nurbs()` now frees the `nurbs` object after its
   member allocations.
3. `upstream/cfir.c`: `cfir_impulse()` now frees its temporary transition table
   before returning the generated impulse.

Without these lines, opening and closing one RX channel leaks one `notchdb`
object and two NURBS objects, while one TX channel leaks one transition table.
`wdsp_channel_test` detects both paths deterministically.

When refreshing WDSP, first check whether upstream contains equivalent frees.
If it does, drop the corresponding local patch. Otherwise reapply only these
minimal fixes and run the lifecycle test under AddressSanitizer on every supported
platform.
