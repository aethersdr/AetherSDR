# GUI nested-loop lifetime audit (#5568)

Baseline: `c692c75eb9f31aac151e943b9f88e6298007c6c5`. This audit follows
parented stack `QMenu` / `QDialog` candidates, their derived dialogs and message
helpers, and the state used after their nested event loops. It extends #5567;
it does not claim that every reentrant signal in the application is safe.

## Ownership rule

A parented stack widget is unsafe when its parent can be destroyed while
`exec()` runs: QObject deletes the child, although its storage belongs to the
calling stack. A heap child held through `ScopedChildWidget` preserves scoped
cleanup and permits parent deletion. That alone does not protect the caller:
post-loop owner, model and child-widget accesses need survival/identity checks,
and table items must be copied or resolved again.

`MainWindow::showOrRaisePersistent()` sets `WA_DeleteOnClose` before showing
utility dialogs. Closing one during a child loop is therefore a real deletion
route. In contrast, the main window's destructor runs after the outer
application event loop returns; it is not evidence of deletion during a menu.
Workspace removal first evicts ordinary applet containers, and floating
container close does not delete them. `ContainerManager::destroyContainer()`
has no production caller outside its own recursive implementation at this
baseline. Those facts distinguish ordinary applets from a transient parented
directly to a workspace window.

## Direct stack-menu/dialog inventory

The following baseline inventory comes from `git grep` for explicit parented
`QMenu` and `QDialog` local declarations under `src/gui`. Line numbers refer to
the baseline, so they remain reproducible after this patch.

| Baseline site | Disposition and reason |
| --- | --- |
| `AppletPanel.cpp:1208` | Retained: ordinary applet ownership and workspace eviction keep the parent alive. |
| `Ax25HfPacketDecodeDialog.cpp:4263` | Retained: persistent hide-on-close dialog; it is not delete-on-close. |
| `ClientChainWidget.cpp:688` | Retained: chain/editor is retained by its strip; ordinary strip close hides it. |
| `ClientEqEditorCanvas.cpp:229` | Retained: EQ editor belongs to the retained chain. |
| `ConnectionPanel.cpp:1915` | Menu retained: panel belongs to MainWindow. Discovery can replace list items while the menu/nickname prompt runs; resolve the row again by family and serial. |
| `CopyAssistController.cpp:1008` | Retained: settings window is retained by the ordinary applet/controller, with no close-time deletion route found. |
| `CrossNeedleMeterApplet.cpp:174` | Retained: ordinary applet ownership. |
| `CwxPanel.cpp:773` | Menu retained: panel belongs to MainWindow. Copy the bubble text before the loop because history clearing can delete the bubble. |
| `DxClusterDialog.cpp:2515` | Scoped menu: Spot Hub is delete-on-close. |
| `LpMeterApplet.cpp:386` | Retained: ordinary applet ownership. |
| `MainWindow.cpp:4538` | Retained: MainWindow survives this dialog loop. |
| `MidiMappingDialog.cpp:678` | Scoped manual-binding dialog and post-loop owner/manager/child checks. MIDI mapping is delete-on-close. |
| `MiniPanApplet.cpp:83` | Retained: ordinary applet ownership. |
| `NetSchedulerDialog.cpp:173` | Scoped editor, copied capture callback and stable schedule IDs across loops. Double-click opening is deferred until the table event returns. Scheduler is delete-on-close. |
| `NetworkDiagnosticsDialog.cpp:1387` | Scoped menu and persistent selected-row indexes. Diagnostics is delete-on-close and the log model can change. |
| `RxApplet.cpp:454` | Scoped TX-antenna menu with original-slice survival/identity check. Ordinary owner deletion is not claimed as a production route; slice rebinding is the stale-intent hazard. |
| `RxApplet.cpp:1064` | AGC menu retained: ordinary applet ownership; the callback uses current calibration state. |
| `RxApplet.cpp:3235` | Scoped custom-filter dialog/menu and original slice/preset-button checks. Actions are connected with the preset button as context so a rebuilt preset cannot retain a callback. The menu itself was **parentless** (`QMenu menu;`) at the baseline, so it was never the invalid-free case #5568 warns against conflating by grep; it is parented here only to give the nested dialog scoped cleanup. Re-parenting adds no styling: `RxApplet`'s sole widget-level stylesheet is `QSlider::sub-page` (`RxApplet.cpp:306`), which a `QMenu` cannot match. |
| `SettingsBrowserDialog.cpp:666,799` | Scoped document viewer and Add Key; browser is delete-on-close. Double-click opening is deferred one event turn so Qt's table handler finishes before its table can be deleted. |
| `StripChainWidget.cpp:688` | Retained: retained strip owns the chain. |
| `StripRxChainWidget.cpp:593` | Retained: retained strip owns the chain. |
| `TciApplet.cpp:157` | Retained: ordinary applet ownership. |
| `ThemeEditorDialog.cpp:1035` | Scoped menu, owner/theme identity check and row relookup; preserves selection and scroll position. Theme Editor is delete-on-close. |
| `TxApplet.cpp:808,852` | Menus retained: ordinary applet ownership. |

## Derived dialogs and helpers

Qt 6.11.2's [static message-box implementation](https://github.com/qt/qtbase/blob/v6.11.2/src/widgets/dialogs/qmessagebox.cpp)
creates a stack `QMessageBox` inside `showNewMessageBox`. Its parent lifetime
therefore matters even when our call site only says `QMessageBox::warning`.
The [static input-dialog implementation](https://github.com/qt/qtbase/blob/v6.11.2/src/widgets/dialogs/qinputdialog.cpp)
uses a guarded heap allocation; it still leaves the caller responsible for
post-loop accesses. File/color pickers likewise require caller survival checks.

| Site / owner | Disposition |
| --- | --- |
| Shared `FramelessMessageBox::showMessage` | Scoped allocation; callers receive cancellation when the parent destroys the box. |
| Settings Browser confirmations/export | Owner checks after confirmation/picker; the Add Key child must also survive a second confirmation. |
| MIDI import/export | Scoped file pickers/messages and owner, manager and editor survival checks; snapshot exported bindings before the picker. |
| Net Scheduler import/export/remove | Scoped messages, owner checks after pickers, and ID relookup before changing a schedule. |
| Radio Setup TX acknowledgment / Kiwi picker / CSV transfer | Scoped exact dialog constructors and owner/child/manager guards. Closing the acknowledgment does not persist TX permission. |
| Radio Setup reboot / firmware browse+upload / recording directory / slice colour / forget-all SmartLink certificates | Scoped boxes and owner, model and child guards. `RadioSetupDialog` is shown through `showOrRaisePersistent()` (`MainWindow.cpp:3395`), so it is delete-on-close and every one of these resumed into `this` or a child. The three `QMessageBox::` statics were the stack-allocated `showNewMessageBox` case, not merely stale pointers. Reboot, firmware upload and cert clearing keep `Cancel`/`No` as the default. |
| Radio Setup automation-bridge bind failure | Retained: nothing runs after the notice. |
| Waveforms notices/removal / file selection | Scoped `PersistentDialog` helpers and caller survival checks. |
| Memory import/export/removal | Scoped notices and confirmations, owner/model checks and guarded asynchronous completion notices. |
| Profile Manager / Profile Import-Export | Scoped notices/confirmations and owner/model/transfer/picker checks. |
| BNR license in `AetherDspWidget` | Scoped confirmation; DSP window is delete-on-close. Return false if it disappears. |
| Theme Editor operations / token-editor notice | Scoped messages and post-picker/input survival checks; preserve confirmation defaults. |
| Spot Hub color/file selection / FreeDV notice | Caller and child survival checks and scoped message; Spot Hub is delete-on-close. |
| Network Diagnostics save log | Check owner/table after the file picker. |
| AGC calibration NR notice | Scoped warning; calibration window is delete-on-close. |
| ATU pre-tune warnings | Scoped boxes and owner checks, preserving Cancel as default. `TxApplet::openPreTuneDialog` explicitly sets delete-on-close. |
| Ulanzi mapper missing-polkit notice | Scoped warning; mapper is delete-on-close. This path is Linux-only. |
| TX clear-memory confirmation | Scoped box and model identity check. Its parent is `this->window()`: workspace deletion can delete the box even though the applet container is evicted intact. |
| Spectrum background-image picker | Scoped QFileDialog and surviving-spectrum check. The picker can belong directly to a removable workspace or floating spectrum window. |
| MainWindow-owned Shortcut / SWR license / support helpers | Retained: no nested-loop MainWindow destruction route. |
| AX.25, ordinary amplifier applet, Aetherial strip messages | Retained: hide/evict-and-keep owner lifetime. |
| Legacy `GradientEditorDialog` | No production construction found; shared `GradientStrip` is used by TokenEditorWidget. No speculative conversion. |

## Verification boundaries

Socket-free tests exercise real Settings Browser Add Key, document viewer and
shared warning paths, with the production delete-on-close flag and a timer that
closes the owner while the child runs. The original source fails with an ASan
bad-free in `QObjectPrivate::deleteChildren`; the fixed GUI sources pass.
AddressSanitizer instruments the test, SettingsBrowserDialog and
FramelessMessageBox; Qt and the linked engine library are not instrumented.

The Rx component test separately injects owner destruction and slice rebinding;
it does not claim a production applet-deletion route. Paired accepted-dialog
cases require one write to the live original slice and zero writes after rebind.
The scoped-widget test checks parent deletion and forwarded constructor values
against the direct Qt constructor under the same parent, including
platform-specific title behavior. The shared-warning case additionally asserts
that a box whose parent dies mid-loop reports `NoButton` rather than a pressed
button: `SettingsBrowserDialog::addKey()` and `deleteSelected()` only write to
the store on `Yes`, so that return value is the barrier against a post-teardown
write, not a detail. Net Scheduler Add and actual table double-click Edit both close the delete-on-close parent while the editor is active.

Native macOS verification used a fresh `AETHER_SETTINGS_DIR`,
`AETHER_AUTOMATION_NO_TX=1` and the explicit `connect local serial DEMO-0001`
selector. For both Settings Browser / Add Key and Net Scheduler / Add Net,
`invoke` opened the child, `grab` captured it, and `close` targeted the parent
while the child loop was active. A subsequent `dumpTree` contained neither
parent nor child, `ping` remained successful, and the radio snapshot still
reported Demo with `transmitting=false`. Closing MainWindow then produced an
actual process exit status of **0**; the app lock was released.

The full macOS build passed with RADE enabled, ARM64 host/system/executable,
and no RNNoise x86 sources. Nine focused CTests passed: settings browser,
scoped widget, GUI nested lifetime, RX squelch reconciliation, TX power
reconciliation, theme manager, theme seed, MIDI settings and ATU pre-tune
centers. The GUI ASan suite passed all eight cases; removing the custom-filter
slice/button guard in a scratch copy failed the rebound case with a stale
write, while the live acceptance control still passed. No new socket test or
per-PR CI gate was added. These samples do not claim native coverage of every
dialog, a full ASan engine build, or the Linux-only polkit notice.
