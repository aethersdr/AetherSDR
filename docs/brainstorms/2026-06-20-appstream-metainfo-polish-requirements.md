---
date: 2026-06-20
topic: appstream-metainfo-polish
---

# Polish AetherSDR AppStream Metainfo

## Summary

Edit `packaging/linux/io.github.aethersdr.aethersdr.metainfo.xml` so `appstreamcli validate` reports zero info/pedantic notices and the file is Flathub-submittable. Single-file change; no C++ or build-system work.

## Requirements

- R1. Add an OARS content rating with an all-none config to clear the `content-rating-missing` notice.
- R2. Add a `<developer>` element to clear the `developer-info-missing` notice.
- R3. Add a `<releases>` element listing the 5 most recent tagged versions, each with `version` and `date` attributes.
- R4. Rewrite the summary so it neither starts with the article "A" nor ends with a period, per AppStream style.
- R5. Update the default screenshot to use a tagged-commit URL (not `refs/heads/main`) and add a `<caption>`. `width` and `height` attributes are optional.
- R6. Expand the `<description>` with a `<ul>` feature list (panadapter, SmartSDR protocol, audio DSP, etc.) below the existing paragraph.

## Acceptance

`appstreamcli validate packaging/linux/io.github.aethersdr.aethersdr.metainfo.xml` exits clean with zero info/pedantic notices.

## Key Decisions

- **Include the optional description expansion.** Low cost, raises the value the file carries in software centers, stays within the same single-file edit.
- **All-none OARS rating.** AetherSDR is amateur-radio software with no objectionable content; the all-none config matches Flathub's permissive-default policy.
- **5 release entries.** Matches the recent-release cadence and gives software centers a useful version history without bloat.

## Outstanding Questions

- Deferred to Planning: exact `<developer>` text (likely "AetherSDR contributors" — planner should confirm against project conventions); whether to add `width`/`height` on the screenshot (Flathub generally accepts omission).

## Sources

- GitHub issue #3675 — https://github.com/aethersdr/AetherSDR/issues/3675
- Current metainfo file as of 2026-06-20
- Most recent releases: v26.6.3 (2026-06-14), v26.6.2 (2026-06-08), v26.6.1.1 (2026-06-02), v26.6.1 (2026-06-01), v26.5.3 (2026-05-24)
