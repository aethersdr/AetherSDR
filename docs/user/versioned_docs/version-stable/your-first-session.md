---
title: "Your First Session"
slug: "/your-first-session"
description: "This tutorial walks you through AetherSDR for the first time using the built-in demo, so you need no radio, no antenna and no licence."
---

This tutorial walks you through AetherSDR for the first time using the built-in demo, so you need no radio, no antenna and no licence. In about 20 minutes you will connect, tune, change mode and filter, add a panadapter, clean up a noisy band, change the display, save a memory and disconnect. Every step tells you what you should see, so you know it worked.

The demo is a simulator that generates its own receive audio and spectrum. **It cannot transmit**, so nothing you do here reaches the air. It also has fewer features than a real radio: one slice, USB and LSB audio, and no DAX. See [Demo Mode](./demo-mode.md) for everything it offers.

## Requirements

- AetherSDR installed on Linux, macOS or Windows. See [Installation](./installation.md).
- Speakers or headphones on your computer.

## Step 1: Start AetherSDR

1. Start AetherSDR.

**You should see** the **Connect to Radio** window. If it does not open (for example because AetherSDR connected to a radio you used before), choose **File → Connect to Radio...**. On Windows and Linux the menus are behind the **☰** button at the left of the title bar; on macOS they are in the system menu bar.

## Step 2: Connect to the demo

<img src="/img/screens/first-session-main-window.png" width="1600" alt="AetherSDR main window connected to the demo. The title bar has a tab reading Simulator (not on the air). The panadapter shows a narrow spectrum around 14.100 MHz with the slice A VFO flag reading 14.100.000 USB and a speckled blue waterfall below. The applet panel on the right shows the S-Meter, RX Controls, Demo Noise and TX Controls applets; in Demo Noise, Pink / hiss and Birdie carrier are lit." />

*The main window just after connecting to the demo: panadapter and waterfall on the left, applets on the right.*

1. In the Connect to Radio window, choose **On This Network**.
2. Under **Available radios**, find **AetherSDR Demo**, labelled **"Simulator (not on the air)"**. It always sorts below any real radios.
3. Select it and click **Connect Selected Radio**.

**You should see** the main window come to life:

- a **panadapter**: the spectrum on top and the waterfall scrolling below;
- a **VFO** for slice **A** on **14.100 MHz** in **USB**, with a 100–2900 Hz filter;
- a tab for the demo in the title bar;
- the **Demo Noise** applet in the applet panel, with **Pink / hiss** and **Birdie carrier** lit.

**You should hear** a soft hiss with a steady tone in it: the band noise and the birdie carrier.

If the demo is not in the list, tick **Show the AetherSDR demo simulator** in the Connect to Radio window.

The demo panadapter is a narrow 8 kHz window that follows your frequency. A real radio shows much wider spans.

## Step 3: Tune

<img src="/img/screens/first-session-rx-freq-entry.png" width="248" alt="The top of the RX Controls applet: slice A, a lock button, ANT1 and ANT1, filter width 2.8K, a TX badge, USB, and the frequency 14.100000 highlighted for editing in a blue box." />

*Double-click the frequency in RX Controls to type a new one.*

1. Point at the spectrum and turn the mouse wheel one notch.

   **You should see** the frequency change by **100 Hz**, the slice's step size.

2. In the **RX** applet, double-click the frequency, type `14.074` (a frequency in MHz) and press Enter.

   **You should see** the VFO move to **14.074 MHz**.

You can also double-click anywhere on the spectrum or waterfall to tune there. See [Panadapter Controls](./panadapter-controls.md) for every mouse action.

## Step 4: Change the mode

<img src="/img/screens/first-session-lsb-passband.png" width="760" alt="Demo panadapter in LSB around 14.100 MHz. The slice A VFO flag reads 14.100.000 with LSB on its mode tab, and the shaded passband lies just below the VFO line." />

*In LSB the passband (shaded) moves below the VFO line.*

1. On the VFO, open the mode list (it shows **USB**) and choose **LSB**.

   **You should see** the mode read **LSB**, and the shaded passband on the panadapter move from above the frequency line to below it.

2. Choose **USB** again.

   **You should see** the passband move back above the frequency line.

The mode list offers every mode a FlexRadio supports, but the demo's receiver is built for USB and LSB.

## Step 5: Change the filter

1. In the **RX** applet, find the filter preset buttons and click **2.4K**.

   **You should see** the shaded passband on the panadapter narrow, and the filter width in the RX applet header change to match.

2. Click **2.9K** to widen it again.

See [RX Controls](./rx-controls.md) for the presets in each mode.

## Step 6: Add a panadapter

The demo has a single slice, so the **+RX** button does not add a second one here. On a real radio it does; see [Multi-Slice Operation](./multi-slice-operation.md). The demo can show up to four panadapters, so add one instead.

<img src="/img/screens/first-session-layout-dialog.png" width="560" alt="Panadapter Layout window headed Choose panadapter layout. A grid of layout thumbnails, each labelled with its arrangement and pan count: A / B (2 pans), A | B (2 pans), A|B / C (3 pans), A / B|C (3 pans), A / B / C (3 pans), A|B / C|D (4 pans), A/B/C/D (4 pans) and Single (1 pan), highlighted. Layouts for five to eight pans are dimmed. A Cancel button sits at the bottom." />

*The Panadapter Layout window. The demo can show up to four panadapters, so larger layouts are dimmed.*

<img src="/img/screens/first-session-two-pans.png" width="1340" alt="Two demo panadapters stacked one above the other, both tuned around 14.100 MHz with a green spectrum trace. The top one carries the slice A VFO flag at 14.100.000 and a speckled blue waterfall; the new panadapter below has its own overlay menu and spectrum." />

*Two panadapters stacked after choosing A / B (2 pans).*

1. Choose **Tools → Add Panadapter...**.

   **You should see** the **Panadapter Layout** window with **Choose panadapter layout:** and a set of layout buttons.

2. Click **A / B (2 pans)**.

   **You should see** a second panadapter appear with the first. It has no slice on it: in the demo, extra panadapters are extra views of the same simulated band.

3. Choose **Tools → Add Panadapter...** again and click **Single (1 pan)** to go back to one panadapter.

## Step 7: Clean up a noisy band

Make some noise, add a voice to listen to, then let AetherSDR's noise reduction work on it.

<img src="/img/screens/first-session-demo-noise-noisy.png" width="248" alt="Demo Noise applet with Voice (speech), Pink / hiss, Power-line and SMPS hash lit, after the noisy-qth preset and the Voice toggle." />

*Demo Noise after choosing noisy-qth and turning on Voice (speech).*

<img src="/img/screens/first-session-aetherrx-nnr.png" width="720" alt="AetherRX window. AetherNR is selected in the stage column and its enable box is green. The NNR method tab is highlighted, showing Neural NR with a description, a Strength slider at 38, Model Standard or Premium, and Advanced sliders for Alpha, Knee, Tau, Max gain, Attack and Release. The status line reads METHOD NNR, Standard, floor 38, with an NR GAIN trace." />

*AetherRX with NNR running: the NNR tab is lit and the AetherNR stage is ticked.*

1. In the **Demo Noise** applet, click the **noisy-qth** preset.

   **You should see** **Pink / hiss**, **Power-line** and **SMPS hash** light up, and **you should hear** a buzz and a rough hash on top of the hiss.

2. Click **Voice (speech)** to turn it on.

   **You should hear** a voice in the noise. Hover over any source for a tooltip that explains the real-world cause.

3. Choose **Settings → AetherRX...**. On the **AetherNR** tab, click **NNR**.

   **You should see** the **NNR** button light and the **AetherNR** stage's tick box become ticked. **You should hear** the noise drop behind the voice.

4. Click **BYPASS** at the foot of the AetherRX window, then click it again.

   **You should hear** the unprocessed noise while BYPASS is on, and the cleaned-up audio when you turn it off.

NNR works on every platform with nothing to download. Try the other methods too. A method your computer cannot run is greyed out; hover over it for the reason (for example **MNR** outside macOS). The first time you choose **NR2**, it spends a while optimising itself in an **FFTW Wisdom** window. See [DSP Noise Mitigation](./dsp-noise-mitigation.md).

## Step 8: Open the Display panel

1. In the column of buttons at the left edge of the panadapter, click **Display**.

   **You should see** the Display panel open, with controls such as **Heat Map**, **Grid** and FFT averaging.

2. Click **Grid**.

   **You should see** the frequency and dB grid lines disappear from the panadapter. Click **Grid** again to bring them back.

3. Click **Display** again to close the panel.

See [Panadapter Controls](./panadapter-controls.md) and [Themes and Theme Editor](./themes-and-theme-editor.md) for more ways to change the look.

## Step 9: Save and recall a memory

<img src="/img/screens/first-session-save-memory-dialog.png" width="360" alt="Save Memory dialog: Name My first memory, Current Frequency: 14.100000 MHz, USB, Filter 100 to 2900 Hz, and Cancel and Save buttons." />

*The Save Memory dialog, opened with Add Memory on the Memory panel.*

<img src="/img/screens/first-session-saved-status.png" width="520" alt="The left end of the status bar reading Saved &quot;My first memory&quot; to memories." />

*After Save, the status bar confirms the memory.*

1. In the same button column, click **Memory**, then **Add Memory**.

   **You should see** a **Save Memory** window showing the current frequency, mode and filter.

2. Type a name, for example `My first memory`, and click **Save**.

   **You should see** **Saved "My first memory" to memories.** in the status bar.

3. Tune somewhere else, for example by scrolling the mouse wheel a few notches.
4. Choose **Tools → Memory...** and double-click the row for your memory.

   **You should see** the VFO return to the frequency you saved.

The demo keeps its memories on this computer. A FlexRadio keeps them on the radio. See [Memory Channels](./memory-channels.md).

## Step 10: Disconnect

<img src="/img/screens/first-session-file-menu.png" width="177" alt="File menu: Connect to Radio..., Disconnect and Quit." />

*The File menu.*

1. Choose **File → Disconnect**.

   **You should see** the demo session close. Open the **File** menu again and **Disconnect** is greyed out, because nothing is connected.

Because you disconnected yourself, AetherSDR does not reconnect to the demo the next time it starts.

## Going further

- **Fault injection:** the **Fault Injection** row in the Demo Noise applet shows how AetherSDR handles trouble: a high SWR, a dropped slice, a stalled spectrum, a lost connection or a malformed message. **Clear** undoes them. A dropped slice comes back only after you disconnect and reconnect. See [Demo Mode](./demo-mode.md).
- **Other presets:** **storm**, **night-40m** and **cw-in-noise** each set up a different band to practise on.
- **Your own radio:** connect it with [First Connection](./first-connection.md), then work through [Before You Transmit](./before-you-transmit.md) before you key up for the first time.

## See also

- [Demo Mode](./demo-mode.md)
- [First Connection](./first-connection.md)
- [Before You Transmit](./before-you-transmit.md)
- [Panadapter Controls](./panadapter-controls.md)
- [DSP Noise Mitigation](./dsp-noise-mitigation.md)
- [Menu Reference](./menu-reference.md)
