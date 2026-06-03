# Bundled profiles

Pre-built Ulanzi Studio **profiles** (button + dial layouts) for AetherSDR Controller plugin actions.  Each profile maps the plugin's 8 actions onto a specific Ulanzi device's keys + dials so you don't have to drag actions one-by-one after installing the plugin.

## Available profiles

| File | Target device | Layout |
|---|---|---|
| `aethersdr-d100h-default.ulanziDeckProfile` | **Ulanzi D100H / KEHWIN Dial_Lite** (6 keys + 1 dial) | See [D100H layout below](#d100h-default-layout) |
| _(D200H / D200X / others)_ | Coming when the devices arrive | — |

## D100H default layout

Buttons (looking at the device):

```
┌─────────┐ ┌─────────┐ ┌─────────┐
│   MOX   │ │  RIT    │ │  TUNE   │   ← top row (LCD-style indicators)
└─────────┘ └─────────┘ └─────────┘
┌─────────┐ ┌─────────┐ ┌─────────┐
│ Band Up │ │ ╭───╮   │ │ Mode    │
└─────────┘ │ │DIAL ←─┼─┤ Cycle   │
┌─────────┐ │ ╰───╯   │ │         │
│Band Down│ │  Slice  │ └─────────┘
└─────────┘ └─────────┘
```

- **Dial (rotate)** = VFO step ±100 Hz; **press + rotate** = ×10 coarse; **press** = MOX toggle
- **Side buttons** carry the operating workflow (MOX / TUNE / RIT / Slice / Mode / Band)

## How to import a profile

1. Install the AetherSDR Controller plugin first (see the top-level [README](../README.md)).
2. Launch Ulanzi Studio with the D100H connected via Bluetooth.
3. **Studio → Profile menu → Import** (or the equivalent UI in your Studio version — there's an `+ Import` button on the Profile preferences pane).
4. Navigate to this directory and select `aethersdr-d100h-default.ulanziDeckProfile`.
5. Studio imports the profile and re-binds it to your specific device.
6. Done — every button + the dial should now drive AetherSDR via the plugin.

## Customising

Once the profile is imported, you can freely rearrange / replace actions in Studio's layout editor.  Studio saves your customisation as a new state of the profile; the file in this directory is the **starting point**, not a contract.

If you want to share your customisation back upstream (or just keep a personal backup), use Studio's **Profile → Export** to save the file, then either drop a PR adding it as a variant here or stash it in your own dotfiles.

## Caveats

- The bundled D100H profile was exported with a specific device-UUID binding (the export tool includes the original D100H's hardware ID).  Studio usually re-binds on import automatically, but if your imported profile doesn't respond to button presses, **delete + re-add** any action that won't fire to force a fresh per-device link.
- Profile format is `#Version: 2` followed by a standard ZIP archive containing `manifest.json` + per-page action mappings.  It's plain JSON inside, so the file's human-inspectable if you ever need to debug it.
