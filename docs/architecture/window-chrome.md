# Window chrome and the unified title bar

How AetherSDR's main window gets its 52 px title bar on each platform, which
parts Qt owns and which are ours, and how the radio switcher in that bar
behaves. Read this before touching `src/gui/WindowChrome.h`, `TitleBar`,
`RadioTabBar`, `WindowCaptionButtons` or `src/gui/mac/NativeWindowTitle.mm`.
Design rationale and the alternatives considered are in RFC
[#4764](https://github.com/aethersdr/AetherSDR/issues/4764).

## One shared bar, Qt-owned frames

`src/gui/WindowChrome.h` chooses the Qt window policy. Cocoa and Windows use
`Qt::ExpandedClientAreaHint` with `Qt::NoTitleBarBackgroundHint`, without
`Qt::FramelessWindowHint`. These flags arrived in Qt 6.9:
[Qt window flags](https://doc.qt.io/qt-6/qt.html#WindowType-enum) and
[expanded client areas](https://www.qt.io/blog/expanded-client-areas-and-safe-areas-in-qt-6.9).
In qtbase 6.12 only the cocoa and windows platform plugins implement them,
which is why `supportsExpandedClientArea()` keys off the platform name.

| Platform | Window mode | Window controls |
| --- | --- | --- |
| macOS (cocoa) | Expanded client area, no title-bar background | Native traffic lights (tiling menu, Stage Manager, native fullscreen) |
| Windows | Expanded client area, no title-bar background | Shared painted caption chips (`WindowCaptionButtons`) |
| Linux (xcb / Wayland) | `FramelessWindowHint` fallback | Shared painted caption chips (`WindowCaptionButtons`) |

`View → Frameless Window` turns the whole policy off on every platform and
returns the window to system decorations.

`TitleBar` draws the same 52-logical-pixel content everywhere. It opts out of
QWidget's automatic top-level safe-area margin and instead reserves horizontal
control gutters from `QWindow::safeAreaMargins()` and, on macOS, the measured
caption bounds (`WindowChrome::contentInsets`). Safe-area changes re-run the
layout. A fullscreen safe-area top inset can make the reserved height exceed
52 px. On macOS the buttons are measured in Qt content-view coordinates,
leaving exactly one 16 px gap before the brand unless a larger safe-area inset
is required.

Where the platform has no native menu bar (Windows, and Linux without a global
menu), the menus live behind a hamburger button that leads the bar, ahead of
the brand. `TitleBar::setMenuBar()` keeps the `QMenuBar` as a hidden child:
`MainWindow` still builds menus into it, the automation bridge still resolves
actions through it, and the button mirrors menus added later. The button opens
the menu with `popup()`, never a button-owned menu's `exec()` and its nested
event loop. Menu shortcuts survive the hidden bar because the visible button
carries the menu's action, and each menu's Alt+<letter> mnemonic is
re-registered on the bar to pop that menu under the button (the hidden bar's
own never match). A native menu bar (macOS) is left where the platform shows
it.

- **macOS:** Qt keeps the real `NSWindow`, native controls, corners, shadow
  and window-state behaviour. `mac/NativeWindowTitle.mm` sets
  `NSWindow.titleVisibility` and installs an empty unified `NSToolbar` so
  AppKit centres its own traffic lights in the 52 px region. It measures the
  buttons but never moves or reparents them, and implements no frame,
  move/resize, masking, blur or tiling. The toolbar is removed and the prior
  toolbar style restored when expanded chrome is disabled; a toolbar it did not
  install is left alone. Every native access is guarded by the cocoa platform
  name and an existing native view, so offscreen tests never reach AppKit. The
  window title itself is kept (Window menu and accessibility need it); Qt's
  background flag alone does not hide the title text.
- **Windows:** Qt owns the expanded frame, DWM integration (shadow, rounded
  corners) and the resize borders; the bar draws its own caption buttons.
  Qt 6.12 paints its Windows caption buttons and title into a layered child
  window that does not reliably appear, can show stale title text after a
  re-create, and takes its glyph colour from the OS light/dark mode rather than
  the theme. `WindowChrome::chromeFlags()` therefore keeps
  `CustomizeWindowHint` but drops the title and caption-button hints, so Qt
  neither draws nor hit-tests its own; `MainWindow::applyWindowsCaptionStyles()`
  puts `WS_MINIMIZEBOX`/`WS_MAXIMIZEBOX` back on the HWND on every show, since
  taskbar minimize, Win+Up and snap-to-maximize need them. **The Snap Layouts
  hover flyout does not appear** (nothing returns `HTMAXBUTTON` on hover);
  that is tracked in #6224.
- **Linux:** Qt's desktop Linux backends do not advertise expanded client
  areas. The fallback uses the shared caption cluster and `FramelessResizer`
  (6 px edge band; no top-edge resize under the bar, #4886). Title dragging
  calls `QWindow::startSystemMove()`, except on xcb, where the WM grab is
  silently dropped under Mutter/XWayland (#4827) and the bar moves the window
  itself. Disable
  Frameless Window to use compositor decorations. Compositor decoration-
  preference negotiation is not implemented. Wayland/X11 snap and
  fractional-scale resize are native test items.

The previous custom Windows `nativeEvent`/DWM frame and the macOS corner and
shadow shim are gone. The window is **opaque** (`WA_TranslucentBackground`
off): cheaper to composite, and it avoids the earlier disappearing header and
status-bar regressions. There is no system-blur promise and no custom corner
radius.

### Painting the bar

The bar paints its own fill and 1 px bottom hairline from
`color.titlebar.background` / `color.titlebar.border`, pre-composited over
`color.background.app` so the bar can be `WA_OpaquePaintEvent` — a tab's
heartbeat repaint then stops at the bar instead of repainting the window under
it. Two traps, both covered by `unified_title_bar_test`:

- A `TitleBar { … }` stylesheet rule never matches: the class is namespaced,
  and a bare QWidget subclass also needs `WA_StyledBackground`. Paint it.
- MainWindow carries a window-wide `QWidget { background-color }` rule, which
  gives every **plain** `QWidget` container a styled background in the window
  colour. The bar's own containers (drag gutter, audio cluster, tab viewport)
  are made transparent **by object name**, not by type — the discovery popover
  and the bar's menus are descendants and must keep their panels.

Radio tabs are 36 px tall and sit 8 px clear of the bar's top and bottom
edges. The inset is load-bearing: Qt's expanded Windows frame keeps a top
resize border of about 8 px, so a taller tab would sit where a press starts a
window resize.

## Radio switcher behaviour

Each tab shows the radio's name on line one and `[model ·] status [· detail]`
on line two — the model only when a nickname hides it, the state always in
words (WCAG 1.4.1; the dot's colour is never the only carrier). The active
tab's dot is also the radio-link indicator: it swells once per discovery
heartbeat, turns amber while discovering and red after three missed beats
(blinking, or solid when the operator has blinking off). An operator
disconnect stops the miss timer and clears the alarm; an unexpected loss
raises it. While the alarm is up the tab says "link lost" in words too, and the
alarm stays on the radio that dropped (not on whichever tab comes first) until
a different session starts. A vertical mouse wheel scrolls an overflowing
strip.

The "+" panel has a bounded scrollable list, search by name/model/address/
status, active-radio-first ordering, readable status text, and one Actions menu
per row. Names and addresses are elided visually but kept in tooltips and
accessible names. Search and controls are keyboard reachable.

| Action | Behaviour |
| --- | --- |
| Select a radio | Opens Connect to Radio on that radio's LAN/SmartLink row; never connects or disconnects without confirmation. |
| Disconnect | Enabled only for this client's connected radio; uses the intentional-disconnect path. |
| Rename | Client-owned names use the per-radio identity store and a non-modal dialog (built by MainWindow; `ConnectionPanel` decides and applies). Radio-owned names open Radio Setup while connected and are disabled while disconnected. The demo radio's "not on the air" name is a safety label and cannot be renamed. An empty nickname resets it. |
| Radio setup | Enabled only for this client's connected radio. |
| Remove from tabs | Available only while not connected here. Hides the tab; does not erase credentials, operating state, or the radio. |
| Add to tabs | Restores a hidden tab. Hidden radios stay in the "+" list; selecting one also restores it. |
| Connect manually | Opens the IP connection page. |
| Rescan radios | Requests discovery without connecting; list changes preserve the current search. |

Tab visibility is client UI state in `AppSettings["RadioSwitcher"].hiddenRadios`.
Disconnect, rename and removal are revalidated against the current session when
executed. This is **not** a destructive Forget Radio operation: discovery keeps
seeing the radio, and deleting saved endpoints or credentials needs its own
explicit workflow.

## Testing

`unified_title_bar_test` (headless) covers bar geometry, the painted fill and
border under MainWindow's real stylesheet cascade, tab insets, status text,
overflow and wheel scrolling, hidden-tab link carrier, link-alarm raise/clear,
minimal mode, search, action enablement/dispatch, keyboard-only focus rings and
light-theme panel colours. These are QWidget checks, not native frame
certification.

Drive the live bar with the [automation bridge](../automation-bridge.md#titlebar)
(`get titlebar`, `titlebar`, `applet`) on an isolated settings profile and the
built-in demo radio. Native items need real hardware and real pointer input:
macOS traffic-light hover/tiling and pointer hits across the top of the bar
under the unified toolbar; Windows Snap Layouts, Aero Snap and top-edge drift
(#4557); Linux compositor behaviour and fractional-scale drag/resize.
