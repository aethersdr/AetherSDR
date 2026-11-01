---
title: "Waveform Container Environment"
slug: "/waveform-environment"
description: "View and change the environment variables of a Docker waveform container on a FlexRadio running SmartSDR v4.3.16 or later, and recreate the container with them."
status: "Experimental"
applies_to: ["FlexRadio"]
---

:::info[Status]

**Status:** Experimental · **Applies to:** FlexRadio

:::

A Docker waveform container on the radio can take settings as environment variables, such as a server address, a mode or an access key. With SmartSDR v4.3.16 or later the radio keeps a set of variables for each container. AetherSDR shows them and can replace them, then recreate the container from the image already on the radio, without uploading it again.

## Requirements

- A FlexRadio with a Waveform Processor (FLEX-8000 series, Aurora and the other non-6000-series models) running SmartSDR v4.3.16 or later.
- A Docker waveform container installed on the radio (**Tools → Waveforms... → Install...**).

On older firmware, on a 6000-series radio, or with no radio connected, the **Environment…** button is dimmed and its tooltip says why.

## Changing a container's environment

1. Open **Tools → Waveforms...**.
2. On the container's row, click **Environment…**. The window lists the variables the radio reports for that container. Values are hidden; tick **Show values** to see them.
3. Edit a name or value, click **Add Variable** for a new one, or **Remove** to delete one. **Revert** goes back to what the radio reports.
4. Click **Apply & Recreate**. AetherSDR replaces the container's whole environment with the rows shown and asks the radio to rebuild the container. The waveform restarts, and the row in the Waveforms window reads **Recreating…**.
5. When the radio reports the restart, the row reads **Recreated** and the editor shows the variables the radio now reports. Nothing is saved on the computer: what you see always comes from the radio.

The editor can be closed while the radio works. The Waveforms window still shows the result on the container's row.

## Reference

| Item | Rule |
|---|---|
| Variable name | Letters, digits and underscores, starting with a letter or underscore; at most 64 characters. Each name once. |
| Value | Any text on one line, including spaces; at most 512 characters. A value can be empty. |
| Blank rows | Skipped. |
| Apply & Recreate | Clears the radio's staged variables, stages each row, recreates the container, then clears the staged variables again so they cannot reach a later install. |

| Row message | Meaning |
|---|---|
| **Recreated** | The radio rebuilt the container with the new environment. |
| **Recreate failed: the radio reported an error** | The radio could not rebuild the container. |
| **Recreate not confirmed by the radio** | The radio accepted the request but did not report a result within a minute. Check the waveform before relying on it. |
| **Environment not applied** | The radio refused a step, or the connection ended, before the container was recreated. The editor names the step. |

## Known issues

- Values may be passwords or keys. AetherSDR keeps them out of its logs, the support bundle and the automation bridge (which lists variable names only), but the radio itself sends them back over the network in plain text.
- Whether **Restart** alone applies changed variables, and whether two clients staging variables at the same moment can collide, has not been confirmed on a radio. **Apply & Recreate** does not depend on either.

## See also

- [Remote Access (Tailscale)](./tailscale-remote-access.md), which installs a waveform container
- [FlexRadio](./flexradio.md)
