---
title: "Net Scheduler"
slug: "/net-scheduler"
description: "The Net Scheduler keeps a list of the nets you check into and reminds you when one is about to start, with a one-click button that tunes the radio to it."
---

The Net Scheduler keeps a list of the nets you check into and reminds you when
one is about to start, with a one-click button that tunes the radio to it.

The schedule is stored on your computer, not in the radio's memory channels,
so it works with any radio and needs no radio connected to edit.

## Setup

Open it from **Tools → Net Scheduler...**

## Using the Net Scheduler

### Adding a net

Click **Add…** and fill in (a new net starts as Weekly on today's weekday):

<img src="/img/screens/net-scheduler-add-net.png" width="488" alt="Add Net dialog. Name: Tuesday 20 m Net. Repeats Weekly every 1 week(s), On with day buttons M, T, W, T, F, S, S. At 8:00 PM, Etc/UTC. Remind me 10 minutes before. Frequency 14.250000 MHz, USB, with a Capture current VFO button. Filter 100 Hz to 2800 Hz, an empty Notes field, the next occurrence in blue, and OK and Cancel buttons." />

*Adding a net: name, repeat pattern, time, reminder and frequency.*

- **Name**
- **Repeats:** **Once (no repeat)**, **Daily**, **Weekly** or **Monthly**,
  with **every N** days, weeks or months (1–52).
  - **Weekly:** tick the days of the week it meets.
  - **Monthly:** pick the week and day, for example "on the **second**
    Tuesday". The choices are first, second, third, fourth, fifth and last.
  - **Once:** pick the date.
- **At:** the start time and its time zone. The list offers your system time
  zone and **Etc/UTC**; you can type another zone name.
- **Remind me:** **At start time**, or **5**, **10**, **15** or **30 minutes
  before**, or **1 hour before**.
- **Frequency**, with **Capture current VFO** to copy the active slice's
  frequency, mode and filter (it needs an open slice on a connected radio).
- **Mode** and **Filter**.
- **Notes:** net control, NetLogger name and anything else you want to keep
  with it.

A live **Next:** line under the form confirms when the net will next run, so
you can check the recurrence before saving.

### Reminders

At reminder time a banner appears in the main window with:

- **Tune Now** — tunes to the net's frequency, mode and filter, the same way
  recalling a memory channel does. It needs a connected radio with an open,
  unlocked slice; otherwise the status bar says what to do.
- **Dismiss** — closes the reminder.

AetherSDR also tries to show a desktop notification.

### Import and export

**Export…** saves the schedule as a JSON file; **Import…** reads one back,
which is handy for sharing a club's net list or moving to another computer.
When an imported net matches one you already have, choose:

- **Skip existing** — keep yours.
- **Overwrite** — replace yours with the imported one.
- **Import as new** — keep both.

## Reference

### The schedule list

The window lists your nets with these columns:

<img src="/img/screens/net-scheduler-window.png" width="640" alt="Net Scheduler window. A table with columns On, Name, Repeats, Next, Frequency and Mode lists two nets: Tuesday 20 m Net, weekly, 14.2500 MHz USB, and Sunday 20 m Net, weekly, 14.3000 MHz USB, the second selected. Add…, Edit…, Remove, Disable, Tune Now, Import… and Export… buttons run along the bottom." />

*The Net Scheduler with two weekly nets added.*

| Column | Meaning |
|---|---|
| **On** | Whether reminders are enabled for this net |
| **Name** | The net's name |
| **Repeats** | How often it recurs |
| **Next** | The next date and time it starts |
| **Frequency** | Where it meets |
| **Mode** | The mode to tune |

Buttons: **Add…**, **Edit…**, **Remove**, **Enable** / **Disable**,
**Import…** and **Export…**.

## Troubleshooting

### Tune Now doesn't tune the radio

**Tune Now** needs a connected radio with an open, unlocked slice. The status
bar says what is missing.

1. Connect to the radio.
2. Open a slice, or unlock the active one.
3. Press **Tune Now** again.

### Capture current VFO does nothing

It copies from the active slice, so it needs an open slice on a connected
radio.

1. Connect to the radio and open a slice.
2. Press **Capture current VFO** again, or type the frequency, mode and
   filter by hand.

### A net doesn't remind you

Reminders are switched off for that net.

1. Select the net and press **Enable**. Its **On** column shows that reminders
   are on.

## See also

- [Memory Channels](./memory-channels.md) — for frequencies you want to recall at any time rather
  than on a schedule.
