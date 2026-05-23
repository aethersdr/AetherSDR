# Contributing to MasterSDR

## Why Feedback Matters

MasterSDR grows through the ideas, bug reports, testing notes, and operating experience shared by the people who use it. The future of software defined radio is shaped by operators who take a moment to say what worked well, what felt confusing, and what would make the station experience even better.

Every contribution helps move the project forward:

- layout feedback that makes the window easier to read at a glance
- workflow ideas that make operating smoother and more intuitive
- bug reports that help close the gap between the software and real station use
- documentation improvements that help the next operator get on the air faster

You do not need to write code to make a real impact. If you operate, experiment, explore, test, or simply notice where the app could be clearer, your feedback is welcome and genuinely valuable.

## The Most Helpful Kinds of Contributions

Useful contributions include:

- clear bug reports
- operator-focused feature requests
- screenshots that show a confusing layout or state mismatch
- testing on unusual radios, operating systems, or network conditions
- better wording for menus, tooltips, help guides, and setup instructions

High-value feedback often comes from moments like these:

- "I could not tell which slice owned transmit."
- "The right control exists, but it is buried in the wrong window."
- "The panadapter looked correct, but the applet state was misleading."
- "A remote or multiFLEX workflow made the status hard to read."

Those are exactly the kinds of observations that make the software safer and easier to operate.

## Project Home

GitHub:

[https://github.com/MasterSDR/MasterSDR](https://github.com/MasterSDR/MasterSDR)

This is the main place for issues, discussions, releases, and pull requests.

## Reporting Bugs Well

When filing a bug, include enough context that someone else can reproduce both the station state and the screen state.

### Minimum information

1. Operating system
2. MasterSDR version
3. Radio model
4. Local LAN or SmartLink remote use
5. What you expected to happen
6. What actually happened
7. Whether the issue is repeatable

### Extra details that are especially helpful

- which menu path or button you used
- which window or dialog was open
- which slice letter was active
- which mode you were in
- whether the slice also owned transmit
- whether the applet panel was visible
- whether `Minimal Mode` was on
- whether `PC Audio` was on
- whether external devices such as MIDI, FlexControl, Stream Deck, or USB serial hardware were involved

### For layout or clarity bugs

Include a full-window screenshot if possible, not just a cropped control. Layout problems are often caused by the relationship between the title bar, panadapter, applet panel, and status bar rather than one isolated widget.

### For digital-mode bugs

Also include:

- whether you used CAT over TCP, CAT over TTY, or TCI
- the port or channel you used
- whether DAX was enabled
- whether receive worked, transmit worked, both worked, or neither worked

### For transmit-path bugs

State clearly whether RF was actually transmitted or whether the bug was caught before transmitting. That helps prioritize safety issues correctly.

## Use the In-App Tools

Before filing a report, check the built-in tools:

- `Help -> Support...` gathers logs, settings, and support-bundle information.
- `Help -> Slice Troubleshooting...` exposes detailed slice and state information that can explain many operating mismatches.
- The lightbulb button in the title bar opens the feature-request and AI-assisted reporting workflow.

These tools can save time for both operators and maintainers.

## Feature Requests That Lead to Good Design

A good feature request starts with an operating problem, not just a missing button.

Useful requests usually explain:

1. what task you were trying to perform
2. which current window, menu, or applet you had to use
3. why the current workflow is slow, confusing, or unsafe
4. what change would reduce friction or operator error
5. whether the request matters most for local, remote, contest, CW, voice, or digital use

Requests about layout are especially valuable when they describe what needs to stay visible at the same time. For example:

- transmit ownership plus tuner state
- DIGI controls plus active slice
- spots plus panadapter plus memory recall

## Documentation Contributions

Documentation is part of the operator experience. If something in the app is accurate but still confusing, that is a valid contribution target.

Helpful documentation feedback includes:

- menu labels that are unclear
- setup steps that assume too much prior knowledge
- controls whose name does not match the operator's mental model
- missing explanations for which settings are global and which are slice-specific
- missing examples for remote, digital, or multi-operator workflows

When suggesting a doc fix, cite the exact menu path, dialog tab, or control label so the wording can be matched to the UI.

## Testing Contributions

Testers are especially helpful when they can describe the environment around the bug:

- radio model and firmware family
- LAN vs WAN
- accessory hardware
- operating mode
- whether more than one slice or panadapter was open
- whether another multiFLEX station was connected

Reports from less-common environments are often the ones that uncover the most important edge cases.

## If You Want to Contribute Code

Read the repository guidance first:

- `README.md`
- `CONTRIBUTING.md`
- `CLAUDE.md`

Those files explain the project's expectations, build flow, conventions, and architecture.

### Useful source-tree landmarks

- `src/core`: protocol, transport, audio, and integration infrastructure
- `src/models`: radio, slice, transmit, panadapter, cable, and settings state
- `src/gui`: the operator-facing windows and controls

### GUI files that matter often

- `MainWindow`: top-level layout and menu wiring
- `TitleBar`: station state and top-strip controls
- `AppletPanel`: right-side control stack
- `SpectrumOverlayMenu`: left-side panadapter quick controls
- `RadioSetupDialog`: radio-wide configuration
- `MemoryDialog`, `ProfileManagerDialog`, `MultiFlexDialog`, and `SpotHub` dialogs for workflow support

### Project expectations

- use Qt6 and C++20 patterns
- use `AppSettings`, not `QSettings`
- keep classes focused
- protect slice and transmit correctness
- do not break core receive flow while adding features elsewhere

Pull requests should follow the project conventions, pass CI, and use GPG-signed commits.

## AI-Assisted Development

The project actively uses AI-assisted workflows for development, debugging, and triage. That makes clear structure even more valuable:

- precise reproduction steps
- exact menu paths
- exact control names
- logs and screenshots with context

The clearer your report is, the easier it is for both humans and tools to turn that report into a fix.

## A Simple Way to Start Helping

If you want an easy first contribution:

1. operate normally for a while
2. write down one moment where the UI forced you to stop and think
3. identify the window, menu, or applet involved
4. describe what information should have been clearer or what action should have been easier

That kind of feedback often turns directly into a better window layout, a safer workflow, or a better help guide.
