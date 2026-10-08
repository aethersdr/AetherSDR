---
title: "Automation Bridge and MCP"
slug: "/automation-bridge-and-mcp"
description: "The automation bridge is an opt-in, local command channel into a running AetherSDR."
status: "Off by default"
applies_to: ["All radios"]
---

:::info[Status]

**Status:** Off by default · **Applies to:** All radios

:::

The automation bridge is an opt-in, local command channel into a running AetherSDR. A program on the same computer can read the app's state (radio, slices, panadapters, the widget tree), capture any window or the panadapter as a PNG, and, if you allow it, operate controls. The AetherSDR source tree also includes an **MCP server** that exposes the bridge to AI assistants such as Claude Code, Cursor, Copilot, Codex CLI and Gemini CLI.

It is mainly a developer and testing tool, but it is also a supported way for your own control-surface or automation software to drive AetherSDR. The full reference, including every verb, is [docs/automation-bridge.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/automation-bridge.md).

## Requirements

- A client on the same computer. The bridge listens on a local socket (a Unix domain socket on macOS and Linux, a named pipe on Windows). There is **no TCP port** and no network exposure.
- For the MCP server: Python 3 (no other dependencies).

Each request is one line (a bare command or a JSON object) and each reply is one line of JSON. On startup the app writes the socket path to `aethersdr-automation.json` in the system temp folder so clients can find it.

## Setup

Open **Settings → Radio Setup... → Network** and find the **Advanced** group:

<img src="/img/screens/radio-setup-network-advanced.png" width="643" alt="Advanced group of the Network page. Enforce Private IP Connections and Agent Automation (MCP) both read Enabled. The Access Token field is blacked out, beside Copy and Rotate buttons. Below are the Allow TX via MCP checkbox (Enable transmit control), the Observe only checkbox (Read-only, block all driving), Network MTU at 1450 bytes and a VITA-49 RX buffer slider at 4 MB, granted 8 MB." />

*The Advanced group on the Network page, with the agent automation (MCP) switch. The access token is blacked out.*

| Control | What it does |
|---------|--------------|
| **Agent Automation (MCP):** Enabled / Disabled | Starts or stops the bridge. Off by default. Enabling it with no token creates one. The setting is saved, so the bridge comes back at the next launch. |
| **Access Token:** with **Copy** and **Rotate** | The shared secret clients must present. **Rotate** creates a new token and applies it immediately, locking out any client still using the old one. |
| **Allow TX via MCP:** "Enable transmit control" | Lets bridge clients key the transmitter. Off by default; turning it on asks for a one-time confirmation. |
| **Observe only:** "Read-only (block all driving)" | Blocks every action that changes anything; reads still work. |

You can also start the bridge for a single run by launching with `AETHER_AUTOMATION=1`. In that case the Enabled/Disabled button is greyed out, because the environment variable wins.

### Access token

Every verb except `ping` is refused without the matching token. The token is kept in your operating system's secret store (macOS Keychain, Windows Credential Manager, or libsecret/KWallet on Linux), never in the settings database.

To give it to an MCP client, set `AETHER_MCP_TOKEN` **only in the environment of the shell that launches your assistant**, using a method that does not record it in shell history. The MCP server inherits it from there.

> **Keep the token out of shared configs.** Do not put the literal token in a shell profile, in `.mcp.json`, or in any other MCP config file: that puts a live credential on disk and risks it landing in a commit or a backup. The token field's tooltip in Radio Setup gives the same advice and links this page, as do the tooltips on the other bridge controls.

Headless or CI runs can pass `AETHER_MCP_TOKEN` to AetherSDR directly; it overrides the keychain.

The token decides which client you have deliberately let in. It is not a wall against other programs running as your own user once your keychain is unlocked.

### MCP server for AI assistants

`tools/aether_mcp.py` in the AetherSDR repository (plain Python 3, no dependencies) wraps the bridge as a Model Context Protocol server.

- **Claude Code** picks it up from the repository's `.mcp.json`; approve it on first use.
- **Other assistants:** add it to your MCP config:
  ```json
  {"mcpServers": {"aethersdr-automation": {
     "command": "python3", "args": ["tools/aether_mcp.py"]}}}
  ```
  On Windows use `python` (or `py -3`).

It offers typed tools (`bridge_status`, `dump_tree`, `grab_widget`, `get_state`, `get_log`, `invoke`, `tune`, `slice`, `pan`, `record`, `menu`, `connect`, `capture_audio`, `wait_for`, `assert_state` and more), plus a raw `bridge_command` for everything else, a `validate_ui_change` prompt, and read-only state resources. The transmit verbs are deliberately reachable only through `bridge_command`.

## Using transmit safety

- The bridge refuses to operate any control that keys the transmitter (MOX/PTT, TUNE, ATU, CWX send, packet/APRS send) through its generic "click this control" path, whoever is asking.
- The dedicated transmit verbs (`key`, `cwx`, `txtest`, `atu`, `transmit`, …) work only when you have allowed TX: tick **Allow TX via MCP**, or launch with `AETHER_AUTOMATION_ALLOW_TX=1`.
- A force-unkey watchdog drops a bridge-started transmission that runs too long (`AETHER_AUTOMATION_TX_MAX_MS`, default 20 s). It does not affect your own, DAX or TCI transmissions.
- `AETHER_AUTOMATION_NO_TX=1` pins TX off for that process even if you allowed it before.

You remain responsible for every emission the bridge makes on your behalf. See [Before You Transmit](./before-you-transmit.md).

## Using observe-only mode

Tick **Observe only** to hand a client visibility without control. The bridge then answers only pure reads (`ping`, `get`, the widget tree, screenshots, logs and similar) and refuses everything else. It is enforced inside AetherSDR, so a client cannot switch it off, and it takes effect immediately on a running bridge. `ping` and `whoami` report it as `readOnly`. Launching with `AETHER_AUTOMATION_READONLY=1` pins it on.

## Checking which build you are talking to

`ping` needs no token and returns the app version plus the **build identity**: the `git describe` string, commit SHA, the release tag it is based on, commits since that tag, and whether the build had local changes. It is the same identity **Help → About** shows, so a test build can be told apart from a release with the same version number.

## Reference

### Environment variables

| Variable | Effect |
|----------|--------|
| `AETHER_AUTOMATION=1` | Starts the bridge for this run; greys out the Enabled/Disabled button |
| `AETHER_MCP_TOKEN` | The access token for an MCP client; passed to AetherSDR itself, it overrides the keychain |
| `AETHER_AUTOMATION_ALLOW_TX=1` | Allows the dedicated transmit verbs |
| `AETHER_AUTOMATION_NO_TX=1` | Pins TX off for the process |
| `AETHER_AUTOMATION_TX_MAX_MS` | Force-unkey watchdog limit for bridge-started transmissions (default 20 s) |
| `AETHER_AUTOMATION_READONLY=1` | Pins observe-only mode on |
| `AETHER_AUTOMATION_SOCKET=<name>` | Overrides the socket name at launch |

## Known issues

- The VOX button refuses every bridge action, including switching VOX off, while the `vox_toggle` shortcut can still switch it on ([#6053](https://github.com/aethersdr/AetherSDR/issues/6053)).
- `key ptt off` and `key mox off` report success without checking that the transmitter actually unkeyed ([#5252](https://github.com/aethersdr/AetherSDR/issues/5252)).

## Troubleshooting

### Every verb except `ping` is refused

The client is not presenting the matching access token.

1. Copy the token with **Copy** in **Settings → Radio Setup... → Network → Advanced**.
2. Set `AETHER_MCP_TOKEN` in the environment of the shell that launches your assistant, then restart the assistant.
3. If the token was rotated, repeat with the new one.

### Transmit verbs are refused

TX is not allowed for bridge clients.

1. Tick **Allow TX via MCP** and confirm, or launch with `AETHER_AUTOMATION_ALLOW_TX=1`.
2. Check that the process was not launched with `AETHER_AUTOMATION_NO_TX=1`, which pins TX off.

### Only reads work

Observe-only mode is on.

1. Untick **Observe only** in **Settings → Radio Setup... → Network → Advanced**.
2. If it stays on, the process was launched with `AETHER_AUTOMATION_READONLY=1`; relaunch without it.

### The Enabled/Disabled button is greyed out

AetherSDR was launched with `AETHER_AUTOMATION=1`, and the environment variable wins.

1. Relaunch without `AETHER_AUTOMATION` to control the bridge from Radio Setup.

### The client reaches a different AetherSDR instance

Each launch rewrites `aethersdr-automation.json` in the system temp folder, so with two instances running the discovery file points at the one started last.

1. Launch each instance with its own `AETHER_AUTOMATION_SOCKET=<name>`.
2. Point the client at that socket explicitly (for example `tools/automation_probe.py --socket <path>`).

## See also

- [AI-Assisted Development](./ai-assisted-development.md): how contributors use the bridge to prove a change
- [TCI Server](./tci-server.md): the network protocol for logging, digital-mode and control-surface software
- [USB Control Surfaces](./usb-control-surfaces.md)
- [StreamDeck](./streamdeck.md)
- [Before You Transmit](./before-you-transmit.md)
- [docs/automation-bridge.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/automation-bridge.md)
