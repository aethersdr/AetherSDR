---
title: "Generated Reference"
description: "Reference tables generated from the AetherSDR source, so they always match the code."
sidebar_position: 1
custom_edit_url: null
generated_by: "tools/docs/gen_reference.py"
---

The pages in this section are generated from the AetherSDR source code, not written by hand, so they always match the version of the app they were built with.

- [Default Keyboard Shortcuts](./default-shortcuts.md): assignable keyboard actions and their default keys.
- [MIDI Controller Actions](./midi-actions.md): every control you can map to a MIDI controller.
- [FlexControl Actions](./flexcontrol-actions.md): FlexControl button actions and defaults.
- [Ulanzi Dial Actions](./ulanzi-dial-actions.md): Ulanzi Dial wheel actions and button defaults.
- [Stream Deck+ and HID Controller Actions](./hid-controller-actions.md): Stream Deck+, HID encoder and TMate 2 actions and defaults.
- [Log Categories](./log-categories.md): logging categories for support logs.
- [TCI Commands](./tci-commands.md): command names the TCI server recognises.

## Refreshing these pages

Run this from the top of the source tree after changing any of the source files a page names:

```bash
python3 tools/docs/gen_reference.py
```

`python3 tools/docs/gen_reference.py --check` changes nothing and exits with an error, printing the difference, when a committed page no longer matches the source. CI runs it, so a change that alters one of these tables has to carry the regenerated page with it.
