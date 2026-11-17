# FLEX-6000 qualification handoff

Status: development; the FLEX-8600 results do not qualify a FLEX-6000 install.
The target floor is the supplied FLEX-6300 running Linux 2.6.37 on a single
Cortex-A8. The maintainer's teammate owns hardware execution; direct access
from this development host is not required.

## Artifacts and limits

Build the native ARMv7 bundle from a pinned source revision:

```sh
python3 tools/flex-tailnet-shim/native/build.py --output build/native-pd --small --with-aetherpd
```

This builds the ordinary shim, DSP, unkey guardian and API lease guardian. It does not provision
the privileged session provider. Do not install the FLEX-8600 development
bridge, grant the Go process root, or copy its network profile to a 6000.
The capture constructor's fixed `172.30.2.1`/`eth0.10` profile is measured
only on the 8600. The 6300 probe reported `172.30.1.1`; that alone does not
identify the complete FPGA IQ path.

The complete ARMv7 development runtime from signed source `aa5a33fc5` is
9,404,028 bytes; its expanded executables/licenses occupy 22,836,670 bytes in RAM. Both ARMv7 and
ARM64 runtime releases reproduced byte for byte with pinned compilers on the
same host. This does not qualify the first-install adapter or radio operation.
See [reproducible runtime qualification](./NATIVE-IDENTITY-DURABILITY-RUNTIME-QUALIFICATION.json)
and [installed ARM64 qualification](./NATIVE-IDENTITY-DURABILITY-INSTALLED-QUALIFICATION.json).
Earlier [restart qualification](./RESTART-RUNTIME-QUALIFICATION.md) remains
bound to its original source. This runtime adds native identity directory sync
and uncertain-commit refusal to the Go shim. The other ten expanded payload
records, boot helper and service definitions are byte-identical to its predecessor.
The installed report includes receive-only whole-service stop/start and fresh
local IQ on the 8600. The earlier [lifecycle report](./WHOLE-RUNTIME-STOP-QUALIFICATION.json)
preserves the predecessor's results and failed observer attempt.

The calculated ARMv7 successor-upgrade budget is up to 39,690,240 bytes,
including the existing compacted installation, stated state/metadata allowances,
transient files and the native 8 MiB reserve. Against a conservative decimal
interpretation of the reported 41 MB free, only 1,309,760 bytes remain.
One extra full release archive does not fit. The final 6000 adapter must account
for firmware-retained upload/extraction copies as well as signed input staging;
that adapter and first-install high-water measurement remain unqualified.
Other 6000 models may have much smaller partitions.
Keep expanded executables, logs, packet captures and caches in RAM. New native writes cap each configuration/identity file at 64 KiB, including
encoding overhead, and reuse fixed temporary siblings. That bounds these new
file data writes to 256 KiB including temporaries; it does not enforce the full
state/metadata allowance or remove legacy leftovers. Persistent identity and
atomic update/rollback still need an actually qualified 6000 installer.
Native writes now sync the containing directory before mutation and after the
file sync and rename. An uncertain configuration commit preserves the file but
blocks control/enrollment and retires the owned PD lifetime. The [component report](./NATIVE-IDENTITY-DURABILITY-QUALIFICATION.json)
includes injected failures and retry/owner-refusal checks; the 8600 installed
report includes an ordinary save with the same effective routes and owner.
Neither establishes actual 6300 directory-sync behavior or physical power-loss
durability of the complete installation and its ancestor directories.

The 8600 first-install carrier/acceptor still requires `no_new_privs` in
`boot/pd_carrier.c`, `boot/pd_accept.c` and `boot/pd_first_request.c`.
[Linux introduced that flag in 3.5](https://www.kernel.org/doc/html/latest/userspace-api/no_new_privs.html),
so those entry points must not be treated as the Linux 2.6 legacy adapter.
The first-request packager accepts ELF64, its native acceptor refuses non-ARM64,
and the boot helper's first-install transaction also refuses ARMv7. Building an
ARMv7 runtime does not supply those missing delivery/installation paths.
The existing capture worker's strict-seccomp design below is a separate mechanism;
do not replace it with an assumption that SO_LOCK_FILTER is required.

The boot helper's tmpfs admission guard is component-qualified separately in
[the boot storage report](./BOOT-TMPFS-QUALIFICATION.json) and installed on the
8600 in the package above. Before installing a final candidate, verify that both
`/run` and the runtime directory use tmpfs. The supplied 6300 report established
tmpfs for `/tmp` and `/dev/shm`, but did not report `/run`. If its layout differs,
the fixed ARMv7 runtime path/adapter needs qualification; do not silently place
the expanded payload on persistent storage. ARMv7 execution of a fixture on an
8600 is not evidence about that layout on a 6300.

## Receive-only qualification first

1. Record firmware, kernel, CPU features, available memory, mount options,
   free space, waveform UID/GID and capability masks. On Linux before 3.14,
   absent `MemAvailable` is not zero available RAM; retain `MemFree`, `Cached`
   and `Buffers` separately. The [read-only kernel probe](./session/README.md#read-only-kernel-probe)
   package 0.1.3 reports type and available capacity for six fixed filesystem
   paths without root access. Its report distinguishes query failure (`null`
   and errno) from successfully measured zero capacity. Before installation,
   an absent `/run/aetherpd` is expected; `/run` must be assessed independently.
   The probe's [8600 qualification](./KERNEL-FILESYSTEM-PROBE-QUALIFICATION.json)
   does not establish the 6300's layout. Never include credentials, environment dumps,
   Tailscale state or private keys in a report.
2. Establish an authorized provisioning mechanism. The ordinary waveform
   UID78666 and autostart/persistence evidence do not establish setup privileges.
   Qualify the operator-approved fixed bootstrap and retain its exact plan and
   signed artifacts. No generic root command bridge or permanent SUID setup is
   part of this handoff. The ARM64 carrier is not a FLEX-6000 delivery adapter.
3. Measure the internal interface, local destination, FPGA source IP/port,
   VITA stream/class and IQ byte order through documented APIs and packet
   observations. Do not access proprietary process memory or registers.
4. Exercise actual required socket/kernel features. The current ring worker
   requires TPACKET_V2, monotonic timerfd, a working clock child and strict
   seccomp after closing its raw descriptor. It does not use SO_LOCK_FILTER
   or no_new_privs as its capture isolation mechanism. Run the read-only
   `session/kernel_probe.c` helper as the actual waveform user to report
   capability masks, strict-seccomp entry and timerfd execution. Packet-ring
   setup still requires a separate authorized test. Missing required features
   fail closed; ARMv7 compilation is not a pass.
   Probe 0.1.1 also reports the current scheduler, priority, nice value and
   RTPRIO/RTTIME/NICE resource limits without changing them. The ordinary
   8600 waveform user reported SCHED_OTHER/priority0, RTPRIO soft/hard0 and
   NICE soft/hard0: ordinary waveform startup did not inherit real-time
   scheduling authority. Unlimited limits have explicit boolean flags; do
   not interpret their numeric placeholder zero as a finite zero limit.
   Scheduler queries use direct read-only syscalls because the bundled
   musl wrappers can return ENOSYS without querying the kernel.
5. Confirm local IQ under normal RX load, including packet sequence, receive
   queue drops, maximum age, CPU and RSS. Then exercise owner/helper/client
   death, API loss, canceled arms and stop while unkeyed. Confirm exact ARP,
   streams, processes and receive-state cleanup after every case.
6. Verify reboot/autostart and identity persistence independently of the RF
   session. No reboot may rekey, rearm calibration or resume transmission.

The supplied Go 1.27.1 probe executed on this 6300, but Linux 2.6.37 remains
below Go's supported kernel floor. Exercise the complete linked shim under
load; a probe's successful startup is not runtime support certification.
Keep local IQ outside Tailscale: the supplied 9.8 MB/s crypto measurement is
for remote relaying and is not a local feedback latency measurement.

The [ordinary-user CPU diagnostic](./session/README.md#ordinary-user-cpu-regression-probe)
runs the five existing regressions without SSH or a privileged helper. Its
330,383-byte native package reproduced in two source/output/cache paths with
Zig 0.17.1; the reviewed SHA256 is
`b1136f77a78e0828c6904c41a8add2258459ffe62dd5063ea544826fe6be8074`.
The same install/read/uninstall path has receive-only FLEX-8600 evidence in
[the diagnostic qualification](./CPU-PROBE-QUALIFICATION.json). This does not
qualify the ARMv7 image or Linux 2.6 execution on a 6300.

Use a fresh copy of the supplied CPU diagnostic handoff kit, including its
Python clients. Stop AetherPD and leave the radio unkeyed before installing.
Read the completed report and uninstall the diagnostic immediately afterwards;
return its JSON with the radio model, firmware and package digest. The probe
requires the expected ordinary UID/GID and zero effective/permitted/inheritable
capabilities, refuses an inherited real-time scheduler, and lowers only itself
to nice 19. It requests no scheduling privilege. Each child has a 45-second
watchdog budget, 30/31-second CPU limits, a 256 MiB address-space limit and no
core/regular-file data writes. The read-only TCP48999 report server exits after
five minutes, with a separate ten-minute overall alarm. Individual output is
capped and marks truncation. These budgets are not hard scheduling guarantees
under a frozen kernel.

The report's elapsed time, CPU time and Linux-reported peak RSS describe these
finite simulations. They do not measure sustained correction throughput,
real-time priority donation, local IQ latency, capture access, RF or installation
privileges. Keep the separate kernel/filesystem probe results: the CPU suite
does not determine `/run` placement or the FPGA network profile. A remaining
unresolved native 6000 bootstrap entry must still be provided and qualified;
the available descriptor/Podman transport is not evidence of that entry.

The restored CPU-only regressions can also be built with
`tools/pd-waveform/build.py --output build/pd-tests --arch armv7 --regression-tests`.
Run the five `*-test-armv7` binaries separately as the ordinary waveform user.
They inject samples and datagram I/O and do not connect to the radio API or
transmit. They check incomplete calibration coverage, phase settling, warm
models, EQ worker results and dispatch/freshness fences. All five passed as
ARMv7 binaries on the 8600; repeat them on the actual 6300. That run does not
qualify real-time scheduling, feedback routing or RF. The builder also produces
a standalone development waveform ZIP; do not install it as part of these
CPU-only tests or treat it as the managed shipping package.

## RF qualification requires the hardware operator

Do not copy the 8600 RXA wiring recipe to the floor model. FlexRadio's
[SmartSDR guide](https://edge.flexradio.com/www/offload/20260423063240/SmartSDR-Software-User-Guide.pdf)
describes the 6300's FDX receive-only path through XVTR; RXA is an alternative
on the 6500. Qualify the actual feedback input, one-SCU antenna switching and
normal receive recovery on the target model before any RF trial.

Confirm dummy load, sampler attenuation, receiver input rating, preamp state,
antenna and independent load measurement on the teammate's own bench. The
8600 operator's physical confirmation and 15 W authorization do not authorize
transmissions on another radio. Use bounded keys, a separate unkey guardian,
firmware timeout and fresh directional power checks. Positive keying belongs
to an explicit operator action through the client TX gate.

Capture matched OFF/ON tones and speech, repeated keys in one session, normal
RX/UI load, and deliberate bounded faults. Record RF power setting and actual
output, input clipping, correction state, IMD referenced per tone, speech band
definitions, settling, packet age, deadline misses, CPU and memory. Keep failed
cases. Native stock APD is unavailable on the 6000; never infer its presence
from the 8600 comparison.

## Release gates

The 6000 cannot be marked supported until its actual kernel/network profile,
narrow installation mechanism, lifecycle failure behavior and RF performance
pass on hardware. Independent review, signed commits, required project CI and
the maintainer's release decision remain separate gates. A reproducible
development ZIP and finite 8600 bench results are not a production release.
