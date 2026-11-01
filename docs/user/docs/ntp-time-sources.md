---
title: "NTP Time Sources"
slug: "/ntp-time-sources"
description: "See which NTP server a FlexRadio sets its clock from, and add, edit, enable, disable or remove the radio's NTP time sources (SmartSDR v4.3.16 or later, FLEX-8000 and Aurora)."
status: "Experimental"
applies_to: ["FlexRadio"]
---

:::info[Status]

**Status:** Experimental · **Applies to:** FlexRadio

:::

A FLEX-8000 or Aurora radio running SmartSDR v4.3.16 or later sets its own clock from a list of NTP servers that it keeps itself. **Radio Setup → Time** shows that list and lets you change it. The radio owns the list: AetherSDR sends each change to the radio, and the table shows what the radio then reports.

## Requirements

- A FLEX-8000 series or Aurora radio running SmartSDR v4.3.16 or later.

On older firmware, on a 6000-series radio, or with no radio connected, the **Time** page is still there but dimmed, and it says why.

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

Status, stratum and reach update live while the page is open.

## Adding, editing and removing

- **Add…** opens a form below the table. The ID can use letters, digits and hyphens (up to 64 characters, not starting or ending with a hyphen). The address can use letters, digits and `. : - _ %` (up to 253 characters). The description can use letters, digits, spaces and `. _ -` (up to 62 characters). Anything else is refused with the reason, and nothing is sent. The radio holds at most 10 sources; at 10, **Add…** is dimmed.
- **Edit…** (or double-click a row) changes the address, type, description or prefer setting. The radio has no command to change a source in place, so AetherSDR removes it and creates it again with the same ID, and disables it again if it was disabled. If the radio refuses the new settings, AetherSDR re-creates the old ones and says so. The row reads **Updating…** while this is happening.
- **Remove** asks first, then removes the source from the radio.

## Known issues

- Removing or disabling the last enabled source leaves the radio with no NTP server, so it cannot set its clock from the network. AetherSDR asks before doing either.
- An edit is a remove followed by a create. For that moment the radio has one source fewer, and if the session ends in between the source may be missing until you add it again. The page shows what the radio reports.
- Only the behavior read from SmartSDR v4.3.16 on a FLEX-8600 has been checked against a radio. The create, remove, enable and disable replies are taken from FlexLib and have not yet been checked on a radio.

## See also

- [Radio Setup](./radio-setup.md)
- [AetherClock and GPS](./aetherclock-and-gps.md)
