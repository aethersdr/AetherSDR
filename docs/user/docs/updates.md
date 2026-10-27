---
title: "Updates"
slug: "/updates"
description: "The Updates window shows the newest AetherSDR release and what is in it, and installs it on Windows or opens the download built for your computer."
status: "Supported"
applies_to: ["All radios"]
---

:::info[Status]

**Status:** Supported · **Applies to:** All radios

:::

The **Updates** window shows whether a newer AetherSDR is available and what is in it. On Windows, **Install update** downloads it and installs it when AetherSDR restarts or quits. Elsewhere, **Get AetherSDR** opens the download built for your computer in your browser, and you install it as you installed AetherSDR. AetherSDR never downloads or installs an update until you ask it to.

## Requirements

- An internet connection that reaches GitHub (`api.github.com`). The check sends one request with AetherSDR's version; there is no AetherSDR server and nothing else is sent.

## Using the Updates window

Open it with **File → Update...**, by clicking the **UPDATE** badge in the title bar, or with **Help → Check for Updates...**, which also checks again right away. Opening it when AetherSDR knows nothing about the newest release starts a check.

When a newer release exists, the window shows:

- **NEW RELEASE** with its date, the version, and the release's one-line summary.
- **WHAT'S INSIDE**: one chip for each chapter of the release notes.
- **Install update** (Windows): see [Installing an update on Windows](#installing-an-update-on-windows). **Download installer** beside it opens the download in your browser instead, if you'd rather install it yourself.
- **Get AetherSDR** *version* (macOS, Linux, and Windows when it can't install in place): opens the download in your browser. The line under it names the file: the Windows installer, the Windows portable zip if you run the portable copy, the macOS Apple silicon disk image, or the Linux AppImage for your processor. When there is no build for your computer, it opens the release page instead.
- **Full release notes**: opens the release page on GitHub.
- **Skip this version**: hides the **UPDATE** badge for this release. The window still shows it, and the next release shows the badge again. Skipping a release you already downloaded with **Install update** removes the download, so it doesn't install. Clicking **Install update** on a release you skipped un-skips it.

If GitHub can't be reached, the window still shows a newer release that an earlier check found, with **Get AetherSDR** (which then opens the release page), **Skip this version** and **Check Now**, and a note that the release's details are missing.

When you have the newest release, the window says **UP TO DATE** and shows what came in your version. **Check Now** asks GitHub again.

**Check daily** is the same setting as **Help → Check for Updates Automatically**; changing either changes both. It is on by default: AetherSDR checks about 15 seconds after it starts, no more than once a day, counting a check that could not reach GitHub. A newer release lights the **UPDATE** badge after the **AetherSDR** brand mark; nothing pops up, and a check that finds nothing or cannot reach GitHub stays silent. The badge stays on across restarts until you update or skip that release. Turning **Check daily** off also removes the badge.

On a store-managed install (the Microsoft Store, or a Flatpak build), the store updates AetherSDR: there is no automatic check, no badge and no download button, **Check daily** is dimmed, and the window says where updates come from.

## Installing an update on Windows

1. Click **Install update**. A progress bar shows the download. AetherSDR keeps running and nothing changes yet. **Cancel** stops it, and so does closing the window.
2. When it's ready, the button becomes **Restart to update**, and the title-bar badge reads **RESTART TO UPDATE**.
3. Either click **Restart to update**, which quits AetherSDR, installs the update and starts it again, or carry on: the update installs the next time you quit AetherSDR. It stays ready across restarts until then.

Most updates download only what changed since your version. When that isn't possible, AetherSDR downloads the whole release instead. Every file is checked against AetherSDR's signed list of the release before anything is installed, and checked again just before it goes in.

While it installs, a small updater swaps the new version in and starts it once with a quick check, without a window or the radio. If that check fails, your previous version is put back, and that release is never offered again. The previous version is kept beside the new one (in a folder named like your install with `.previous` after it) until the next update.

The next time AetherSDR starts, the status bar says what happened: **Updated to AetherSDR** *version*, or that the update didn't start and your previous version was restored.

Install update isn't offered when:

- AetherSDR is running as administrator. Run the installer instead; a system-wide install updates that way.
- The folder AetherSDR is installed in can't be changed by your user, for example under **Program Files**.
- A store installs your updates.
- The release was rolled back on this computer. The window says so, and offers **Get AetherSDR** if you want to try it by hand. The **UPDATE** badge doesn't show for it.

Windows copies that cannot install in place offer **Get AetherSDR**. Store-managed copies update through their store. During a radio firmware install, the update button waits until the job finishes, and an update ready to install waits for the next quit.

## Troubleshooting

### The window says COULDN'T REACH GITHUB

1. Check that this computer can open https://github.com in a browser.
2. If you use a proxy or firewall, allow `api.github.com`.
3. Click **Check Now**.

### The update didn't install

1. Look at the status bar when AetherSDR starts: it says whether the update went in, or why your previous version was kept.
2. If it says the update was rolled back, that release is not offered again; the next release is.
3. Use **Get AetherSDR**, or the installer, to install it by hand.

### The UPDATE badge doesn't appear

1. Open **File → Update...** and see what the window reports.
2. If it shows the release with **Skipped**, you skipped it; the next release shows the badge again.
3. Check that **Check daily** is on.

## See also

- [Menu Reference](./menu-reference.md)
- [Firmware Update](./firmware-update.md)
