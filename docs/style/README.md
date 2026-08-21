# docs/style/

Visual and interaction conventions for AetherSDR's UI. Read these
before adding a new applet, dialog, or shared widget so the result
feels at home next to existing screens.

- [`theme-style-guide.md`](theme-style-guide.md) — **read this first
  for anything with a colour in it.** The semantic map from system
  states (error, warning, success, notification, TX/RX, selection…)
  to ThemeManager tokens, the no-new-literals rule, and the
  add-a-token path for genuinely new UX.
- [`applet-style-guide.md`](applet-style-guide.md) — colors, fonts,
  spacing, button states, and the dark-techy aesthetic that ties the
  applet panel together.
- [`dialog-patterns.md`](dialog-patterns.md) — settings persistence,
  modal vs. modeless, escape handling, and the `PersistentDialog`
  proposal in issue #2605.

If you find yourself reaching for an exception to one of these
patterns, file an issue rather than diverging quietly — drift accrues
fast across an applet zoo.
