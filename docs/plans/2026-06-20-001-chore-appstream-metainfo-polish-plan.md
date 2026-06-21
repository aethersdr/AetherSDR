---
title: "chore: Polish AppStream metainfo for Flathub submission"
type: chore
date: 2026-06-20
origin: docs/brainstorms/2026-06-20-appstream-metainfo-polish-requirements.md
---

# chore: Polish AppStream metainfo for Flathub submission

## Summary

Edit `packaging/linux/io.github.aethersdr.aethersdr.metainfo.xml` so `appstreamcli validate` reports zero info/pedantic notices and the file is Flathub-submittable. Six co-located XML edits, no C++ or build-system work, no runtime behavior change.

## Problem Frame

`packaging/linux/io.github.aethersdr.aethersdr.metainfo.xml` (introduced in #3673, merged as `a01a089`) currently triggers several `appstreamcli validate` notices: missing content rating, missing developer, missing releases, an unstable `refs/heads/main` screenshot URL, and a non-conformant summary. Flathub requires all of these to be addressed before the file can be submitted; software centers also use the developer, releases, and rating elements to display the app. Polishing the file is the next smallest step toward a Flathub listing and a richer software-center experience.

## Requirements

### AppStream validation

- R1. The file declares an OARS content rating with an all-none config so `appstreamcli` does not flag `content-rating-missing`.
- R2. The file declares a `<developer>` element so `appstreamcli` does not flag `developer-info-missing`.
- R3. The file declares a `<releases>` element listing the 5 most recent tagged versions, each with `version` and `date` attributes.
- R4. The summary neither starts with the article "A" nor ends with a period, per AppStream style.
- R5. The default screenshot uses a tagged-commit URL (not `refs/heads/main`) and carries a `<caption>`. `width` and `height` are optional.

### Description content

- R6. The `<description>` adds a `<ul>` feature list (panadapter, SmartSDR protocol, audio DSP, etc.) below the existing paragraph.

## Key Technical Decisions

- **All-none OARS rating.** AetherSDR is amateur-radio software with no objectionable content; the all-none config matches Flathub's permissive-default policy. Selecting per-category ratings would falsely imply content categories the project does not actually ship.
- **`<developer>` text: "AetherSDR".** The project's identity is the application, not a corporate entity. "AetherSDR" matches the convention used in the existing `<name>` element and reads correctly in software centers.
- **5 release entries.** Captures the recent-release cadence (a release every 1-2 weeks) and gives software centers a useful version history without bloating the file. Older releases can be added later if Flathub requires more depth.
- **Screenshot URL points to `v26.6.3`.** The latest tagged release at the time of writing; using a tag (not `main`) keeps the URL stable across further commits. Width and height are omitted — Flathub accepts the omission and `appstreamcli` does not flag it.
- **Description feature list derived from the README.** The list of features (panadapter, SmartSDR protocol, audio DSP, etc.) should be sourced from the project's actual capabilities as documented in `README.md`, not invented. The implementer should lift 4-6 representative features from the README's feature list.

## Scope Boundaries

### Deferred to Follow-Up Work

- **CI check for metainfo validity.** A GitHub Actions job that runs `appstreamcli validate` on every PR touching the metainfo file would prevent regressions. Out of scope for this change; worth a separate issue.
- **Translation support.** AppStream supports `<translation>` elements for localized metadata. Not requested in #3675 and not required by Flathub.
- **Sponsorship / donation URLs.** Optional AppStream elements; out of scope.

## System-Wide Impact

- **Linux packagers and Flathub reviewers.** This change unblocks Flathub submission and is the prerequisite for any further metainfo-related work.
- **Software center users.** Indirectly: the description, developer, releases, and rating display in GNOME Software, KDE Discover, elementary AppCenter, and other libAppStream consumers.
- **No runtime or build-system impact.** The C++ application is unaffected; the change is pure packaging metadata.

## Risks & Dependencies

- **`<developer>` text acceptance.** Flathub has historically been strict about `<developer>` content. If the chosen text is rejected, the file is one edit away from a fix — no plan restructure needed.
- **Release-list coverage.** If the project wants 10 releases listed rather than 5, the diff grows but the structure stays the same. The 5-entry count is a default, not a hard limit.
- **No new dependencies.** `appstreamcli` ships in standard Linux distributions (`appstream` package on Debian/Ubuntu, `appstream` on Fedora/Arch). No install or pinning required for local development.

## Implementation Units

### U1. Apply all six XML edits to the metainfo file

- **Goal:** Land the six AppStream corrections and the description expansion in a single coherent edit.
- **Requirements:** R1, R2, R3, R4, R5, R6
- **Dependencies:** None
- **Files:** `packaging/linux/io.github.aethersdr.aethersdr.metainfo.xml`
- **Approach:**
  - Read the current file (already known from the brainstorm; current content has name, summary, description, launchable, screenshots, urls) and identify anchor points for each edit.
  - Insert the OARS `<content_rating>` element with `<content_rating id="oars-1.0">` and the all-none config block.
  - Insert the `<developer>` element with the project name.
  - Insert the `<releases>` block with the 5 most recent tagged versions.
  - Edit the `<summary>` to drop the leading "A" and trailing period.
  - Update the screenshot URL to point to `v26.6.3` and add a `<caption>`.
  - Expand `<description>` with a `<ul>` feature list sourced from `README.md`.
- **Patterns to follow:** Freedesktop AppStream metainfo spec for element shape and ordering; the file's existing XML style (2-space indent, attribute alignment).
- **Test scenarios:** This is a packaging change; behavioral tests are not applicable. `Test expectation: none` — verification is the validator run in U2.
- **Verification:** The file is well-formed XML; all six required changes are visible in the diff; the diff is reviewable in a small set of hunks.

### U2. Validate with `appstreamcli` and confirm clean output

- **Goal:** Confirm the file passes `appstreamcli validate` clean.
- **Requirements:** R1, R2, R3, R4, R5
- **Dependencies:** U1
- **Files:** `packaging/linux/io.github.aethersdr.aethersdr.metainfo.xml` (read-only)
- **Approach:**
  - Run `appstreamcli validate packaging/linux/io.github.aethersdr.aethersdr.metainfo.xml` and capture the output.
  - Confirm exit code 0 and no `W:` (warning) or `I:` (info) lines.
  - If warnings or info notices remain, amend U1's diff and re-run.
- **Patterns to follow:** `appstreamcli` is the standard Flathub-submission validator.
- **Test scenarios:** This unit IS the test, executed by running the validator. `Test expectation: none` — the validator run is the verification.
- **Verification:** Validator output shows no warnings or info notices. The file is Flathub-submittable.

## Sources & Research

- GitHub issue #3675 — https://github.com/aethersdr/AetherSDR/issues/3675
- Brainstorm requirements doc: `docs/brainstorms/2026-06-20-appstream-metainfo-polish-requirements.md` (currently at `C:/tmp/aethersdr-2026-06-20-appstream-metainfo-polish-requirements.md` until the AetherSDR repo is cloned)
- Freedesktop AppStream metainfo specification — defines `<content_rating>`, `<developer>`, `<releases>`, `<screenshot>` structure and ordering rules.
- AetherSDR release list (5 most recent): v26.6.3 (2026-06-14), v26.6.2 (2026-06-08), v26.6.1.1 (2026-06-02), v26.6.1 (2026-06-01), v26.5.3 (2026-05-24)
