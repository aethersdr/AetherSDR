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

The complete ARMv7 development runtime from signed source `7080de965` is
9,398,712 bytes; its expanded executables/licenses occupy 22,771,134 bytes in RAM. Both ARMv7 and
ARM64 runtime releases reproduced byte for byte with pinned compilers on the
same host. This does not qualify the first-install adapter or radio operation.
See [current runtime qualification](./MANAGED-OWNER-RUNTIME-QUALIFICATION.json).
Earlier [restart qualification](./RESTART-RUNTIME-QUALIFICATION.md) remains
bound to its original source. Only the Go shim payload member changed from
the currently installed `938fc001…` runtime; DSP and fixed helpers are byte-identical.

The calculated ARMv7 successor-upgrade budget is up to 39,682,048 bytes,
including the existing compacted installation, stated state/metadata allowances,
transient files and the native 8 MiB reserve. Against a conservative decimal
interpretation of the reported 41 MB free, only 1,317,952 bytes remain.
One extra full release archive does not fit. The final 6000 adapter must account
for firmware-retained upload/extraction copies as well as signed input staging;
that adapter and first-install high-water measurement remain unqualified.
Other 6000 models may have much smaller partitions.
Keep expanded executables, logs, packet captures and caches in RAM. New native writes cap each configuration/identity file at 64 KiB, including
encoding overhead, and reuse fixed temporary siblings. That bounds these new
file data writes to 256 KiB including temporaries; it does not enforce the full
state/metadata allowance or remove legacy leftovers. Persistent identity and
atomic update/rollback still need an actually qualified 6000 installer.

The 8600 first-install carrier/acceptor still requires `no_new_privs` in
`boot/pd_carrier.c`, `boot/pd_accept.c` and `boot/pd_first_request.c`.
[Linux introduced that flag in 3.5](https://www.kernel.org/doc/html/latest/userspace-api/no_new_privs.html),
so those entry points must not be treated as the Linux 2.6 legacy adapter.
The existing capture worker's strict-seccomp design below is a separate mechanism;
do not replace it with an assumption that SO_LOCK_FILTER is required.

## Receive-only qualification first

1. Record firmware, kernel, CPU features, available memory, mount options,
   free space, waveform UID/GID and capability masks. On Linux before 3.14,
   absent `MemAvailable` is not zero available RAM; retain `MemFree`, `Cached`
   and `Buffers` separately. Never include credentials, environment dumps,
   Tailscale state or private keys in a report.
2. Establish an authorized provisioning mechanism. The ordinary waveform
   UID78666 and autostart/persistence evidence do not establish setup privileges.
   No privilege bypass, generic root command bridge or SUID installation is
   part of this handoff.
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

The restored CPU-only regressions can be built with
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
