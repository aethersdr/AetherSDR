#pragma once

#include "DragValuePopup.h"

#include <QSlider>
#include <QComboBox>
#include <QAbstractItemView>
#include <QLabel>
#include <QLineEdit>
#include <QIntValidator>
#include <QAccessible>
#include <QCoreApplication>
#include <QAccessibleWidget>
#include <QEvent>
#include <QLocale>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QWheelEvent>
#include <functional>
#include <utility>

// Global lock for sidebar controls — when locked, sliders, combo boxes,
// and scrollable labels ignore wheel/mouse events so the user can scroll
// the applet panel without accidentally changing values. (#745)
class ControlsLock {
public:
    static bool isLocked() { return s_locked; }
    static void setLocked(bool locked) { s_locked = locked; }
private:
    static inline bool s_locked = false;
};

// QSlider subclass that always consumes wheel events, even at min/max
// boundaries. Prevents scroll from propagating to parent widgets (e.g.
// SpectrumWidget tuning the VFO when a slider bottoms out). (#570)
// When controls are locked (#745), ignores wheel events and lets the
// parent scroll area handle them.
class GuardedSlider : public QSlider {
public:
    using DragValueFormatter = std::function<QString(int)>;

    explicit GuardedSlider(QWidget* parent = nullptr)
        : QSlider(parent)
    {
    }

    explicit GuardedSlider(Qt::Orientation orientation, QWidget* parent = nullptr)
        : QSlider(orientation, parent)
    {
    }

    void setDragValueFormatter(DragValueFormatter formatter) {
        m_dragValueFormatter = std::move(formatter);
    }

    void setDragValuePopupEnabled(bool enabled) {
        m_dragValuePopupEnabled = enabled;
        if (!enabled && m_dragValuePopup)
            m_dragValuePopup->hideNow();
    }

    // Flash the value badge in response to a keyboard step, then let it
    // linger and fade with the same timeout as a mouse release.  Keyboard
    // nudges for these sliders are routed through MainWindow's shortcut
    // lease (so global operating shortcuts can resume), so the lease handler
    // calls this to mirror the mouse-drag readout. (#3303 follow-up)
    void flashDragValue() {
        if (!m_dragValuePopupEnabled)
            return;
        showDragValuePopup(mapToGlobal(rect().center()));
        if (m_dragValuePopup)
            m_dragValuePopup->linger();
    }

    void mousePressEvent(QMouseEvent* ev) override {
        if (ControlsLock::isLocked()) {
            ev->ignore();
            return;
        }
        QSlider::mousePressEvent(ev);
        if (ev->button() == Qt::LeftButton) {
            m_dragValueActive = true;
            showDragValuePopup(ev->globalPosition().toPoint());
        }
    }
    void mouseMoveEvent(QMouseEvent* ev) override {
        if (ControlsLock::isLocked()) {
            ev->ignore();
            return;
        }
        QSlider::mouseMoveEvent(ev);
        if (m_dragValueActive || isSliderDown())
            showDragValuePopup(ev->globalPosition().toPoint());
    }
    void mouseReleaseEvent(QMouseEvent* ev) override {
        const bool wasActive = m_dragValueActive;
        QSlider::mouseReleaseEvent(ev);
        if (wasActive && ev->button() == Qt::LeftButton) {
            showDragValuePopup(ev->globalPosition().toPoint());
            m_dragValueActive = false;
            if (m_dragValuePopup)
                m_dragValuePopup->linger();
        }
    }
    void wheelEvent(QWheelEvent* ev) override {
        if (ControlsLock::isLocked()) {
            ev->ignore();
            return;
        }
        // Use singleStep (default 1) instead of pageStep (default 10) so
        // that mouse-wheel adjustments are fine-grained (#1026).
        int delta = ev->angleDelta().y();
        if (delta != 0)
            setValue(value() + (delta > 0 ? singleStep() : -singleStep()));
        ev->accept();
    }

protected:
    // Below is protected (not private) so subclasses that override the
    // mouse handlers for custom drag behaviour — e.g. WaterfallRateSlider's
    // click-to-jump positioning — can still drive the same drag-value popup
    // instead of silently losing it.
    QString dragValueText() const {
        if (m_dragValueFormatter)
            return m_dragValueFormatter(value());
        return QString::number(value());
    }

    QPoint dragValueAnchor(const QPoint& fallbackGlobal) const {
        QStyleOptionSlider opt;
        initStyleOption(&opt);
        const QRect handle = style()->subControlRect(
            QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
        if (handle.isValid())
            return mapToGlobal(handle.center());
        return fallbackGlobal;
    }

    void showDragValuePopup(const QPoint& fallbackGlobal) {
        if (!m_dragValuePopupEnabled)
            return;
        if (!m_dragValuePopup)
            m_dragValuePopup = new AetherSDR::DragValuePopup(this);
        m_dragValuePopup->showValue(dragValueAnchor(fallbackGlobal),
                                    dragValueText());
    }

    DragValueFormatter m_dragValueFormatter;
    AetherSDR::DragValuePopup* m_dragValuePopup{nullptr};
    bool m_dragValuePopupEnabled{true};
    bool m_dragValueActive{false};
};

// QComboBox subclass that only responds to wheel events when the dropdown
// popup is open. Prevents accidental value changes when scrolling the applet
// panel, but allows normal wheel scrolling through the list when the user
// has clicked to open the dropdown. (#570, #676)
// When controls are locked (#745), also blocks mouse press to prevent
// opening the dropdown.
class GuardedComboBox : public QComboBox {
public:
    using QComboBox::QComboBox;
    void wheelEvent(QWheelEvent* ev) override {
        if (ControlsLock::isLocked()) {
            ev->ignore();
            return;
        }
        if (view() && view()->isVisible())
            QComboBox::wheelEvent(ev);  // popup open — scroll the list
        else
            ev->ignore();  // popup closed — let parent handle scroll
    }
    void mousePressEvent(QMouseEvent* ev) override {
        if (ControlsLock::isLocked()) {
            ev->ignore();
            return;
        }
        QComboBox::mousePressEvent(ev);
    }
};

// The editor's validator, pinned to the C locale.
//
// QIntValidator validates in ITS locale while QString::toInt() always parses
// in the C locale, so by default the two disagree: the validator accepts
// locale digits and correctly-placed group separators ("3,000" under en_US,
// Arabic-Indic digits under ar_*) that toInt() then refuses, and the edit
// closes without ever reaching the model — an accepted keystroke that does
// nothing (#5064 review). Pinning the validator to C, with the group
// separator explicitly rejected rather than merely omitted, makes the two
// halves agree on exactly one grammar: optional sign, ASCII digits.
//
// There is deliberately no clamping fixup() here. QIntValidator calls an
// out-of-range number Intermediate, and QLineEdit will not emit
// returnPressed/editingFinished on Intermediate input — the earlier version
// of this class clamped in fixup() to keep Enter from being a dead keypress.
// Return is now handled explicitly on the editor (see eventFilter below), so
// an out-of-range entry closes the editor and RESTORES the previous value,
// which is what issue #3627 asks for.
inline QIntValidator* makeCLocaleIntValidator(int minimum, int maximum, QObject* parent)
{
    auto* validator = new QIntValidator(minimum, maximum, parent);
    QLocale cLocale = QLocale::c();
    cLocale.setNumberOptions(QLocale::RejectGroupSeparator
                             | QLocale::OmitGroupSeparator);
    validator->setLocale(cLocale);
    return validator;
}

class ScrollableLabel;
// Installed lazily by setEditable() — see the class comment below.
inline QAccessibleInterface* scrollableLabelAccessibleFactory(const QString& key,
                                                              QObject* object);

// QLabel subclass that emits scrolled(int steps) on wheel events and
// always consumes them. Used for RIT/XIT/pitch numeric displays. (#619)
// When controls are locked (#745), ignores wheel events.
//
// The label can also be typed into (#3627): call setEditable() with the
// accepted range and a double-click swaps a QLineEdit over the label.
// Editing is OFF by default, so every existing user — RIT/XIT, step size,
// RTTY mark/shift — keeps its display-only behaviour unchanged.
//
// A committed value is only ever a REQUEST. The owner clamps it against
// whatever relationship its own controls carry (e.g. TX low <= high - 50)
// and then re-syncs the text from the model, so the label can never show a
// number the model did not accept.
class ScrollableLabel : public QLabel {
    Q_OBJECT
public:
    using QLabel::QLabel;

    // Enable double-click-to-type. min/max bound the editor's validator;
    // the owner still applies any cross-control clamping on commit.
    void setEditable(int minValue, int maxValue) {
        m_editable = true;
        m_min = minValue;
        m_max = maxValue;

        // docs/a11y.md, "Interactive QLabel anti-pattern": a QLabel that opens
        // an editor is an interactive control, so it needs Tab focus, a
        // keyboard activation path (keyPressEvent below) and a role that is
        // not StaticText. Without these the feature was reachable only by
        // mouse double-click — which is the opposite of the accessibility
        // motivation in issue #3627 (#5064 review).
        //
        // StrongFocus rather than TabFocus: the label already takes a click,
        // and a control that focuses on Tab but not on click reads as broken.
        setFocusPolicy(Qt::StrongFocus);
        if (accessibleDescription().isEmpty()) {
            setAccessibleDescription(
                tr("Editable. Press Enter to type an exact value in Hz "
                   "between %1 and %2, or use the arrow buttons.")
                    .arg(minValue).arg(maxValue));
        }

        static bool s_accessibilityFactoryInstalled = false;
        if (!s_accessibilityFactoryInstalled) {
            s_accessibilityFactoryInstalled = true;
            QAccessible::installFactory(scrollableLabelAccessibleFactory);
        }
    }
    bool isEditable() const { return m_editable; }
    int  editMinimum() const { return m_min; }
    int  editMaximum() const { return m_max; }

    // The owner styles the editor, so it does not flash an unthemed system box
    // over a dark panel. A callback rather than a stylesheet string for two
    // reasons: the owner writes a QLineEdit rule directly (a QLabel rule would
    // not match a QLineEdit at all, since Qt selects on widget class), and the
    // stylesheet is applied by ThemeManager itself, which owns theming
    // and is where the colour audit expects it to live.
    void setEditorStyler(std::function<void(QWidget*)> styler) {
        m_editorStyler = std::move(styler);
    }
    bool hasEditorStyler() const { return static_cast<bool>(m_editorStyler); }

    void wheelEvent(QWheelEvent* ev) override {
        if (ControlsLock::isLocked()) {
            ev->ignore();
            return;
        }
        int delta = ev->angleDelta().y();
        if (delta > 0) emit scrolled(1);
        else if (delta < 0) emit scrolled(-1);
        ev->accept();
    }

    // Keyboard route into the editor. Same gates as the double-click: editing
    // must be enabled and the panel unlocked. Anything else falls through to
    // QLabel so Tab/Shift-Tab keep working.
    void keyPressEvent(QKeyEvent* ev) override {
        const bool activates = ev->key() == Qt::Key_Return
                               || ev->key() == Qt::Key_Enter;
        if (activates && m_editable && !m_editor && !ControlsLock::isLocked()) {
            beginEdit(Qt::TabFocusReason);
            ev->accept();
            return;
        }
        QLabel::keyPressEvent(ev);
    }

    void mouseDoubleClickEvent(QMouseEvent* ev) override {
        // Same gate as the wheel handler: a locked panel is being scrolled,
        // not operated. (#745)
        if (!m_editable || ControlsLock::isLocked()) {
            QLabel::mouseDoubleClickEvent(ev);
            return;
        }
        beginEdit();
        ev->accept();
    }

    // Exposed so a test (and any future keyboard route) can open the editor
    // without synthesising a double-click.
    void beginEdit(Qt::FocusReason reason = Qt::MouseFocusReason) {
        if (!m_editable || m_editor)
            return;
        m_editor = new QLineEdit(text(), this);
        m_editor->setValidator(makeCLocaleIntValidator(m_min, m_max, m_editor));
        // The editor is a fresh widget every time, so it inherits none of the
        // label's accessible metadata — to a screen reader it was an unnamed
        // edit box appearing from nowhere (#5064 review).
        m_editor->setAccessibleName(accessibleName());
        m_editor->setAccessibleDescription(
            tr("Type a value in Hz between %1 and %2, then press Enter. "
               "Escape cancels.").arg(m_min).arg(m_max));
        m_editor->setAlignment(alignment());
        if (m_editorStyler)
            m_editorStyler(m_editor);
        m_editor->setGeometry(rect());
        m_editor->selectAll();
        // Escape has to be claimed on the EDITOR, via ShortcutOverride — see
        // eventFilter() below for why a keyPressEvent override cannot work.
        m_editor->installEventFilter(this);
        connect(m_editor, &QLineEdit::editingFinished, this, [this]() { commitEdit(); });
        m_editor->show();
        m_editor->setFocus(reason);
    }

    bool isEditing() const { return m_editor != nullptr; }

    // Whether closing the editor should hand focus back to the label. Yes for
    // deliberate keyboard exits (Enter, Escape); No for focus-out, where the
    // user is already on their way to another control.
    enum class RestoreFocus { No, Yes };

signals:
    void scrolled(int direction);
    // A typed value the owner should clamp and push at the model.
    void editCommitted(int value);

protected:
    // Cancel the edit on Esc.
    //
    // This MUST filter the editor and MUST answer ShortcutOverride, not just
    // KeyPress. The main window carries `QShortcut(Qt::Key_Escape, window())`
    // (CwxPanel, DvkPanel), and Qt dispatches a shortcut by first offering a
    // ShortcutOverride event: if nothing accepts it, the shortcut fires and
    // the focused widget is never sent a KeyPress at all. A keyPressEvent
    // override on this label therefore never runs, which is exactly how this
    // shipped broken — Esc did nothing in the app while passing a unit test
    // that delivered the key straight to the widget with no shortcut present.
    //
    // Same shape as VfoWidget::eventFilter for its frequency direct-entry.
    bool eventFilter(QObject* obj, QEvent* ev) override {
        if (obj == m_editor) {
            if (ev->type() == QEvent::ShortcutOverride
                || ev->type() == QEvent::KeyPress) {
                auto* ke = static_cast<QKeyEvent*>(ev);
                if (ke->key() == Qt::Key_Escape || ke->key() == Qt::Key_Cancel) {
                    // Accepting the ShortcutOverride claims the key so the
                    // window shortcut does not consume it; the KeyPress that
                    // follows is what actually closes the editor.
                    if (ev->type() == QEvent::KeyPress)
                        closeEditor(RestoreFocus::Yes);
                    ev->accept();
                    return true;
                }

                // Return/Enter must be handled HERE rather than left to
                // QLineEdit's own signal (#5064 review). QLineEdit emits
                // returnPressed/editingFinished on Return only once the
                // validator reports Acceptable — an EMPTY field is
                // Intermediate and nothing repairs it, so Enter emitted
                // nothing at all and left the editor sitting open on top of
                // the label, eating every wheel event aimed at the numerals
                // underneath. The same held for any out-of-range entry once
                // the clamping fixup() was removed.
                //
                // Committing explicitly makes Enter terminal for EVERY input:
                // a legal value commits, anything else cancels and the
                // previous value stands.
                if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
                    if (ev->type() == QEvent::KeyPress)
                        commitEdit(RestoreFocus::Yes);
                    ev->accept();
                    return true;
                }
            }

            // Focus-out must ALWAYS end the edit.  QLineEdit emits
            // editingFinished on focus-out only when the input is Acceptable,
            // so a half-typed or out-of-range value emitted nothing and left
            // the editor sitting open on top of the label — where it silently
            // ate every wheel event aimed at the numerals underneath.
            if (ev->type() == QEvent::FocusOut) {
                // RestoreFocus::No — the user is on their way somewhere else
                // (a Tab, a click on another control); pulling focus back to
                // the label here would fight them for it.
                commitEdit(RestoreFocus::No);
                return false;               // let Qt finish its own focus work
            }

            // The editor covers the label exactly, so while it is open the
            // label can never see a wheel event of its own.  Forward it, so
            // rolling steps the value whether or not a field is open, and
            // re-seed the text from whatever the model accepted.
            if (ev->type() == QEvent::Wheel) {
                if (ControlsLock::isLocked())
                    return false;
                auto* we = static_cast<QWheelEvent*>(ev);
                const int delta = we->angleDelta().y();
                if (delta != 0) {
                    emit scrolled(delta > 0 ? 1 : -1);
                    if (m_editor)
                        m_editor->setText(text());   // text() is model truth
                }
                ev->accept();
                return true;
            }
        }
        return QLabel::eventFilter(obj, ev);
    }

private:
    // Tear the editor down. Clears the member FIRST: destroying a focused
    // QLineEdit emits editingFinished, and a null member is what stops that
    // re-entering commitEdit() and committing a value twice.
    void closeEditor(RestoreFocus restore = RestoreFocus::No) {
        if (!m_editor)
            return;
        QLineEdit* ed = m_editor;
        m_editor = nullptr;
        // hide() before deleteLater(): the delete only lands on the next
        // event-loop pass, and until then the editor is still a visible
        // child sitting on top of the label.
        // window()->focusWidget(), not hasFocus(): hasFocus() is additionally
        // false whenever the window is not ACTIVE, so a commit made while
        // another window happens to be in front would silently skip the
        // restore and strand the keyboard user with no focused control. The
        // question here is only "was the editor this window's focus target".
        QWidget* const editorWindow = ed->window();
        const bool editorHadFocus =
            editorWindow && editorWindow->focusWidget() == ed;
        ed->hide();
        ed->deleteLater();
        // A keyboard user who opened the editor with Enter and closed it with
        // Enter or Escape must land back on the label, not on whatever Qt
        // picks when the focused child disappears.
        if (restore == RestoreFocus::Yes && editorHadFocus
            && focusPolicy() != Qt::NoFocus) {
            setFocus(Qt::OtherFocusReason);
        }
    }

    // Issue #3627: "Invalid values are rejected with validation and the
    // previous value is restored." Restoring is implicit and that is the point
    // — the editor is a separate QLineEdit laid OVER the label, so the label's
    // own text was never touched and still holds model truth. Returning
    // without emitting is the restore.
    //
    // This replaced a clamp (#5064 review). Clamping is a defensible UX, but
    // it is not the one the issue specifies, and silently moving an operator's
    // typed 20000 to 3250 reads as the radio having ignored them.
    void commitEdit(RestoreFocus restore = RestoreFocus::No) {
        if (!m_editor)
            return;
        const QString typed = m_editor->text().trimmed();
        closeEditor(restore);
        bool ok = false;
        const int value = typed.toInt(&ok);
        // Empty, unparseable, or outside the accepted range: reject, and the
        // label keeps whatever the model last put there. The range test is
        // here and not only in the validator because focus-out reaches this
        // with Intermediate text the validator never blocked.
        if (!ok || value < m_min || value > m_max)
            return;
        emit editCommitted(value);
    }

    bool       m_editable{false};
    int        m_min{0};
    int        m_max{0};
    std::function<void(QWidget*)> m_editorStyler;
    QLineEdit* m_editor{nullptr};
};

// An editable ScrollableLabel is an interactive control, and QLabel's default
// StaticText role tells a screen reader the opposite — the same
// interactive-QLabel anti-pattern docs/a11y.md names. Same lazy-factory shape
// as CrossNeedleMeterWidget's.
//
// The role is Button, with a `press` action that opens the editor, exactly as
// docs/a11y.md prescribes. An earlier version of this claimed EditableText on
// the argument that activating it exposes a value rather than performing an
// action — and shipped neither contract: runtime inspection found
// textInterface() and editableTextInterface() both null and SetFocus as the
// only action, so VoiceOver/NVDA/switch-control could focus the readout but
// never open the editor (#5064 review). A role is a promise about which
// interfaces exist; claiming one and implementing none is worse than the
// StaticText it replaced, because an AT stops looking.
//
// Button is also the honest description of the RESTING state. This is a
// two-stage control: the label is the thing you activate, and the QLineEdit
// that appears carries Qt's own complete editable-text semantics. Nothing is
// editable until the press happens.
//
// Keyboard users reach the same beginEdit() through keyPressEvent; AT users
// reach it here. Both go through one entry point, and both honour the #745
// lock — an AT press on a locked panel is the no-op the double-click is.
//
// Display-only ScrollableLabels — RIT/XIT, step size, RTTY mark/shift —
// return nullptr from the factory and keep QLabel's StaticText, which is
// correct for them.
class ScrollableLabelAccessible : public QAccessibleWidget {
public:
    explicit ScrollableLabelAccessible(QWidget* widget)
        : QAccessibleWidget(widget, QAccessible::Button) {}

    // QAccessibleWidget already implements QAccessibleActionInterface (it
    // offers SetFocus for a focusable widget), so these are overrides rather
    // than a new interface — actionInterface() is non-null either way. What
    // was missing was an action that DOES anything.
    QStringList actionNames() const override
    {
        QStringList names{QAccessibleActionInterface::pressAction()};
        names += QAccessibleWidget::actionNames();   // keeps SetFocus
        names.removeDuplicates();
        return names;
    }

    void doAction(const QString& actionName) override
    {
        if (actionName == QAccessibleActionInterface::pressAction()) {
            if (auto* label = qobject_cast<ScrollableLabel*>(object())) {
                if (!ControlsLock::isLocked())
                    label->beginEdit(Qt::OtherFocusReason);
            }
            return;
        }
        QAccessibleWidget::doAction(actionName);
    }

    QStringList keyBindingsForAction(const QString& actionName) const override
    {
        if (actionName == QAccessibleActionInterface::pressAction()) {
            // The same keys keyPressEvent() answers, so what an AT announces
            // and what the keyboard actually does cannot drift apart.
            // Space is deliberately absent: MainWindow owns it as the default
            // application-level PTT-hold key and intercepts it before this
            // widget while a radio is connected (Principle VI).
            return {QStringLiteral("Return")};
        }
        return QAccessibleWidget::keyBindingsForAction(actionName);
    }

    QString localizedActionDescription(const QString& actionName) const override
    {
        if (actionName == QAccessibleActionInterface::pressAction())
            return QCoreApplication::translate(
                "ScrollableLabel", "Open an editor to type an exact value");
        return QAccessibleWidget::localizedActionDescription(actionName);
    }
};

inline QAccessibleInterface* scrollableLabelAccessibleFactory(const QString& key,
                                                              QObject* object)
{
    if (key == QLatin1String("ScrollableLabel")) {
        auto* label = qobject_cast<ScrollableLabel*>(object);
        if (label && label->isEditable())
            return new ScrollableLabelAccessible(label);
    }
    return nullptr;
}
