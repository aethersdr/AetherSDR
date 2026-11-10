---
title: "NTP Time Sources"
slug: "/ntp-time-sources"
description: "See which NTP server a FlexRadio sets its clock from, and add, edit, enable, disable or remove the radio's NTP time sources (SmartSDR v4.3.16 or later)."
status: "Experimental"
applies_to: ["FlexRadio"]
---

:::info[Status]

**Status:** Experimental · **Applies to:** FlexRadio

:::

A radio running SmartSDR v4.3.16 or later sets its own clock from a list of NTP servers that it keeps itself. **Radio Setup → Time** shows that list and lets you change it. The radio owns the list: AetherSDR sends each change to the radio, and the table shows what the radio then reports.

## Requirements

- A FlexRadio running SmartSDR v4.3.16 or later. That includes the FLEX-6000 series as well as the FLEX-8000 series, Aurora, ML-9600 (or MLS-9601) and CL-9300 (or CLS-9301).

On older firmware, or with no radio connected, the **Time** page is still there but dimmed, and it says why.

A FLEX-6000 has been seen reporting its sources, but adding, removing, enabling or disabling one there has not been tested yet. If the radio refuses a change, the page shows the radio's error, and the list stays as the radio reports it.

## The table

| Column | What it shows |
|---|---|
| **ID** | The source's name on the radio. It cannot be changed once the source exists. |
| **Address** | The NTP server's host name or IP address. |
| **Type** | **server** (one server), **pool** (a DNS name that hands out several servers) or **peer**. |
| **Status** | **★ In use** for the source the radio is synced to. Otherwise **Candidate**, **Outlier**, **Jittery**, **Falseticker (rejected)**, **Not usable**, **Unreachable** or **Disabled**. |
| **Stratum** | How many steps the server is from a reference clock, or — while unknown. |
| **Reach** | How many of the last eight polls the server answered, such as **8/8**. |
| **Enabled** | Tick or untick to enable or disable the source on the radio. The box shows the radio's state, so it changes only once the radio reports the change. |
| **Prefer** | Whether the radio prefers this source when several are usable. |
| **Description** | An optional note. |

Status, stratum and reach update live while the page is open. Any change to the list restarts the radio's time service, so for a few seconds afterwards every source reads **—** until the radio has polled it again.

## Adding, editing and removing

- **Add…** opens a form below the table. The ID can use letters, digits and hyphens (up to 64 characters, not starting or ending with a hyphen). The address can use letters, digits and `. : - _ %` (up to 253 characters). The description can use letters, digits, spaces and `. _ -` (up to 62 characters). Anything else is refused with the reason, and nothing is sent. The radio holds at most 10 sources; at 10, **Add…** is dimmed.
- **Edit…** (or double-click a row) changes the address, type, description or prefer setting. **Save Changes** stays dimmed until something has changed. The radio has no command to change a source in place, so AetherSDR removes it and creates it again with the same ID, and disables it again if it was disabled. For that moment the radio has one source fewer. The row stays where it is, reads **Updating…** and keeps its **Enabled** box as it was until the edit ends. If the radio refuses the new settings, AetherSDR re-creates the old ones and says so. If the session ends or the radio stops answering partway, AetherSDR sends nothing more and says so; the table then shows what the radio reports, and a source it no longer lists has to be added again.
- **Remove** asks first, then removes the source from the radio.
- Removing or disabling the last enabled source leaves the radio with no NTP server, so it cannot set its clock from the network. AetherSDR asks before doing either. Pending disables and removals count immediately; an added or enabled source counts only once the radio reports it as enabled, even if its command reply has already arrived.
- While the radio has not answered a change to a source, that source cannot be changed again.

## See also

- [Radio Setup](./radio-setup.md)
- [AetherClock and GPS](./aetherclock-and-gps.md)
