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

## How the radio handles it

Measured on a FLEX-8600 running SmartSDR v4.3.16:

- **Recreate replaces the whole environment** with the staged variables. It does not merge them with the old ones, and it uses up the staged set.
- **Restart does not apply staged variables.** **Restart** in the Waveforms window restarts the container with its current environment; staged variables wait for the next recreate. **Apply & Recreate** is the way to change them.
- **With nothing staged, a recreate keeps the old environment.** There is no way to return a container to having no variables. To empty one, give it an empty value.
- **The radio collapses runs of spaces** in a value into one space.
- **Staging is shared by every client on the radio.** If another program stages variables, then whichever client recreates a container next applies them. AetherSDR clears the staged variables before staging its own, which also discards anything another client had staged but not yet applied, and clears them again afterwards.

## Changing a container's environment

1. Open **Tools → Waveforms...**.
2. On the container's row, click **Environment…**. The window lists the variables the radio reports for that container. Values are hidden; tick **Show values** to see them. Even when shown, values stay hidden from the automation bridge and its screenshots.
3. Edit a name or value, click **Add Variable** for a new one, or **Remove** to delete one. **Revert** goes back to what the radio reports.
4. Click **Apply & Recreate**. AetherSDR replaces the container's whole environment with the rows shown and asks the radio to rebuild the container. The waveform restarts, and the row in the Waveforms window reads **Recreating…**.
5. When the radio reports the new variables, the editor reads **Recreated. These are the variables the radio now reports.** and the row reads **Recreated**. Nothing is saved on the computer: what you see always comes from the radio.

The editor can be closed while the radio works. The Waveforms window still shows the result on the container's row. **Apply & Recreate** is dimmed while the list is empty, because the radio would keep the old variables.

## Reference

| Item | Rule |
|---|---|
| Variable name | Letters, digits and underscores, starting with a letter or underscore; at most 64 characters. Each name once. |
| Value | Text on one line, at most 512 characters. Single spaces between words are fine; a value cannot start or end with a space or contain two spaces in a row, because the radio would change it. A value can be empty. |
| Longer values from the radio | Shown in full and never shortened. Apply is refused with the reason until the value fits, so editing another row cannot change it. |
| Blank rows | Skipped. |
| Apply & Recreate | Clears the radio's staged variables, stages each row, recreates the container, waits for the radio to report the new variables, then clears the staged variables again and waits for the radio to confirm. |

| Message | Meaning |
|---|---|
| **Recreated** | The radio rebuilt the container and reported the new environment. |
| **Recreated, but the radio has not yet reported the new variables** | The radio rebuilt the container, but no report of the new variables arrived within a minute. The rows follow the radio's next report. |
| **Recreate failed: the radio reported an error** | The radio could not rebuild the container. |
| **Recreate not confirmed by the radio** | The radio accepted the request but did not report a result within a minute. Check the waveform before relying on it. |
| **Environment not applied** | The radio refused a step, or the connection ended, before the container was recreated. The editor names the step. |
| **Recreated, but clearing staged variables failed** | The container is rebuilt, but the radio did not confirm that the staged variables are gone. Docker installs stay blocked until **Clear Staged Variables** in the editor succeeds. |

## Known issues

- Values may be passwords or keys. AetherSDR keeps them out of its logs, the support bundle and the automation bridge (which lists variable names only, and masks the editor's fields even after **Show values**), but the radio itself sends them back over the network in plain text.
- Because staging is shared by every client on the radio, two clients changing environments at the same moment can apply each other's variables. Change environments from one client at a time.
- While AetherSDR cannot tell whether the radio still holds variables it staged (a clear that was refused, or a connection that ended mid-change), Docker installs wait, since an install would pick them up. **Clear Staged Variables** in the editor resolves it.

## See also

- [Remote Access (Tailscale)](./tailscale-remote-access.md), which installs a waveform container
- [FlexRadio](./flexradio.md)
