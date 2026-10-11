---
title: "Workspace Canvas"
slug: "/workspace-canvas"
description: "The workspace canvas is an alternative to the standard (\"Classic\") layout."
status: "Experimental"
applies_to: ["All radios"]
---

:::info[Status]

**Status:** Experimental · **Applies to:** All radios

:::

The workspace canvas is an alternative to the standard ("Classic") layout. With it on, panadapters and applets become freely placed, resizable, layered items on a canvas, and the canvas can span several windows. Named workspaces remember which applets are open as well as where they sit, and a workspace can follow a radio profile.

The canvas is **off by default**. If you never turn it on, the Classic layout is unchanged and no canvas settings are written. Work remaining before the experimental label comes off: dragging items live between canvas windows, and more on-air time compared with the Classic layout.

## Setup

**View → Workspace Canvas ▸**

| Item | What it does |
|------|-------------|
| **Enabled** | Turns the canvas on or off. The choice is remembered. If the canvas can't start, the status bar shows "Workspace canvas unavailable: `<reason>`". |
| **Edit Layout** | Switches between the two postures (below). Only available while the canvas is on, and not remembered across restarts. |
| **Workspaces ▸** | The workspace switcher and workspace actions. |
| **Canvas Windows ▸** | Extra canvas windows. |

## Using the canvas

### Locked and edit postures

- **Locked** (the default) is for operating. Items stay put and every control works normally.
- **Edit** is for arranging. Turn it on with **View → Workspace Canvas → Edit Layout**, or **Edit layout** at the top of the canvas right-click menu.

### Placing items

In edit posture:

- **Move** an item by dragging its title bar; it follows the cursor.
- **Resize** with the eight grips on the item's 8 px border. The contents stay clickable.
- **Snapping:** edges and centres snap to other items and to the canvas edges within 8 px, with guide lines drawn while you drag. **Snap to grid** adds an optional 96 × 54 grid with a gentler pull; edges of other items win over the grid.

Positions are stored as fractions of the canvas, so a layout made on a 3840 × 1600 screen comes back in proportion on 1920 × 1080.

### Named workspaces

**View → Workspace Canvas → Workspaces ▸** lists your workspaces with a check mark on the active one. Switching restores both which applets are open and where everything sits.

| Item | What it does |
|------|-------------|
| **New from current layout…** | Copies what is on screen now. |
| **New from Classic…** | Starts from the Classic arrangement. |
| **New blank…** | Starts with an empty canvas. |
| **Rename active…** | Renames the current workspace. |
| **Delete active…** | Deletes it after confirmation ("Its arrangement is lost"). |
| **Import pop-outs onto canvas** | Docks every floating applet window and floating panadapter onto the canvas, at positions mapped from where the windows were. |
| **Bind to radio profile ▸** | Links this workspace to a global radio profile, so recalling that profile switches to the workspace. See [Profile Management](./profile-management.md). |

With the canvas off, the submenu just says "Enable the canvas to create workspaces".

### Canvas windows

**View → Workspace Canvas → Canvas Windows ▸** adds more top-level canvases, for example one per monitor.

- **New canvas window…** creates one; **Rename ▸** renames it.
- **Closing** a canvas window only hides it; its items are back exactly where they were when you reopen it.
- **Remove ▸** deletes a canvas window after confirmation; its items move back to the main canvas.
- To move an item to another window, use **Move to ▸** on the item. Dragging an item from one window to another is not supported yet.

### Minimal Mode and the applet panel

- Entering **Minimal Mode** (Ctrl+Shift+M) while the canvas is on pauses the canvas, with the notice "Workspace canvas will resume after leaving minimal mode". Leaving Minimal Mode brings the canvas back with its arrangement intact.
- **Ctrl+Shift+S** (pop out the applet panel) does nothing while the canvas is on, because the canvas owns the applets.

## Reference

### The canvas right-click menu

Right-click an empty part of the canvas:

| Item | When | What it does |
|------|------|-------------|
| **Edit layout** | Always | Toggles edit posture. |
| **Workspace ▸** | More than one workspace | Switch workspace. |
| **Add widget ▸** | Always | Every applet, grouped by category. Applets already on the canvas are shown checked. Applets for hardware that isn't detected (tuner, amplifiers, Antenna Genius, ShackSwitch, Demo Noise) are greyed rather than hidden. |
| **Snap to grid** | Edit | Turns the 96 × 54 grid on or off. |
| **Undo last placement** | Edit | Undoes the most recent placement. |
| **Tidy layout** | Edit | Tidies up the current arrangement. |
| **Reset layout to Classic** | Edit | Puts the canvas back to the Classic arrangement. |

Right-click an item in edit posture for **Move to ▸** (another canvas window), **Return to panel**, **Bring to front**, **Send to back** (panadapters only offer Send to back) and **Hide band stack**.

### Compatibility

A workspace file written by a newer AetherSDR release is refused rather than overwritten, so trying an older build won't damage a newer layout.

## Troubleshooting

### Floating panadapters don't come back where they were

A layout with floating panadapters did not restore properly.

1. Turn on **View → Workspace Canvas → Enabled**.
2. Choose **View → Workspace Canvas → Workspaces → Import pop-outs onto canvas**. Every floating applet window and floating panadapter is docked onto the canvas.

### Items won't move or resize

The canvas is in locked posture.

1. Turn on **View → Workspace Canvas → Edit Layout**, or choose **Edit layout** from the canvas right-click menu.
2. Arrange the items, then turn Edit Layout off again to operate.

### The canvas vanished after entering Minimal Mode

Minimal Mode pauses the canvas.

1. Press **Ctrl+Shift+M** to leave Minimal Mode. The canvas comes back with its arrangement intact.

### Ctrl+Shift+S does nothing

The canvas owns the applets while it is on, so the applet panel can't be popped out.

1. Move the applet where you want it on the canvas, or turn off **View → Workspace Canvas → Enabled** to go back to the Classic applet panel.

## See also

- [Panadapter Controls](./panadapter-controls.md)
- [Menu Reference](./menu-reference.md)
- [Profile Management](./profile-management.md)
- [Keyboard Shortcuts](./keyboard-shortcuts.md)
