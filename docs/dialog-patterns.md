# AetherSDR Dialog Patterns

When you add a new dialog to AetherSDR — or convert an existing modal one to
non-modal — there's a canonical pattern that the project's existing dialogs
follow. This doc captures it in one place so contributors (human or agent)
don't have to reverse-engineer it from 8+ existing dialogs.

Long-term, the [`PersistentDialog` base class issue (#2605)](https://github.com/ten9876/AetherSDR/issues/2605)
will collapse this boilerplate into a parent class with `bodyWidget()`. Until
then, every new dialog needs to wire these pieces by hand.

## The four concerns every persistent dialog handles

A persistent dialog in AetherSDR is **non-modal**, **lazy-constructed**,
**geometry-persistent**, and **frameless-chrome-aware**. Skip any of the four
and the dialog will misbehave in a predictable way.

| Concern | Why | Symptom if skipped |
|---|---|---|
| Non-modal + lazy-construct | Operator should keep the dialog open while interacting with the radio | Modal dialog blocks tuning |
| `WA_DeleteOnClose` + `QPointer` slot | Dialog instance ownership; survive close + re-open without leaks | Stack-allocated dialog leaks on each open; or stale pointer crash |
| Geometry persistence (base64) | Restore last-known position on next launch | Dialog opens at default OS position every time |
| Frameless chrome integration | Respect the global `FramelessWindow` setting | Double-title-bar visual (native + custom), or wrong chrome on toggle |

## The canonical pattern

### 1. Header — derive from `QDialog`, declare the slot fields

```cpp
class FooDialog : public QDialog {
    Q_OBJECT
public:
    explicit FooDialog(...args..., QWidget* parent = nullptr);
    void setFramelessMode(bool on);

protected:
    void closeEvent(QCloseEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void saveGeometryToSettings();
    void restoreGeometryFromSettings();

    // ... actual dialog state members ...

    QWidget*     m_titleBar{nullptr};
    QVBoxLayout* m_bodyLayout{nullptr};
    bool         m_restoringGeometry{false};
};
```

Forward-declare `class QCloseEvent;`, `class QMoveEvent;`, `class QResizeEvent;`,
`class QVBoxLayout;` in the header rather than `#include`ing the Qt headers.
Include the events in the `.cpp` where they're used.

### 2. Constructor — embed the frameless title bar, restore geometry

```cpp
FooDialog::FooDialog(...args..., QWidget* parent)
    : QDialog(parent), ...
{
    setWindowTitle("Foo");
    setMinimumSize(WIDTH, HEIGHT);
    setStyleSheet(kDialogStyle);

    // Outer layout wraps the frameless title bar + body widget.  Body
    // widget contains the actual dialog content.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_titleBar = new FramelessWindowTitleBar(QStringLiteral("Foo"), this);
    outer->addWidget(m_titleBar);

    auto* bodyWidget = new QWidget(this);
    auto* root = new QVBoxLayout(bodyWidget);
    root->setContentsMargins(9, 9, 9, 9);
    root->setSpacing(9);
    m_bodyLayout = root;
    outer->addWidget(bodyWidget, 1);

    // ... build dialog content inside `root` ...

    // Frameless chrome support — must be wired up before first show()
    FramelessResizer::install(this);
    setFramelessMode(
        AppSettings::instance().value("FramelessWindow", "True").toString() == "True");

    // Restore geometry from last session (after content built so initial
    // size is sensible for first-launch users).
    restoreGeometryFromSettings();
}
```

### 3. `setFramelessMode` — toggle the frameless chrome

This is the same shape for every dialog, only the title and key change:

```cpp
void FooDialog::setFramelessMode(bool on)
{
    const QRect geom = geometry();
    const bool wasVisible = isVisible();

    Qt::WindowFlags flags = (windowFlags() & ~Qt::WindowType_Mask) | Qt::Dialog;
    flags.setFlag(Qt::FramelessWindowHint, on);
    setWindowFlags(flags);
    // Restore geometry ONLY when the dialog was already visible (runtime
    // toggle).  On first open (wasVisible=false), let Qt place normally.
    if (wasVisible) setGeometry(geom);

    if (m_titleBar) m_titleBar->setVisible(on);
    if (m_bodyLayout) m_bodyLayout->setContentsMargins(9, on ? 7 : 9, 9, 9);
    if (wasVisible) show();
}
```

The `if (wasVisible) setGeometry(geom)` guard is critical on macOS — without
it, first-open dialogs land at the top-left corner because the geometry
captured during construction is the pre-show default position. See PR #2580.

### 4. Geometry persistence — base64 + move/resize/close

`AppSettings` serializes via XML, so binary `QByteArray` values from
`saveGeometry()` need base64 encoding to round-trip. The project's existing
`MainWindowGeometry` / `MainWindowState` use this same pattern.

```cpp
void FooDialog::saveGeometryToSettings()
{
    auto& s = AppSettings::instance();
    s.setValue("FooDialogGeometry", saveGeometry().toBase64());
}

void FooDialog::restoreGeometryFromSettings()
{
    const QString geomB64 = AppSettings::instance()
        .value("FooDialogGeometry").toString();
    if (geomB64.isEmpty()) return;
    m_restoringGeometry = true;
    restoreGeometry(QByteArray::fromBase64(geomB64.toLatin1()));
    m_restoringGeometry = false;
}

void FooDialog::closeEvent(QCloseEvent* event)
{
    saveGeometryToSettings();
    AppSettings::instance().save();   // flush to disk on close
    QDialog::closeEvent(event);
}

void FooDialog::moveEvent(QMoveEvent* event)
{
    QDialog::moveEvent(event);
    if (!m_restoringGeometry) saveGeometryToSettings();
}

void FooDialog::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    if (!m_restoringGeometry) saveGeometryToSettings();
}
```

Two details worth understanding:

- **`m_restoringGeometry` guard.** `restoreGeometry()` triggers move/resize
  events as Qt repositions the window. Without the guard, those events would
  immediately call `saveGeometryToSettings()` and overwrite the freshly-loaded
  value with the dialog's current (default) position.
- **Save on move/resize is in-memory only; close-time save flushes to disk.**
  `AppSettings::setValue` updates the in-memory store; `save()` writes the
  XML file. The move/resize saves keep the in-memory store current so a
  crash or force-quit preserves the last-known position. The close-time
  `save()` is the canonical persistence point.

### 5. MainWindow wiring — lazy-construct + raise on re-invoke

In `MainWindow.cpp::buildMenuBar()`:

```cpp
auto* action = menu->addAction("Foo...");
connect(action, &QAction::triggered, this, [this] {
    if (!m_fooDialog) {
        auto* dlg = new FooDialog(...args..., this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setFramelessMode(framelessWindowEnabled());
        m_fooDialog = dlg;
    }
    m_fooDialog->show();
    m_fooDialog->raise();
    m_fooDialog->activateWindow();
});
```

In `MainWindow.h`:

```cpp
class FooDialog;   // forward-declare in namespace AetherSDR
...
QPointer<FooDialog> m_fooDialog;
```

The `QPointer` slot auto-nulls when `WA_DeleteOnClose` destroys the dialog.
Next menu click sees `m_fooDialog == nullptr` and constructs a fresh one
(which then restores geometry from AppSettings).

### 6. Runtime frameless toggle propagation

In `MainWindow.cpp::setFramelessWindow(bool on)`:

```cpp
if (m_fooDialog)
    m_fooDialog->setFramelessMode(on);
```

This propagates the View → Use Frameless Windows toggle to already-open
instances of the dialog. There's currently one line per dialog in
`setFramelessWindow()` — the `PersistentDialog` refactor in #2605 will
replace these with a single `QSet<QPointer<PersistentDialog>>` walk.

## Existing dialogs that follow this pattern (look at these for reference)

- `src/gui/NetworkDiagnosticsDialog.{h,cpp}` — most complete reference, has all the patterns
- `src/gui/MemoryDialog.{h,cpp}`
- `src/gui/AetherDspDialog.{h,cpp}`
- `src/gui/DxClusterDialog.{h,cpp}` (SpotHub)
- `src/gui/MultiFlexDialog.{h,cpp}`
- `src/gui/MidiMappingDialog.{h,cpp}`
- `src/gui/PanLayoutDialog.{h,cpp}`
- `src/gui/ProfileManagerDialog.{h,cpp}` — most recent example; has the move/resize event saving

## Common pitfalls

These are the issues that have hit real PRs in the project's history.
Listing them so they don't bite the next contributor.

| Pitfall | Wrong | Right | Surfaced in |
|---|---|---|---|
| AppSettings is a reference, not a pointer | `AppSettings::instance()->value(...)` | `AppSettings::instance().value(...)` | #2591 |
| AppSettings serializes XML, binary needs base64 | `setValue("Geom", saveGeometry())` | `setValue("Geom", saveGeometry().toBase64())` | #2591 |
| Include path uses `core/` prefix | `#include "AppSettings.h"` | `#include "core/AppSettings.h"` | #2591 |
| Restore at construction triggers move/resize | Save unconditionally on every move/resize | Guard with `m_restoringGeometry` | #2592 |
| First-open `setGeometry(geom)` on macOS captures pre-show position | `setGeometry(geom)` unconditionally | `if (wasVisible) setGeometry(geom)` | #2580 |
| QDialog::show() is non-modal by default | `setModal(false)` + `setWindowModality(Qt::NonModal)` (both redundant) | omit both | #2592 |
| Frameless chrome must be set up before first show | Skip `setFramelessMode(framelessWindowEnabled())` on construct | Call it in the ctor after `FramelessResizer::install(this)` | #2591, #2592 |
| Runtime toggle needs explicit propagation | Forget to wire into `MainWindow::setFramelessWindow()` | Add `if (m_fooDialog) m_fooDialog->setFramelessMode(on);` | #2591 |
| Include heavy Qt event headers in `.h` | `#include <QCloseEvent>` in the header | Forward-declare in `.h`, include in `.cpp` | #2591 |

## See also

- `CLAUDE.md` — project-wide AI agent guidelines including settings persistence
- `docs/architecture-pipelines.md` — broader project architecture overview
- Issue #2605 — `PersistentDialog` base class proposal that will eliminate this boilerplate
