# Testing the native tailnet shim on a FLEX-6000

AetherSDR's tailnet shim makes a FlexRadio a node on your own Tailscale
network (tailnet), so AetherSDR can reach it from anywhere, including from
behind CGNAT or Starlink. On FLEX-8000 and Aurora radios it runs in their
Docker container feature. This is an **experimental** build for FLEX-6000
radios, installed as a legacy waveform named **AetherTailnet**.

AetherRadioProbe (`../cmd/radio-probe`) showed a FLEX-6300 can host it:

- the radio runs our Go program on its 2.6.37 kernel;
- the waveform's own folder keeps files across a reboot;
- the radio relaunches the waveform at boot;
- the waveform can reach Tailscale.

This test answers what the probe couldn't. Does the real shim join a
tailnet? What does it cost the radio's single CPU core? Does a remote
AetherSDR session work through it? Does it come back by itself after a
reboot?

Allow about an hour: 10 minutes idle, 10 minutes of remote receive, and one
reboot.

## How it differs from the container shim

Same code, same version (`0.4.1-native`), started by a launcher (`shim.sh`)
that:

- keeps the node's identity and settings in `state/` beside the waveform,
  falling back to `/var/tmp/aether-tailnet` if that isn't writable;
- keeps Tailscale's logs and caches in RAM (`/tmp/aether-tailnet`), so they
  don't wear or fill the radio's small flash partition;
- reaches the radio's API at `127.0.0.1`;
- turns off Tailscale's log uploads (`TS_NO_LOGS_NO_SUPPORT`) to save CPU;
- runs the shim at `nice 10`, so SmartSDR wins whenever the one core is busy;
- refuses to start on anything but an ARMv7 radio.

## Safety

- **Receive only.** Don't transmit through the shim during this test. The
  shim relays whatever the remote client sends, transmit included. Keeping
  the test to receive keeps an untested build away from your transmitter.
- The shim joins the tailnet you choose, with an auth key you create. Only
  devices on that tailnet, and only those on its allowlist if you set one,
  can reach the radio through it.
- Uninstall the other tailnet waveforms first (for example FlexiTailnet).
  Two shims can't share the radio's ports.
- Signing out (step 7) removes the radio from your tailnet. Uninstalling
  removes every file.

## You need

- A FLEX-6000 on SmartSDR 4.x. The probe ran on 4.2.18.
- A Tailscale account. Create an **auth key** in the admin console
  (Settings → Keys): one-off, *not* ephemeral (an ephemeral node can vanish
  across the reboot). Save it in a file such as `ts-key.txt`, readable only
  by you. Never paste it into a chat.
- A computer on the radio's LAN with Python 3, ideally with Tailscale
  installed and on the same tailnet, so it can read the shim's CPU figures.
- A second device for the remote test, running AetherSDR and Tailscale. It's
  best if it's *off* your LAN, for example a laptop on a phone hotspot.
- The package `AetherTailnet.ssdr_waveform`, from the AetherSDR team or built
  yourself (see "Building it").

## The test

Run the commands from this folder. `tool.py` talks to the radio over the LAN.

1. **Note** the radio model, SmartSDR version, and AetherSDR version on the
   remote device. For the first run, also start the log receiver,
   `python3 -I logsink.py`, and use a package built with
   `--log-url http://<this computer's LAN IP>:8765/`. The radio gives
   waveforms no other way to report errors.
2. **Install:** `python3 -I tool.py --host <radio-ip> install AetherTailnet.ssdr_waveform`.
   Within a minute or two, `python3 -I tool.py --host <radio-ip> status`
   should show `"version": "0.4.1-native"` and `"state": "unprovisioned"`.
   Note how long that took and whether a reboot was needed.
3. **Join the tailnet:**
   `python3 -I tool.py --host <radio-ip> provision --key-file ts-key.txt --hostname flex-6300`.
   Add `--allow you@example.com` to limit who may connect. The shim's admin
   token is saved to `admin-token.txt`, so keep that file private. `status`
   should now show `"state": "running"` and a `tailnet_ip` (100.x.y.z). The
   radio appears in your Tailscale admin console.
4. **Idle for 10 minutes**, with no AetherSDR connected:
   `python3 -I tool.py --host <radio-ip> --tailnet-ip <100.x.y.z> watch --seconds 600 --out idle.jsonl`.
   `shim_cpu_pct` is the shim's share of the radio's one core. If you have
   AetherRadioProbe, install it during this window and save its report as
   `probe-idle.json`: its `load.top_by_cpu` shows SmartSDR's and the shim's
   CPU side by side. Uninstall it afterwards.
5. **Remote receive for 10 minutes.** On the remote device, connect
   AetherSDR to the radio's tailnet IP (Connect → Manual). Receive only:
   tune around, keep a panadapter open, listen. Meanwhile run
   `watch --seconds 600 --out session.jsonl` again on the LAN computer, with
   `--tailnet-ip` as before. Note the audio quality, dropouts, how the
   panadapter and waterfall keep up, and AetherSDR's Network Diagnostics
   (path, round-trip time, loss). A screenshot helps.
6. **Reboot the radio** and don't re-provision. Time how long until `status`
   shows `"state": "running"` again with the **same** `tailnet_ip`, and
   whether the remote device can reconnect. This proves the identity
   survived the reboot.
7. **Clean up:**
   - `python3 -I tool.py --host <radio-ip> signout`, which removes the radio
     from your tailnet;
   - `python3 -I tool.py --host <radio-ip> uninstall`;
   - delete `admin-token.txt`;
   - revoke the auth key if it's still valid.
8. **Send** `notes.md`, `idle.jsonl`, `session.jsonl`, the `status` outputs,
   `probe-idle.json` if you made one, and `shim-log.txt`. These contain
   tailnet and LAN addresses but no keys or tokens; remove the addresses if
   you like.

## The data we're looking for

| # | Question | Where | Why |
|---|---|---|---|
| 1 | Does the shim start? | `status` → `version` | The real shim is bigger than the probe and does more at start-up |
| 2 | Does it join the tailnet? | `status` → `state`, `last_error`; `shim-log.txt` | The first use of Tailscale's networking code on a 2.6.37 kernel. Watch for "function not implemented" / ENOSYS |
| 3 | What does it cost when idle? | `idle.jsonl` → `shim_cpu_pct`, `shim_rss_kb`; `probe-idle.json` | One core, already about 46% busy with SmartSDR. The container shim idles at 0–3% of a much faster core |
| 4 | Does a remote session work? | your notes; `session.jsonl` → `session_detail` (packets, gaps, breaks), `shim_cpu_pct`, `path`, `rtt_ms` | The point of the shim |
| 5 | Does it survive a reboot? | step 6 timing; same `tailnet_ip` | No re-provisioning after power cuts |
| 6 | Direct or relayed? | `path` (`direct`, `peer-relay`, `relay`) | A relayed path costs latency |

## Prompt for your AI assistant

Fill in the angle brackets and start your assistant in this folder.

```text
Help me test the native AetherSDR tailnet shim (the "AetherTailnet" legacy
waveform) on my FlexRadio, and collect the results for the AetherSDR team.
Read native/TESTING.md and native/tool.py first (and ../cmd/radio-probe/
TESTING.md if I use the probe); they describe the steps and the data needed.

My radio: <model> on SmartSDR <version>, LAN IP <radio-ip>.
This computer's LAN IP: <pc-ip>. Tailscale on this computer: <yes/no>.
My Tailscale auth key is in the file <ts-key.txt>.
Remote device for step 5: <description>.

If this folder has no AetherTailnet.ssdr_waveform, build one as TESTING.md's
"Building it" says, with --log-url http://<pc-ip>:8765/ for this first run,
and start logsink.py before installing. Ask me before installing Go.

Follow the steps in TESTING.md in order. Ask me before each step that
changes the radio (install, provision, reboot, sign-out, uninstall), and tell
me when I need to do something myself (the remote session in step 5).
Keep notes.md: what you did, when, how long each step took, every error
message, and anything that differs from TESTING.md.

Rules you must follow:
- Never print, log or copy the auth key or admin-token.txt anywhere, and
  never put them on a command line; use the files.
- Never send a radio command other than the upload, `waveform restart` and
  `waveform uninstall` that tool.py sends for AetherTailnet (or the probe's
  equivalents). Nothing that transmits, tunes or changes radio settings.
  This test is receive only.
- Don't try to gain root or other privileges on the radio, don't get around
  its permissions, and don't decompile or read FlexRadio's software.
- Don't send my files anywhere; I'll send the results.

If something fails, use TESTING.md's troubleshooting section to find out
why. You may change the launcher or rebuild with build.py's options, and you
may change the shim's code if a fix is clear (it's GPL v3). Record every
change and its result in notes.md. The reason something fails is as useful
to the team as a success. Stop and ask me after a few unsuccessful attempts,
or before anything that would break the rules above.

At the end, answer the six questions in TESTING.md's "The data we're looking
for" table with numbers, and list the files I should send.
```

## Troubleshooting (for you or your AI)

### The upload fails, or the waveform is never listed

- The full package is about 8.4 MB, and the probe that ran on a FLEX-6300
  was 5.5 MB. If the radio refuses or drops the upload, rebuild with
  `--small`, which is about 0.8 MB smaller because it leaves out Tailscale
  features the shim doesn't use.
- Unpacked it needs about 22 MB. On the 6300 the `/user` partition that
  holds waveforms had 41 MB free, and other waveforms use that space too.
- As with the probe: try SmartSDR's Waveforms window, and reboot once if it
  isn't listed after 10 minutes.

### Listed, but `status` can't connect (port 48992)

1. Wait a minute and try again. Then `tool.py restart`.
2. With a `--log-url` build and `logsink.py` running, the shim posts its log
   every 15 s, so silence there means it never started.
3. The launcher exits immediately on anything but ARMv7 (`uname -m`). If
   another program already holds port 48992, such as another tailnet
   waveform, the shim logs `api: … address already in use; retrying` and
   keeps trying.
4. If the probe ran on this radio but the shim doesn't, the difference is
   the shim itself: start-up system calls or memory. Find where it stops
   using the log, or a launcher that writes `date` and the exit code to
   `/tmp` around the binary, then read it back with a probe-style report.
   Report what you find; don't work around the radio's permissions.

### Provision fails, or `state` is `error`

- Read `last_error` and `shim-log.txt`. Typical causes:
  - the key is invalid, used up or expired;
  - the radio's clock is wrong (TLS certificate errors right after boot);
  - the kernel lacks a feature Tailscale needs ("function not implemented",
    "ENOSYS", "invalid argument" on a socket call).

  Report the last one with the log lines around it: it's the risk this test
  exists to find.
- Provisioning again needs the same `admin-token.txt` (tool.py sends it).

### It joins, but the remote session is poor

- Note `path`. `relay` means traffic goes through a Tailscale DERP server.
  Direct paths need UDP to pass your router's NAT.
- Watch `shim_cpu_pct` during the session. If the core is saturated, that's
  the answer. A `--small` build also leaves out Tailscale's logging engine,
  so try it to see whether CPU drops.
- `session_detail` counts the radio's packets before the tunnel: `gaps`
  there means the shim fell behind on the radio side.

### After a reboot it asks to be provisioned again

The identity didn't survive. Report `status` before and after, and the
probe's storage section from this radio if you have it. The launcher keeps
state in `state/` beside the waveform, or `/var/tmp/aether-tailnet` when
that isn't writable.

## Building it

```sh
git clone https://github.com/aethersdr/AetherSDR.git
cd AetherSDR
git checkout feat/tailnet-shim-native
cd tools/flex-tailnet-shim/native
python3 -I build.py --output ~/aether-tailnet [--log-url http://<pc-ip>:8765/] [--small]
```

It needs Go 1.27.1 (a newer Go fetches it by itself) and Python 3. All
dependencies are vendored. Without `--log-url` the package is reproducible
byte for byte.
