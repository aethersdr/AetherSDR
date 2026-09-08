# Qt 6.12 chrome development branch

This is a development migration, not a release-toolchain change. CMake requires
Qt 6.12; the local macOS SDK is the official 6.12.0 preview package dated
2026-09-01. Windows/Linux CI and release packaging remain on their upstream
toolchains and cannot build this branch unchanged. Do not publish it as a
release-ready, cross-platform-verified migration.

## Scope after the applet-picker split

This branch contains the unified window chrome and **radio** switcher only.
The grouped **applet** picker, brighter category headings, and pinned S-Meter
alignment are developed independently on
[`aether/applet-picker`](https://github.com/jensenpat/AetherSDR/tree/aether/applet-picker),
based on upstream main without the Qt 6.12 requirement. The chrome branch
retains its existing applet buttons and pinned-meter layout.

The latest laptop package, `AetherSDR qt612-applet-picker-0de5a2b6-dirty 1748.app`,
was intentionally built with both changes before this split. Its 5/5 focused
test result and native picker checks are combined-build evidence, not a claim
that either standalone PR was rebuilt or independently certified. The combined
source is preserved locally on `aether/applet-picker-integration`; the earlier
chrome-only and caption-spacing evidence below remains attributed to its
original source/build identity.

That package passed a 125-Mach-O dependency audit, file-hash/symlink/signature
verification after SFTP, quarantine removal, and an isolated loader check on
the test Mac. It remains an ad-hoc-signed preview build with session-only
credentials; native tiling acceptance and Windows/Linux validation remain open.

## One shared bar, Qt-owned frames

`src/gui/WindowChrome.h` chooses the Qt window policy. Cocoa and Windows use
`Qt::ExpandedClientAreaHint` with `Qt::NoTitleBarBackgroundHint`, without
`Qt::FramelessWindowHint`. These flags arrived in Qt 6.9, not 6.12:
[Qt window flags](https://doc.qt.io/qt-6/qt.html#WindowType-enum) and
[expanded client areas](https://www.qt.io/blog/expanded-client-areas-and-safe-areas-in-qt-6.9).

`TitleBar` draws the same 52-logical-pixel content on every platform. It opts
out of QWidget's automatic top-level safe-area margin and instead reserves
horizontal control gutters using QWindow safe-area margins, measured macOS
caption bounds, and QStyle metrics on Windows.
Changes in native safe areas update the layout. Fullscreen safe-area top insets
can make the total reserved height greater than 52 pixels. Qt does not expose
the exact native caption-button rectangles through a portable public API;
the Windows gutter remains conservative. On macOS the existing presentation
adapter measures the native buttons in Qt content-view coordinates, leaving
exactly one 16-logical-pixel gap before the brand (unless a larger safe-area
inset is required). It never derives button width from title-bar height.

- **macOS:** Qt retains the real NSWindow, native controls, corners, shadow,
  and window-state behavior. `mac/NativeWindowTitle.mm` sets
  `NSWindow.titleVisibility` and installs an empty native unified toolbar so
  AppKit centers its own traffic lights in the 52-pixel title region. It
  measures the buttons but does not manually move or reparent them, implement
  a frame, move/resize, masking, blur, or tiling. The toolbar is removed and
  the prior toolbar style restored when expanded chrome is disabled; existing
  toolbars are left alone. Every native access is guarded by the Cocoa platform
  name and an existing native view, including offscreen tests.
  This small presentation adapter
  avoids clearing the actual window title (which would leave Window-menu and
  accessibility names blank). Qt's background flag alone does not hide text.
- **Windows:** Qt owns the expanded frame, DWM integration, caption drawing,
  and native hit testing. `CustomizeWindowHint` plus the explicit caption-button
  flags suppresses duplicate title text while retaining the window's title.
  Snap Layouts hover must be tested on Windows: the inspected Qt 6.12 backend
  returns `HTMAXBUTTON` conditionally on mouse-button state, so native-looking
  buttons are not proof that the hover menu works.
- **Linux:** expanded client areas are not advertised as supported by Qt's
  desktop Linux backends. Use the single shared fallback caption cluster and
  existing FramelessResizer; title dragging calls `QWindow::startSystemMove()`.
  Disable the existing Frameless Window option to use compositor decorations.
  Automatic compositor decoration-preference negotiation is not implemented
  here. Wayland/X11 snap and fractional-scale resize remain native test items.

The prior custom Windows native-event frame and macOS corner/shadow shim are
removed. No new chrome colors are introduced: background, border, hover,
caption, and active-tab accents resolve through the existing theme tokens.
The window remains opaque to avoid the previous disappearing header/status-bar
regressions. There is no new system-blur promise or custom corner radius.

## Radio switcher behavior

The + panel has a bounded scrollable list, search by name/address/status,
active-radio-first ordering, readable status text, and one Actions menu per row.
All rows and action menus use the same QWidget layout and theme tokens on all
platforms. Names/addresses are elided visually but retained in tooltips and
accessible names. Search and controls are keyboard reachable; arrow keys move
between controls and the native Qt menu supports keyboard action selection.

| Action | Behavior |
| --- | --- |
| Select a radio | Opens Connect to Radio on that radio's LAN/SmartLink row; never disconnects or connects without confirmation. |
| Disconnect | Enabled only for this client's connected radio; uses the existing intentional-disconnect path. |
| Rename | Client-owned names use the existing per-radio Identity store and a non-modal dialog. Radio-owned names open Radio Setup while connected; disconnected radio-owned naming is disabled. An empty client nickname resets it. |
| Radio setup | Enabled only for this client's connected radio. |
| Remove from tabs | Available only while not connected here. Hides the tab; does not erase credentials, operating state, or a physical/discovered radio. |
| Add to tabs | Restores a hidden tab. Hidden radios remain in the + list. Selecting a hidden radio also restores it. |
| Connect manually | Opens the IP connection page, not whichever picker page happened to be used last. |
| Rescan radios | Requests discovery without initiating a connection; list changes preserve the current search. |

Tab visibility is client UI state in `AppSettings["RadioSwitcher"].hiddenRadios`.
Disconnect, rename, and removal are revalidated against the current session
when executed. A status-dot color is never the only connection-state label.
This is **not** a destructive Forget Radio operation: discovery will continue
to see a radio on the network, and saved endpoint/credential deletion needs a
separately defined, explicit data-removal workflow.

## Local macOS SDK and build

Install the official Qt 6.12 preview SDK using Qt's installer, or extract its
verified macOS archives into a worktree-local SDK directory. The SDK must
include base, declarative, SVG, tools, translations, Multimedia, SerialPort,
ShaderTools, and WebSockets. This worktree uses `build/qt-sdk`; it does not
replace Homebrew Qt or another worktree's SDK.

Official repository metadata:
[Qt 6.12 macOS packages](https://download.qt.io/online/qtsdkrepository/mac_x64/desktop/qt6_6120/qt6_6120/Updates.xml).

```sh
cmake -S . -B build -U 'Qt6*_DIR' \
  -DCMAKE_PREFIX_PATH="$PWD/build/qt-sdk;/opt/homebrew" \
  -DQt6_DIR="$PWD/build/qt-sdk/lib/cmake/Qt6"
cmake --build build -j22
QT_QPA_PLATFORM=offscreen ctest --test-dir build -j22 --no-tests=error \
  -R '^(unified_title_bar_test|titlebar_headphone_mute_test|hl2_pc_audio_lock_test|connection_panel_size_test)$' \
  --output-on-failure
```

The local app is not a self-contained deployment bundle until packaged against
this SDK. QtKeychain must also be built for this SDK before credential
persistence can be claimed; an app compiled without it has session-only
credential support. Do not copy an unbundled app to another Mac as a test DMG.

## Validation boundary

The focused tests cover shared geometry, status text, tab overflow, hidden-tab
heartbeat selection, search, action enablement/dispatch, light-theme panel
colors, and existing title-bar audio behavior. The connection-panel test covers
its existing layout contract. These are headless QWidget checks, not native OS
frame certification.

Use the [automation bridge](automation-bridge.md#titlebar) with an isolated
settings profile and `AETHER_AUTOMATION_NO_TX=1` for select/action/readback,
rename, remove/restore, manual-page selection, and screenshots. Assert runtime
`get titlebar` reports Qt 6.12, 52 pixels, offset zero, and native captions on
macOS. Keep live-radio/TX testing outside this UI validation.

Before promotion: manually test native macOS traffic-light hover/tiling,
pointer hits across the entire bar, resize/fullscreen/minimal-mode round trips,
status-bar visibility, and dark/light appearance. Then migrate CI and run
Windows Snap Layouts/Aero Snap and Linux compositor/fractional-scaling tests.

### Local evidence, 2026-09-07

- Rebased onto upstream main `c3e1fe6e`; branch HEAD `bf53789b` plus the
  uncommitted migration described here.
- Qt 6.12.0 application and four focused test targets compiled successfully.
  The four CTest selections above passed against these changes, offscreen.
- Simulator-only bridge checks passed: search/empty-state, rescan, nickname
  save/reset, remove/restore, manual/LAN page selection, no implicit connection,
  explicit simulator connection, connected-action gating, and disconnect.
- A separate app restart preserved both the simulator nickname and hidden-tab
  preference. Test changes were restored and the isolated instance closed.
- Changed-file accessibility scan: zero findings. Engine-boundary strict
  scan: zero blocking findings; existing tracked legacy warnings remain.
- No live radio or TX validation, Windows/Linux CI, deployment packaging,
  or final native macOS hit-test/tiling certification was performed.

### Native caption-spacing follow-up, 2026-09-07

- Rebuilt at `bf53789b` with working-tree changes; all four focused offscreen
  tests passed. The unified-title-bar test also passed on native Cocoa,
  including the measured 16-pixel gap and 26-pixel traffic-light center.
- An isolated, disconnected Cocoa app reported native bounds `[19,19,60,14]`
  and brand bounds `[95,0,109,52]` through the bridge. The 16-pixel gap and
  shared vertical center survived resize, maximize/restore, and fullscreen
  entry/exit. Fullscreen reports empty caption bounds rather than reserving
  the windowed controls' gutter.
- Computer Use clicked the + button; bridge readback confirmed the popover
  opened, and a separate popover capture verified its contents. Computer Use's
  main-window capture omits this separate popup, so that capture alone cannot
  establish whether the click worked.
- No live radio connection, TX, remote redeployment, or Windows/Linux testing
  was performed for this follow-up. Native tiling-menu behavior still requires
  the laptop's manual acceptance check.
