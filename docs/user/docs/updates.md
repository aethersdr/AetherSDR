---
title: "Updates"
slug: "/updates"
description: "The Updates window shows the newest AetherSDR release, what is in it, and the download built for your computer."
status: "Supported"
applies_to: ["All radios"]
---

:::info[Status]

**Status:** Supported · **Applies to:** All radios

:::

The **Updates** window shows whether a newer AetherSDR is available, what is in it, and the download built for your computer. AetherSDR never downloads or installs an update by itself: **Get AetherSDR** opens the download in your browser, and you install it as you installed AetherSDR.

## Requirements

- An internet connection that reaches GitHub (`api.github.com`). The check sends one request with AetherSDR's version; there is no AetherSDR server and nothing else is sent.

## Using the Updates window

Open it with **File → Update...**, by clicking the **UPDATE** badge in the title bar, or with **Help → Check for Updates...**, which also checks again right away. Opening it when AetherSDR knows nothing about the newest release starts a check.

When a newer release exists, the window shows:

- **NEW RELEASE** with its date, the version, and the release's one-line summary.
- **WHAT'S INSIDE**: one chip for each chapter of the release notes.
- **Get AetherSDR** *version*: opens the download in your browser. The line under it names the file: the Windows installer, the Windows portable zip if you run the portable copy, the macOS Apple silicon disk image, or the Linux AppImage for your processor. When there is no build for your computer, it opens the release page instead.
- **Full release notes**: opens the release page on GitHub.
- **Skip this version**: hides the **UPDATE** badge for this release. The window still shows it, and the next release shows the badge again.

If GitHub can't be reached, the window still shows a newer release that an earlier check found, with **Get AetherSDR** (which then opens the release page), **Skip this version** and **Check Now**, and a note that the release's details are missing.

When you have the newest release, the window says **UP TO DATE** and shows what came in your version. **Check Now** asks GitHub again.

**Check daily** is the same setting as **Help → Check for Updates Automatically**; changing either changes both. It is on by default: AetherSDR checks about 15 seconds after it starts, no more than once a day, counting a check that could not reach GitHub. A newer release lights the **UPDATE** badge after the **AetherSDR** brand mark; nothing pops up, and a check that finds nothing or cannot reach GitHub stays silent. The badge stays on across restarts until you update or skip that release. Turning **Check daily** off also removes the badge.

On a store-managed install (the Microsoft Store, or a Flatpak build), the store updates AetherSDR: there is no automatic check, no badge and no download button, **Check daily** is dimmed, and the window says where updates come from.

## Troubleshooting

### The window says COULDN'T REACH GITHUB

1. Check that this computer can open https://github.com in a browser.
2. If you use a proxy or firewall, allow `api.github.com`.
3. Click **Check Now**.

### The UPDATE badge doesn't appear

1. Open **File → Update...** and see what the window reports.
2. If it shows the release with **Skipped**, you skipped it; the next release shows the badge again.
3. Check that **Check daily** is on.

## See also

- [Menu Reference](./menu-reference.md)
- [Firmware Update](./firmware-update.md)
