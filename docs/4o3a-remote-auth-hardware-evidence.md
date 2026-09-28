# TGXL and PGXL remote authentication: hardware evidence

Captured on 2026-09-27 for issue #2313 using
[`tools/probe_4o3a_auth.py`](../tools/probe_4o3a_auth.py). The operator supplied
the device addresses and authorization code. The code was entered through a
non-echoing prompt; it is not stored in the probe or shown below. The probe
opened one TCP connection per device, read the greeting, sent one auth command,
read one reply, and closed the connection. It sent no control or transmit
commands.

Command:

```text
python3 tools/probe_4o3a_auth.py --tgxl-host <TGXL_HOST> --pgxl-host <PGXL_HOST>
```

Output (authorization code and private addresses redacted):

```text
TGXL <TGXL_HOST>:9010
  greeting: V1.2.17 AUTH
  sending: C1|auth <code>
  reply: R0|0|auth OK
  auth accepted: yes
PGXL <PGXL_HOST>:9008
  greeting: V3.9.1 AUTH
  sending: C1|auth code=<code>
  reply: R1|0|Authorized
  auth accepted: yes
```

The probe exited with status 0. A separate single-attempt PGXL probe using
`C1|auth <code>` received `R1|50000016|Incorrect parameter`; the successful
PGXL form includes `code=`. TGXL's success response uses sequence `0` despite
the command using sequence `1`, so a TGXL implementation must not require an
`R1` auth response. This observation establishes the handshake on these two
devices at the firmware versions shown; it does not establish behavior for
other firmware versions or Antenna Genius.

The [vendor TGXL API document](https://github.com/user-attachments/files/27611828/TG.XL.API.Commands.pdf)
lists `R1|0|Unauthorized` for a rejected code. Because its numeric result is
also zero, the client must check the reply body. This rejection was documented
by the vendor; it was not produced by the hardware probe above.

The [4O3A Antenna Genius TCP/IP API](https://github.com/4o3a/genius-api-docs/wiki/Antenna-Genius-TCPIP-API)
documents the optional WAN `AUTH` greeting, and its
[auth command](https://github.com/4o3a/genius-api-docs/wiki/Antenna-Genius-TCPIP-auth)
documents `auth code=<code>`. The command format specifies a carriage-return
terminator. The published response format echoes the sequence number and uses
a zero hexadecimal result for success; an optional message may follow it.
This is protocol documentation, not a live AG validation.

## AetherSDR application run

On 2026-09-27, the QtKeychain-enabled `AetherSDR.app` built from this branch
was launched with the operator's existing settings. The app established TCP
connections from the operator's client host to both configured devices. Two `nettop`
snapshots of the app process showed continuing bidirectional traffic:

| Device | Endpoint | First in/out | Second in/out |
|---|---|---:|---:|
| TGXL | `<TGXL_HOST>:9010` | 42,420 / 1,741 bytes | 67,899 / 2,667 bytes |
| PGXL | `<PGXL_HOST>:9008` | 39,674 / 1,546 bytes | 54,985 / 2,146 bytes |

These counts are per TCP connection. Both links remained established while
the counters increased. Polling starts after a successful AUTH reply **or** an
unchallenged version greeting. The traffic is consistent with working direct
connections but does not independently establish which greeting the app saw.
Packet capture was unavailable because macOS denied access to the BPF device;
no packet payload or authorization code was recorded during this app run.

## Restart with accessory credentials removed

Both `AetherSDR` Keychain items (`tgxl_auth_code` and `pgxl_auth_code`) were
deleted and confirmed absent, then the app was restarted. The restarted app
kept its TCP session to the FlexRadio at `<RADIO_HOST>:4992`, but had no
established direct connection to TGXL `:9010` or PGXL `:9008` in the observed
socket snapshot. Its log reported a TGXL direct-socket closure at 13:23:13.
The tuner still appeared active because the FlexRadio relays TGXL status on
its own connection and supports the fallback tuner commands. This observation
does not indicate that the direct TGXL authentication succeeded without a
code.

## Earlier branch build

An earlier branch build was restarted on 2026-09-27. A socket snapshot showed
established TCP connections to the
FlexRadio (`<RADIO_HOST>:4992`), TGXL (`<TGXL_HOST>:9010`), and PGXL
(`<PGXL_HOST>:9008`). This confirms that build connected to both
accessories; the snapshot alone does not prove the contents of the AUTH reply.

## Earlier Tune presses

The pre-change app log from 2026-09-26 shows
`TgxlConnection: TGXL version "1.2.17 AUTH"` at 22:57:45, immediately
followed by an unauthenticated `C1|info`. At 22:57:48–22:57:58 it records repeated
`TunerModel::autoTune: using direct TGXL path` entries followed by
`TgxlConnection: sent "C…|autotune"`. Those presses did not use the FlexRadio
relay. Before this change, `TgxlConnection` marked an `AUTH`-challenged socket
connected as soon as it read the version greeting. `TunerModel::autoTune()`
prefers that direct socket whenever `isConnected()` is true, so the app sent
Tune over an unauthenticated path. The log establishes the route choice and
outbound commands; the device's reason for not tuning was not captured.

The new handshake only marks the direct path connected after authorization.
With no saved code, the direct path stays unavailable and Tune can use the
existing FlexRadio relay. No relay command implementation was changed in this
branch. During an operator-initiated Tune action in the later 2026-09-27 run,
the radio accepted a relayed
`tgxl autotune` command (`R69|0|`) and reported `tuning=1`.

## Later application run

The app was restarted on 2026-09-27.
`lsof` showed established sockets to the FlexRadio, TGXL `:9010`, and PGXL
`:9008`. Two `nettop` samples one second apart showed TGXL traffic increasing
from 27,538 / 918 to 28,658 / 962 bytes (in / out), and PGXL traffic
increasing from 13,668 / 698 to 14,864 / 742 bytes. These are socket
counters, not captured payloads. They establish live bidirectional traffic
from that build; they do not independently show the AUTH reply text. The means
by which direct connections resumed after the Keychain-deletion experiment
was not recorded, so this later traffic is not evidence that a saved code was
restored or that either device challenged this particular app session.

The endpoint-bound Keychain change made after these captures has been checked
with an in-memory Keychain job test. Its full in-app restore and AUTH path has
not yet been observed on hardware.

## Credential scope and remaining live checks

Each accessory type has one saved Keychain record, bound to the IP address and
port of the peer that accepted the code. Connecting the same type at another
address replaces that record after the new peer accepts a code. If a device's
address changes, the operator must enter its code again; the old record is not
sent to the new address. This is the current credential scope, including for a
hostname whose resolved address changes.

A failed Keychain deletion leaves the saved code in the operating system's
vault. The setup dialog reports that Clear code must be retried; it cannot
claim the secret was removed until the vault confirms deletion.

The saved-code three-failure limit is pinned by socket-free tests for TGXL,
PGXL and Antenna Genius, including timeout and socket-close paths. The full
in-app Keychain restore, Antenna Genius firmware handshake, and docked and
floating applet layout still require operator hardware/UI verification before
the PR claims them as live-proven.

The dialog test uses an in-memory credential-store adapter, and the separate
credential-store test uses an in-memory QtKeychain job double. Their saved and
cleared codes never reach an OS vault. The handshake test uses
an isolated settings profile. The automation grab test was mutation-checked:
removing the masking call made it fail, and restoring the call made it pass.
The TGXL and PGXL host-switch assertions were also mutation-checked: removing
their typed-code target guards made both assertions fail, and restoring the
guards made the handshake test pass.
The setup-dialog recovery and rejected-code flags, and the Antenna Genius
target-specific block lookup, were mutation-checked separately: each removed
guard failed its focused test, and restoring the guard made both focused tests
pass.

## Follow-up review checks (2026-09-28)

The focused socket-free dialog and handshake tests now cover a code discarded
before verification by TGXL/PGXL disconnect or host switch, AG target switch,
and the warning shown in the setup dialog. They also cover per-target AG and
ShackSwitch retry-budget resets, AG Clear code leaving ShackSwitch's block
intact, and clearing a TGXL/PGXL blocked-state notice when the operator retries.
`peripheral_auth_dialog_test`, `peripheral_auth_handshake_test`, and
`peripheral_auth_keychain_test` passed locally. These checks inject protocol
frames or use an in-memory credential store; they do not claim a new live
hardware observation.

The next adversarial review identified a Setup-dialog ordering bug when the
operator corrected a device address and entered a replacement code. A new
dialog regression first failed against the original ordering, then passed
after the dialog retired the previous attempt before marking the replacement
code pending. It exercises the TGXL, PGXL, and AG Connect buttons and checks
that a later unchallenged greeting says the replacement code was not saved.
The test injects the production connect-ordering callback and feeds greetings
directly into the connection handlers. It opens no outbound connection and
uses no listener or simulated firmware peer. This is UI-state evidence, not
a live device handshake.


## PR #6008 review follow-up

AG authorization now requires sequence 1, a zero result, and either an omitted,
empty, or exactly `OK` message. Unknown bodies, extra fields, and zero-result
`Unauthorized` / `Denied` replies are rejected before connected state or
credential acceptance is signaled. The `OK` allowlist is a defensive client
policy tested with injected frames, not a claim that AG hardware emitted it.
The vendor API explicitly documents `V<a.b.c> AG[ AUTH]`, so the AG identifier
requirement is retained. A live AG check must capture both the WAN greeting
(in particular whether it retains `AG`) and accepted/rejected reply bodies;
TGXL's tokenless greeting alone is not evidence of AG behavior.

Auth failure counters include both timeouts and mid-auth socket closes. AG
tracks at most 128 failed targets, ignores empty targets, and does not evict
old blocks to admit new targets. At capacity, untracked targets are blocked
until a tracked target's budget is reset or the model is restarted. This
bounds discovery-churn memory without silently granting old targets retries.

A failed Keychain Clear now retains the cached credential as well as the
vault record. Cache removal waits for successful deletion, and an older
queued delete cannot erase a newer session save. The credential tests cover
denied deletes, delete/save ordering, and consecutive queued clears.

The six CodeGuard path-traversal advisories identify a protocol comment and
unchanged MainWindow label/bundle-path code, not new path operations in this
PR. No code change is warranted for those findings. Applet-indicator design
and central settings placement remain subject to the maintainer's decision.

Local review validation rebuilt and passed the socket-free handshake and
Keychain tests. The committed PR dialog sources were compiled separately
against the updated libraries and passed; the workspace's uncommitted
Peripherals redesign is excluded from this review patch. Mutation checks for
the AG body guard, empty-target guard, retry-history cap, and failed-delete
cache handling each failed with the respective fix removed, then passed after
restoration. Static registration, manifest, engine-boundary, capability-record,
command-plane, and whitespace checks found no new blocker.
