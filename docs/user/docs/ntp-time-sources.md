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

A radio running SmartSDR v4.3.16 or later sets its own clock from a list of NTP servers that it keeps itself. **Radio Setup → Time** shows that list and lets you change it. The radio owns the list: AetherSDR sends each change to the radio, and the dashboard shows what the radio then reports.

## Requirements

- A FlexRadio running SmartSDR v4.3.16 or later. That includes the FLEX-6000 series as well as the FLEX-8000 series, Aurora, ML-9600 (or MLS-9601) and CL-9300 (or CLS-9301).

On older firmware, on a radio model AetherSDR doesn't know, or with no radio connected, the **Time** page is still there but dimmed, and it says why.

On a FLEX-6300 running SmartSDR 4.3.16, the radio reports its sources and accepts enabling one, but it refuses adding a source and removing a missing one ([#7154](https://github.com/aethersdr/AetherSDR/issues/7154)). Disabling or removing a real source there has not been tested. When the radio refuses a change, the page shows the radio's error, and the list stays as the radio reports it.

## Using the dashboard

**Radio Setup → Time** has a compact live status dashboard above ten numbered server tabs. The page scrolls when the window is too short to show all settings.

- **Network time** identifies the radio's selected network reference, or says that it is awaiting a reference. Selection is the radio's report, not an independent clock-accuracy measurement.
- **Source health** counts enabled sources and sources with at least one reply in the last eight polls. A pool is one configured source, even when it resolves to several servers.
- **Onboard reference** shows the radio's GPSDO status. A locked GPSDO receives gold chrome. GPSDO lock does not confirm the local NTP service is stratum 1; the radio does not currently report that service stratum.

Select a numbered tab to see that source's selection state, upstream **Source stratum**, and **Reach** in chrony's octal notation, alongside the number of answered polls. The eight poll indicators run from oldest on the left to newest on the right: a tick means a reply, a cross means no reply, and a dash means no measurement is reported. These are reach-register bits, not a time-scaled graph.

The source in use has a star on its tab. Other selection states include **Candidate**, **Outlier**, **Jittery**, **Falseticker (rejected)**, **Not usable**, **Unreachable** and **Disabled**. Source stratum describes the upstream source, not the radio's own service stratum.

## Configuring a server

1. Select a numbered tab. Existing sources open their settings directly; an empty tab opens a new-source form.
2. Set **ID**, **Address**, **Type**, **Description** and **Prefer this source**. An existing source's ID is read-only.
3. Choose **Add Source** for an empty slot or **Save Changes** for an existing source. **Reset** reloads the radio's settings for that slot.

The ID can use letters, digits and hyphens (up to 64 characters, not starting or ending with a hyphen). The address can use letters, digits and `. : - _ %` (up to 253 characters). The description can use letters, digits, spaces and `. _ -` (up to 62 characters). Invalid values show a reason and cannot be sent. The radio holds at most ten sources; slot numbers are navigation labels, not radio IDs, and are kept only while this page is open.

**Enabled on radio** requests enabling or disabling the selected source. The checkbox follows the radio's reported state. **Remove** asks before removing the selected source. Removing or disabling the last enabled source requires an additional warning. Pending disables and removals count immediately; an added or enabled source counts only once the radio reports it as enabled.

**Save Changes** stays dimmed until something changes. Editing removes and re-creates the source with its original ID, preserving its enabled state. During the edit the source reads **Updating…** and cannot be changed again. If the radio refuses the new settings, AetherSDR attempts to restore the old definition and reports the result. If the session ends or the radio stops answering, AetherSDR stops the operation and shows the radio's reported state.

Status refreshes preserve text being edited in the current tab. Switching tabs preserves unsaved drafts for each server while the page and radio session remain open. **Reset** discards the current tab's draft. Updates from another client keep existing source tab assignments stable. Any change to the source list restarts the radio's time service, so statistics may briefly be unreported.

## Known issues

On a FLEX-6300 running SmartSDR 4.3.16, source creation is refused by the radio ([#7154](https://github.com/aethersdr/AetherSDR/issues/7154)). The dashboard displays the refusal and continues to follow the radio's source list.

## See also

- [Radio Setup](./radio-setup.md)
- [AetherClock and GPS](./aetherclock-and-gps.md)
