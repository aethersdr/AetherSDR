# Testing AetherRadioProbe on your radio

Thank you for helping. AetherSDR's tailnet shim gives remote access to a
FlexRadio through Tailscale, from behind CGNAT, Starlink and similar. Today it
runs only on FLEX-8000 and Aurora radios, inside their Docker container
feature. To run it on a FLEX-6000 it would have to be installed as a legacy
(native) waveform, and nobody on the AetherSDR team has a 6000 to test that.

AetherRadioProbe is a small diagnostic installed the same way. It doesn't
connect to Tailscale. It measures whether the shim *could* run on your radio,
then reports what it found. A full test takes about 15 minutes and one radio
reboot.

## What the probe does and doesn't do

- Reads system information (`/proc`, its own permissions and limits) and
  measures CPU headroom for 3 seconds and encryption speed for 1 second.
- Writes one small file named `.aether-radio-probe-marker` in each directory
  it can write, so the run after a reboot can tell which ones kept it. It
  never writes into the radio's waveform store or its parent directories.
- Checks that it can reach Tailscale's servers (DNS, HTTPS, STUN). It does
  not join a tailnet.
- Connects to the radio's API as a plain, non-GUI client and sends exactly
  two commands, `client udpport <port>` and `sub meter all`, so it can see
  where the radio sends its UDP data from. It **never** sends a transmit,
  tune, slice, panadapter or settings command.
- Serves its report as JSON at `http://<radio-ip>:48994/` for 30 minutes
  after each start, then exits.

Nothing is changed permanently. Uninstalling the waveform and running the
cleanup command removes every trace.

## You need

- The package `AetherRadioProbe.ssdr_waveform`, sent to you by the AetherSDR
  team or built yourself (see "Building it yourself" below).
- A computer on the same LAN as the radio, with SmartSDR for Windows or
  Python 3.
- Nobody operating the radio for those 15 minutes, because step 4 reboots it.

## The test

1. **Note your radio.** Model (for example FLEX-6500), SmartSDR version, and
   your client and its version. Don't send us the serial number.
2. **Install.** In SmartSDR for Windows, open Settings → Waveforms, choose
   Install (Legacy if it asks), and pick `AetherRadioProbe.ssdr_waveform`.
   Or, from Python:
   `python3 -I upload.py --host <radio-ip> --install AetherRadioProbe.ssdr_waveform`
   Note how long it takes, and whether AetherRadioProbe appears in the
   waveform list without a reboot. On a FLEX-6300 and a FLEX-6700 it
   uploaded in under a second and was running 12–45 seconds later with no
   reboot. If it isn't listed after 10 minutes, reboot the radio.
3. **First report.** Once it's listed, wait 2 minutes, then
   `python3 -I upload.py --host <radio-ip> --report boot1.json`
   or open `http://<radio-ip>:48994/` in a browser and save the page as
   `boot1.json`. If the page says "checks still running", reload in a minute.
4. **Reboot** the radio from SmartSDR, or power-cycle it. Once it's back,
   wait 2 minutes and save the report again as `boot2.json`. This second
   report answers the two most important questions: which directories
   survive a reboot, and whether the radio starts the waveform on its own.
5. **Clean up:** `python3 -I upload.py --host <radio-ip> --cleanup`, then
   uninstall AetherRadioProbe in SmartSDR's Waveforms window, or
   `python3 -I upload.py --host <radio-ip> --uninstall`.
6. **Send us** `boot1.json`, `boot2.json` and your notes from steps 1–2.

**Privacy:** the report contains your radio's LAN addresses and your public
IP address (under `network.stun[].mapped`). Remove those if you like. It
contains no serial number, hostname or password.

## The data we're looking for

These are the answers, in order of importance, with where each one appears
in the report.

| # | Question | Where in the report | Why it matters |
|---|---|---|---|
| 1 | Does the probe run at all? | Any report at all | The shim uses the same Go 1.27 runtime. No report from an installed, listed waveform most likely means the kernel is too old for it. |
| 2 | Which writable directories survive a reboot? | `summary.writable_dirs`; `summary.dirs_whose_marker_survived_a_reboot` in `boot2.json`; `storage.dirs[]` | The shim must keep its Tailscale identity across reboots. On the 8600 nothing survives (only RAM-backed `/tmp`, `/var/tmp` and `/dev/shm` are writable). |
| 3 | Does the radio start the waveform at boot, or restart it after it exits? | `boot2.json` existing at all; `summary.runs_this_boot`; `system.uptime_s` | The shim has to come back on its own after a power cut. |
| 4 | Can it reach the internet? | `summary.internet_tcp_443`, `internet_tls_verified`, `internet_udp_stun`; `network.dns`, `network.controlplane_tls.error` | Without this, no tailnet. A TLS error mentioning "certificate" right after boot usually means the radio's clock wasn't set yet. |
| 5 | Is there enough CPU and memory? | `summary.cores`, `cpu_model`, `cpu_idle_pct`, `mem_available_mb`, `chacha20poly1305_mb_per_s`; `load.top_by_cpu` | The shim uses about 80–90 MB of RAM. Its encryption cost per byte relayed is set by the ChaCha number. |
| 6 | Where does the radio's UDP come from? | `summary.radio_api_addr`; `summary.vita_source_by_api_addr`; `radio_api[]` | The shim must accept the radio's streams from exactly that source. |
| 7 | Are the shim's ports free? | `network.shim_port_binds`; `network.listening` | It uses TCP 48992/48993. |
| 8 | Who does it run as? | `identity.uid`, `process_status`, `parent_cmdline`, `limits` | This sets what the shim may do. |

To compare against, a FLEX-6300 on SmartSDR 4.2.18 reported:

- **Platform:** `armv7l` Cortex-A8, kernel `2.6.37-0.1-microburst+`, 1 core, 469 MB total and about 430 MB free, ChaCha 9.8 MB/s, CPU 54% idle.
- **Identity:** UID 78666, parent `/usr/bin/smartsdr`.
- **Lifecycle:** running 45 s after upload with no reboot, and restarted itself at boot.
- **Persistent storage:** `/user/waveforms/AetherRadioProbe` (ext2, 41 MB free) and `/var/tmp` (ext3) kept their markers across a power cycle.
- **UDP source:** `127.0.0.1:4992` → `127.0.0.1:4993`, and the same pattern for each address.

A FLEX-6700 on SmartSDR 4.3.16 had the same computer (same kernel, one
Cortex-A8 core, 469 MB) and the same answers, with two differences:

- **CPU:** SmartSDR took about 51% of the core and ChaCha ran at 7.1 MB/s.
- **Space:** its `/user` partition is only 19 MB, and was full with the
  probe's 12.8 MB installed, so about 13 MB was free beforehand.

Go 1.27 documents Linux 3.2 as its minimum, but the probe ran on that 2.6.37
kernel. Probe 0.1.0 reported `mem_available_mb: 0` on it, because kernels
before 3.14 have no `MemAvailable`; 0.1.1 estimates it instead.

The FLEX-8600 on SmartSDR 4.3.16 reported:

- **Platform:** `aarch64`, kernel `5.15.92-v8+`, 4 cores, 1459 MB available, ChaCha 137 MB/s, CPU about 95% idle.
- **Identity:** UID 78666, parent `/usr/bin/smartsdr`.
- **Lifecycle:** started about 10 s after upload with no reboot, restarted itself at boot, and nothing persistent was writable.
- **UDP source:** `127.0.0.1:4992` → `127.0.0.1:4993`, and the same pattern for each address.

## Prompt for your AI assistant

If you use an AI coding assistant that can run commands on your computer
(Claude Code, Codex, Cursor or similar), you can hand it the whole test.
Copy the text below, fill in the two values at the top, and start it in the
folder that holds this file, `upload.py` and the package.

```text
Help me run the AetherRadioProbe test on my FlexRadio and collect the results
for the AetherSDR team. Read TESTING.md and README.md in this folder first;
they describe the probe, the steps, and the data the team needs.

My radio: <model, e.g. FLEX-6500>, SmartSDR <version>, at IP <radio-ip>.

If this folder doesn't already have AetherRadioProbe.ssdr_waveform, get it
the way TESTING.md's "Building it yourself" section says: clone the repo,
check out the feat/flex6k-radio-probe branch, and build it with build.py
(install Go if needed; ask me first).

Do the test in TESTING.md: install the package with upload.py (or tell me if I
should use SmartSDR's Waveforms window instead), watch for the report on
http://<radio-ip>:48994/, save boot1.json, ask me before rebooting the radio,
save boot2.json after the reboot, then clean up and uninstall. Keep a short
log of what you did, how long each step took, and every error message, in
notes.md.

Rules you must follow:
- Never send any radio command except: the upload commands upload.py uses,
  `waveform restart AetherRadioProbe`, `waveform uninstall AetherRadioProbe`,
  and the probe's own `client udpport` / `sub meter all`. Never send anything
  that transmits, tunes, keys, or changes radio settings, slices or
  panadapters. Never touch the radio's firmware.
- Ask me before every reboot or power cycle, and before installing anything
  other than AetherRadioProbe.
- Don't try to gain root or other privileges on the radio, don't try to
  bypass its permissions, and don't decompile or read FlexRadio's firmware
  or software. The goal is to measure what an ordinary waveform can do.
- Don't send my files anywhere. I'll send the results myself.

If the probe doesn't work at first, use the troubleshooting section of
TESTING.md to find out why, change the probe if it helps (it's open source,
GPL v3), rebuild it with build.py, and try again. Record every change and its
result in notes.md, because the reason it failed is as useful to the team as
a working report. Stop and ask me if you get stuck after a few attempts, or
if a fix would break one of the rules above.

When done, summarize the answers to the eight questions in TESTING.md's
"The data we're looking for" table, and list the files I should send.
```

## Troubleshooting (for you or your AI)

Work out which stage failed, then use the matching section.

### The upload fails or hangs

- If the radio refuses a command, `upload.py` stops with the radio's reply
  line (`R<n>|<hex status>|…`, nonzero status). Record it.
- The data goes to the port the radio names in its reply to
  `file upload <bytes> new_waveform`, or to 42607 if it names none (this is
  FlexLib's own logic). A firewall on your computer can block that second
  connection.
- Try SmartSDR's Waveforms window instead: it uses the same API and may
  show a clearer error.

### Uploaded, but never listed (even after a reboot)

- The manifest (`package/AetherRadioProbe.cfg`) asks for
  `Minimum-SmartSDR-Version: 3.0.0.0`. That's a guess: the format has been
  proven only on SmartSDR 4.2.18 and 4.3.16. If you're on SmartSDR 2.x, try
  lowering it, e.g. to `1.3.1.0`, the value FlexRadio's own FreeDV legacy
  waveform used, then rebuild.
- The radio unpacks the ZIP into a directory named after the package. Keep
  the single top-level `AetherRadioProbe/` folder and keep the `.cfg` name
  matching the `Name:` field.
- Check that it doesn't appear under the Docker list instead of Legacy.

### Listed, but nothing answers on port 48994

This is the most informative failure. Work through it in order.

1. Wait longer. The probe serves a placeholder within a second of starting,
   but the radio may start waveforms late after a boot. Try
   `waveform restart AetherRadioProbe` (`upload.py --restart`).
2. Check the port from another LAN computer, in case your own computer's
   firewall is the problem: `curl http://<radio-ip>:48994/`.
3. The launcher (`package/probe.sh`) picks the binary by `uname -m`:
   `aarch64` gets the ARM64 build, any other `arm*` gets the ARMv7 build, and
   anything else exits with an error. If your radio's CPU isn't ARM at all,
   that's the answer: report it.
4. The binary may not run on your kernel. Go 1.27, which the shim needs,
   requires Linux 3.2 or newer and an ARMv7 CPU with VFPv3 for the ARMv7
   build. To tell "Go can't run here" apart from "nothing ran at all", make a
   diagnostic variant:
   - Replace `probe.sh` with a shell-only version that does no Go at all,
     for example one that tries `busybox httpd`, `nc` or `python` (whichever
     exists) to serve `uname -a`, `cat /proc/version`, `cat /proc/cpuinfo`,
     `id` and `ls -la` of its own directory on port 48994. If even that
     fails, the radio may not run waveform executables at all, or may not
     allow listening sockets.
   - Or build the probe with an older Go (1.21 supports kernels back to
     2.6.32). The probe imports Tailscale's STUN code and
     `golang.org/x/crypto`, which need Go 1.27. For an older Go, copy
     `cmd/radio-probe` into its own module and replace `bench()` and
     `stunChecks()` with stubs. Everything else is standard library. A probe
     that runs with Go 1.21 but not 1.27 tells us the kernel is too old for
     the shim.
5. If you make the launcher log, write to `/tmp` (it was writable on the
   8600), not the probe's own directory (it wasn't).

### The report arrives but parts are empty or failed

- `radio_api[].error` "connection refused" at every address means the API
  isn't on 4992 from inside the radio. Record which addresses were tried.
- `vita.packets: 0` with replies `R1|0|` and `R2|0|` means the radio
  accepted the commands but sent no UDP to this client within 6 seconds.
  Worth reporting as is. Don't add commands to make it work.
- Internet checks failing with DNS errors: record `network.resolv_conf` and
  `network.routes`. Check that the radio has a gateway and DNS configured.
- `shim_port_binds` failing for 48992 just means something else already uses
  it (on the 8600 it's the Docker shim). `network.listening` shows what.

### Changing the probe

- **Code:** `main.go` (flow, report, HTTP), `system.go`, `storage.go`,
  `network.go`, `radioapi.go`; tests in `probe_test.go`.
- **Run tests:** `go test ./cmd/radio-probe` from `tools/flex-tailnet-shim`.
  Tests must not write outside their temp directory.
- **Flags:** `-listen`, `-lifetime`, `-no-radio-api`. The radio runs
  `probe.sh` with no arguments, so pass flags by editing the `exec` lines in
  `probe.sh`.
- **Bump `probeVersion` in `main.go`** and `Version:` in the `.cfg` when you
  change anything, so reports say which build produced them.
- **Rebuild:** `python3 -I build.py --output OUT_DIR`. It needs Go 1.27.1 (or
  a newer Go, which fetches 1.27.1 itself via `GOTOOLCHAIN`) and Python 3.
- **Keep it safe:** whatever you change must keep the radio commands to the
  list in the prompt above.

## Building it yourself

```sh
git clone https://github.com/aethersdr/AetherSDR.git
cd AetherSDR
git checkout feat/flex6k-radio-probe
cd tools/flex-tailnet-shim/cmd/radio-probe
python3 -I build.py --output ~/radio-probe-out
```

The build is reproducible, so the same commit gives a byte-identical package
on any machine with the same toolchain. All dependencies are vendored in the
repository; nothing is downloaded except, if needed, the Go 1.27.1
toolchain.
