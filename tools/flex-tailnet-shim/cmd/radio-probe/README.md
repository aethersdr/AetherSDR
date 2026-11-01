# AetherRadioProbe

A one-off diagnostic, packaged as a legacy (native) SmartSDR waveform, that
answers whether flex-tailnet-shim can run inside a radio we can't bench, such
as a FLEX-6000. It is built from the shim's own module with the shim's pinned
toolchain (Go 1.27.1) and vendored Tailscale code, so if the probe runs, the
shim's runtime runs.

## What it does

On every start it collects a report and serves it for 30 minutes at
`http://<radio-ip>:48994/` (JSON; `summary` at the top). Then it exits.

| Question | How |
|---|---|
| Kernel, CPU, cores, memory | `/proc`, and `uname -m` from the launcher |
| Idle headroom | CPU idle and the top processes over 3 s |
| Crypto speed | ChaCha20-Poly1305 (WireGuard's cipher) on 1420-byte packets for 1 s |
| Who it runs as | UID/GID, capabilities, seccomp, cgroup, limits, parent process |
| Where the shim could keep state | Writes `.aether-radio-probe-marker` in its own install directory and each usual writable place; the radio's waveform store and its parents (`/var/lib/smartsdr`, `…/waveforms`) are only checked with `access(2)`, never written |
| Does that state survive a reboot | Each marker lists the boots that saw it; the run after a reboot reports which survived |
| Does the radio relaunch it | `runs_this_boot` above 1 means the radio restarted it after it exited |
| Internet | DNS, TCP and a verified TLS handshake to the Tailscale control plane, STUN to DERP |
| Shim ports | Binds TCP 48992/48993 and UDP 41641, then lists everything listening |
| Radio API and VITA-49 | Connects to TCP 4992 at loopback and each LAN address, sends `client udpport` and `sub meter all` (what FlexLib sends to get meters), and records where the radio's UDP comes from |

It changes nothing else. It never sends a transmit, slice, panadapter or
settings command. The report leaves out the hostname and cpuinfo's serial,
which identify the radio, and holds no secrets, but it does hold the radio's
LAN addresses and its public IP address (from STUN), so share it privately.

If the radio relaunches waveforms that exit, the probe runs again every 30
minutes (a 1-second CPU burst and two short API connections) until it is
uninstalled.

## Running it on a radio

Testers: follow [TESTING.md](TESTING.md), which also has a prompt for an AI
assistant and the troubleshooting steps. In short:


1. Install `AetherRadioProbe.ssdr_waveform` with SmartSDR's Waveforms window,
   or `python3 -I upload.py --host <radio-ip> --install AetherRadioProbe.ssdr_waveform`.
   The upload can take minutes, and some radios list the waveform only after
   a reboot.
2. Two minutes after it starts, save the report:
   `python3 -I upload.py --host <radio-ip> --report boot1.json`, or open
   `http://<radio-ip>:48994/` and save the page.
3. Reboot the radio, wait for it to come up, and save the report again
   (`boot2.json`). This run tells us which directories kept their marker and
   whether the radio relaunches the waveform at boot.
4. Clean up: `python3 -I upload.py --host <radio-ip> --cleanup` removes the
   markers, then `--uninstall` (or SmartSDR's Uninstall) removes the waveform.

If nothing answers on port 48994, first check that AetherRadioProbe is in
the radio's legacy waveform list. Listed but silent means the probe didn't
start, which is itself the answer: most likely a kernel too old for Go 1.27.
Not listed means the install didn't take; the manifest asks for SmartSDR
3.0 or later, a guess, since only 4.2.18 has been proven to accept the format.

## Building

    python3 -I build.py --output OUT_DIR

The package is reproducible byte for byte: a ZIP with one
`AetherRadioProbe/` directory holding the manifest, the launcher, static
ARM64 and ARMv7 binaries and the license bundle.
