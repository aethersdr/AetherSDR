# Multi-Panadapter Implementation Pitfalls

Extracted from CLAUDE.md for on-demand reference. Read this when working
on multi-pan layout, wirePanadapter(), or slice routing.

### Multi-Pan Implementation Pitfalls (lessons learned)

1. **`QString::toUInt("0x40000001", 16)` returns 0.** Qt's `toUInt` with
   explicit base 16 does NOT handle the `0x` prefix. Use base 0 (auto-detect).
   This silently broke all stream ID comparisons.

2. **`handlePanadapterStatus()` must dispatch by panId.** The `display pan`
   status object name contains the pan ID — pass it through. Never apply pan
   status to `activePanadapter()` unconditionally.

3. **Waterfall ID arrives AFTER pan creation.** The `display pan` status
   message contains `waterfall=0x42xxxxxx` but it arrives after the
   PanadapterModel is created. Connect `waterfallIdChanged` to
   `updateStreamFilters()` to register the wf stream when it arrives.

4. **Don't force-associate waterfalls to pans.** The radio's `display pan`
   status correctly sets `waterfallId` via `applyPanStatus`. Manual
   association logic assigns to the wrong pan (first-empty-slot race).

5. **Display overlay connections must be per-pan.** Wire each
   PanadapterApplet's overlay menu in `wirePanadapter()`, not globally in
   the constructor. Each overlay sends commands with its own panId/waterfallId.

6. **Push `xpixels`/`ypixels` to each new pan on creation.** The radio
   defaults to `xpixels=50 ypixels=20` which produces empty FFT bins.
   Send actual widget dimensions immediately after `panadapterAdded`.

7. **Never send `slice set <id> active=1`.** Active slice is managed
   entirely client-side. The radio bounces `active` between slices when
   two share a pan, creating infinite feedback loops. See pitfall #16.

8. **Use `slice m <freq> pan=<panId>` for cross-pan click-to-tune only.**
   For same-pan tuning (scroll wheel, click on active slice's pan), use
   `onFrequencyChanged()` → `slice tune <sliceId>`. See pitfall #18.
   `slice m` does NOT recenter the pan when crossing band boundaries.

9. **Band changes need `slice tune` + `slice m`.** `slice tune <id> <freq>`
   recenters the pan's FFT/waterfall on the new band. `slice m <freq>
   pan=<panId>` updates the VFO frequency. Both are needed for a complete
   cross-band change in multi-pan mode. In single-pan mode, use
   `onFrequencyChanged()` which handles everything.

10. **Band change handler must target the pan's slice, not `activeSlice()`.**
    Use `sl->panId() == applet->panId()` to find the correct slice for each
    pan's overlay. Falling back to `activeSlice()` causes all band changes
    to affect slice A.

11. **Band stack save must validate frequency vs band.** In multi-pan mode,
    the save handler's `activeSlice()` may return a slice on a different band.
    Use `BandSettings::bandForFrequency()` to verify the frequency belongs to
    the band before saving. Skip the save if they don't match (prevents
    cross-band contamination).

12. **Disconnect dying pan widgets before removal.** When a pan is removed
    (layout reduction), disconnect all signals from its SpectrumWidget and
    OverlayMenu to MainWindow BEFORE the widget is destroyed. This prevents
    all `wirePanadapter` lambdas from calling into dead objects. One-shot
    global fix — covers all current and future lambdas.

13. **Preamp (`pre=`) is shared antenna hardware.** When any `display pan`
    status contains `pre=`, apply it to ALL pans sharing the same antenna,
    not just the pan the status belongs to. Multi-Flex filtering must not
    block preamp updates — they are SCU-level state, not per-client.

14. **Filter polarity normalization.** The radio sometimes sends wrong-polarity
    filter offsets after session restore (e.g. `filter_lo=-2700 filter_hi=0`
    for DIGU). Normalize in `applyStatus()` based on mode: USB/DIGU/FDV must
    have `filterLo >= 0`, LSB/DIGL must have `filterHi <= 0`.

15. **`FWDPWR` meter source is `TX-` (with trailing dash), not `TX`.**
    Use `startsWith("TX")` for matching, not exact equality.

16. **Never send `slice set <id> active=1` — not even in single-pan mode.**
    The radio bounces `active` between slices when two share a pan, creating
    an infinite feedback loop (slice 0 active → we send active=1 for 0 →
    radio sets slice 1 active → we react → loop). SmartSDR manages active
    entirely client-side. The radio sets `active=` as a side-effect of
    `slice m` commands. Removed `s->setActive(true)` from `setActiveSlice()`
    entirely.

17. **`activePanChanged` must sync ALL slice-dependent UI.** In multi-pan mode,
    `setActiveSlice()` does NOT fire on pan click (pitfall #7). So step size,
    CW decode, applet panel controls, and overlay menu slice binding must all
    be synced in the `activePanChanged` handler. Use `setActivePanApplet()`
    to rewire CW decoder connections.

18. **Click-to-tune must not switch slices within the same pan.** The
    `frequencyClicked` handler should only switch active slice when clicking
    on a DIFFERENT pan. When multiple slices share a pan, always tune the
    current active slice via `onFrequencyChanged()` → `slice tune`. Switching
    to the other slice on each scroll event causes both VFOs to move.

19. **+RX must target the button's own pan, not `m_activePanId`.** The
    `addRxClicked` signal must carry the panId from `SpectrumOverlayMenu`.
    Use `RadioModel::addSliceOnPan(panId)` with explicit panId in the
    `slice create pan=<panId>` command.

20. **`setActivePanApplet()` rewires CW decoder.** When the active pan
    changes, disconnect `textDecoded`/`statsUpdated`/pitch/speed signals
    from the old applet and reconnect to the new one. The CW decoder is
    a singleton — its output must follow the active pan.

