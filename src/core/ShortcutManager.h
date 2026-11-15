#pragma once

#include <QObject>
#include <QByteArray>
#include <QKeyEvent>
#include <QKeySequence>
#include <QShortcut>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include "models/TxController.h"

namespace AetherSDR {

struct ShortcutImportResult {
    int importedCount{0};
    QStringList unknownActions;
    // Local (non-imported) actions whose binding this import displaced by a
    // colliding customized incoming key — the caller should surface these so
    // the user notices customizations they didn't intend to overwrite.
    QStringList displacedActions;
    QStringList errors;

    bool ok() const { return errors.isEmpty(); }
};

struct ShortcutExportResult {
    int exportedCount{0};
    QString error;

    bool ok() const { return error.isEmpty(); }
};

// ── Left/right modifier identities (#7354) ─────────────────────────────────
// Qt reports both physical Ctrl (Shift, Alt) keys as one Qt::Key, so a binding
// cannot tell the left paddle from the right on a keyboard-emulating Morse
// adapter. These internal key codes name the physical side. They sit in the
// unused top of the Qt::Key space (below Qt::Key_unknown) and Qt cannot name
// them, so text round-trips go through shortcutKeyText()/shortcutKeyFromText().
// A sided binding wins over the generic key; with none, a sided press falls
// back to the generic binding, so a plain "Control" binding keeps working.
inline constexpr Qt::Key kShortcutKeyLeftCtrl   = static_cast<Qt::Key>(0x01fff001);
inline constexpr Qt::Key kShortcutKeyRightCtrl  = static_cast<Qt::Key>(0x01fff002);
inline constexpr Qt::Key kShortcutKeyLeftShift  = static_cast<Qt::Key>(0x01fff003);
inline constexpr Qt::Key kShortcutKeyRightShift = static_cast<Qt::Key>(0x01fff004);
inline constexpr Qt::Key kShortcutKeyLeftAlt    = static_cast<Qt::Key>(0x01fff005);
inline constexpr Qt::Key kShortcutKeyRightAlt   = static_cast<Qt::Key>(0x01fff006);

struct ShortcutSidedKey {
    Qt::Key key;
    const char* name;   // saved and displayed text
};

inline constexpr ShortcutSidedKey kShortcutSidedKeys[] = {
    {kShortcutKeyLeftCtrl, "Left Ctrl"},   {kShortcutKeyRightCtrl, "Right Ctrl"},
    {kShortcutKeyLeftShift, "Left Shift"}, {kShortcutKeyRightShift, "Right Shift"},
    {kShortcutKeyLeftAlt, "Left Alt"},     {kShortcutKeyRightAlt, "Right Alt"},
};

inline bool isShortcutSidedKey(int key)
{
    for (const auto& s : kShortcutSidedKeys) {
        if (s.key == key)
            return true;
    }
    return false;
}

// The generic Qt key a sided key falls back to. On macOS Qt reports the
// physical Control key as Qt::Key_Meta (Key_Control is Command).
inline Qt::Key shortcutSidedKeyGeneric(int key)
{
    switch (key) {
    case kShortcutKeyLeftCtrl:
    case kShortcutKeyRightCtrl:
#ifdef Q_OS_MACOS
        return Qt::Key_Meta;
#else
        return Qt::Key_Control;
#endif
    case kShortcutKeyLeftShift:
    case kShortcutKeyRightShift:
        return Qt::Key_Shift;
    case kShortcutKeyLeftAlt:
    case kShortcutKeyRightAlt:
        return Qt::Key_Alt;
    default:
        return Qt::Key_unknown;
    }
}

inline bool isShortcutModifierKey(int key)
{
    switch (key) {
    case Qt::Key_Control:
    case Qt::Key_Shift:
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
    case Qt::Key_Meta:
    case Qt::Key_Super_L:
    case Qt::Key_Super_R:
        return true;
    default:
        return isShortcutSidedKey(key);
    }
}

enum class ShortcutNativePlatform { Windows, MacOS, Xkb };

// Which physical side a modifier key event came from, from the platform's
// native key data; Qt::Key_unknown when it cannot tell (synthetic events carry
// zero native data). Pure so every platform's table is testable on any host.
inline Qt::Key shortcutSidedKeyFor(ShortcutNativePlatform platform, int key,
                                   quint32 nativeScanCode, quint32 nativeVirtualKey)
{
    if (!isShortcutModifierKey(key))
        return Qt::Key_unknown;
    switch (platform) {
    case ShortcutNativePlatform::Windows:
        // VK_LCONTROL.. when the sender supplies them (SendInput), else the
        // scan code with the extended-key bit (0x100) marking the right key.
        switch (nativeVirtualKey) {
        case 0xA0: return kShortcutKeyLeftShift;
        case 0xA1: return kShortcutKeyRightShift;
        case 0xA2: return kShortcutKeyLeftCtrl;
        case 0xA3: return kShortcutKeyRightCtrl;
        case 0xA4: return kShortcutKeyLeftAlt;
        case 0xA5: return kShortcutKeyRightAlt;
        default: break;
        }
        switch (nativeScanCode & 0x1ffu) {
        case 0x01d: return kShortcutKeyLeftCtrl;
        case 0x11d: return kShortcutKeyRightCtrl;
        case 0x02a: return kShortcutKeyLeftShift;
        case 0x036: return kShortcutKeyRightShift;
        case 0x038: return kShortcutKeyLeftAlt;
        case 0x138: return kShortcutKeyRightAlt;
        default: return Qt::Key_unknown;
        }
    case ShortcutNativePlatform::MacOS:
        // Carbon kVK_* virtual key codes.
        switch (nativeVirtualKey) {
        case 0x3b: return kShortcutKeyLeftCtrl;
        case 0x3e: return kShortcutKeyRightCtrl;
        case 0x38: return kShortcutKeyLeftShift;
        case 0x3c: return kShortcutKeyRightShift;
        case 0x3a: return kShortcutKeyLeftAlt;
        case 0x3d: return kShortcutKeyRightAlt;
        default: return Qt::Key_unknown;
        }
    case ShortcutNativePlatform::Xkb:
        // X11/xkb keysyms (xcb and Wayland).
        switch (nativeVirtualKey) {
        case 0xffe3: return kShortcutKeyLeftCtrl;
        case 0xffe4: return kShortcutKeyRightCtrl;
        case 0xffe1: return kShortcutKeyLeftShift;
        case 0xffe2: return kShortcutKeyRightShift;
        case 0xffe9: return kShortcutKeyLeftAlt;
        case 0xffea: return kShortcutKeyRightAlt;
        default: return Qt::Key_unknown;
        }
    }
    return Qt::Key_unknown;
}

inline Qt::Key shortcutSidedKeyFromEvent(const QKeyEvent* ev)
{
    if (!ev)
        return Qt::Key_unknown;
    if (isShortcutSidedKey(ev->key()))   // an injected sided key
        return static_cast<Qt::Key>(ev->key());
#if defined(Q_OS_WIN)
    constexpr auto platform = ShortcutNativePlatform::Windows;
#elif defined(Q_OS_MACOS)
    constexpr auto platform = ShortcutNativePlatform::MacOS;
#else
    constexpr auto platform = ShortcutNativePlatform::Xkb;
#endif
    return shortcutSidedKeyFor(platform, ev->key(), ev->nativeScanCode(),
                               ev->nativeVirtualKey());
}

// The key sequence a key event resolves to for binding lookup: the key plus
// Shift/Ctrl/Alt/Meta only. Shared by every resolver so they match alike.
// A modifier key resolves bare: its own press carries its modifier bit (and a
// squeezed paddle's), which would turn a "Control" binding into Ctrl+Control
// that never matches on press (#7354). It resolves to its sided key when the
// native data names the side.
inline QKeySequence shortcutSequenceFromKeyEvent(const QKeyEvent* ev)
{
    if (!ev || ev->key() == Qt::Key_unknown)
        return {};

    if (isShortcutModifierKey(ev->key())) {
        const Qt::Key sided = shortcutSidedKeyFromEvent(ev);
        return QKeySequence(sided != Qt::Key_unknown ? sided : ev->key());
    }

    const Qt::KeyboardModifiers modifiers =
        ev->modifiers() & (Qt::ShiftModifier
                           | Qt::ControlModifier
                           | Qt::AltModifier
                           | Qt::MetaModifier);
    return QKeySequence(static_cast<int>(modifiers) | ev->key());
}

// QKeySequence::toString()/fromString() that also name the sided keys. Every
// persisted or displayed binding goes through these: Qt alone renders a sided
// key as unparseable text, which would drop it on save/load and CSV import.
// The defaults mirror Qt's: toString() is PortableText, QKeySequence(QString)
// parses NativeText.
inline QString shortcutKeyText(const QKeySequence& seq,
                               QKeySequence::SequenceFormat format = QKeySequence::PortableText)
{
    if (seq.count() == 1 && isShortcutSidedKey(seq[0].key())) {
        for (const auto& s : kShortcutSidedKeys) {
            if (s.key != seq[0].key())
                continue;
            // Let Qt render any modifier prefix in the requested format.
            QString prefix = QKeySequence(seq[0].keyboardModifiers() | Qt::Key_F35)
                                 .toString(format);
            prefix.chop(3);   // "F35"
            return prefix + QLatin1String(s.name);
        }
    }
    return seq.toString(format);
}

inline QKeySequence shortcutKeyFromText(const QString& text,
                                        QKeySequence::SequenceFormat format = QKeySequence::NativeText)
{
    for (const auto& s : kShortcutSidedKeys) {
        const QLatin1String name(s.name);
        if (!text.endsWith(name, Qt::CaseInsensitive))
            continue;
        const QString prefix = text.left(text.size() - name.size());
        if (prefix.isEmpty())
            return QKeySequence(s.key);
        const QKeySequence mods =
            QKeySequence::fromString(prefix + QLatin1String("F35"), format);
        if (mods.count() == 1 && mods[0].key() == Qt::Key_F35)
            return QKeySequence(mods[0].keyboardModifiers() | s.key);
    }
    return QKeySequence::fromString(text, format);
}

class ShortcutManager : public QObject {
    Q_OBJECT
public:
    enum class ShortcutPolicy { Operating, WindowManagement };

    struct Action {
        QString id;
        QString displayName;
        QString category;
        QKeySequence defaultKey;
        QKeySequence currentKey;
        std::function<void()> handler;
        bool autoRepeat{false};   // allow key-hold repeat (e.g. tuning)
        bool persisted{false};    // explicit user intent (set/cleared) → written to settings
        bool keysTx{false};       // keys the transmitter — declared at the
                                  // registration site (like markTxKeying for
                                  // widgets) so TX gates read one source of
                                  // truth, not a hand-maintained id list that
                                  // drifts (#4057 review: atu_start was missed).
        TxController::Activity txActivity{TxController::Activity::Mox};
        std::function<void(const TxController::Input&)> txHandler;
        ShortcutPolicy policy{ShortcutPolicy::Operating};
    };

    explicit ShortcutManager(QObject* parent = nullptr);

    // Register an action with its default key binding and handler. Pass
    // keysTx=true for any action that keys the transmitter (MOX, TUNE, ATU,
    // two-tone, PTT, CW keying) — automation gates honor the flag.
    void registerAction(const QString& id, const QString& displayName,
                        const QString& category, const QKeySequence& defaultKey,
                        std::function<void()> handler,
                        bool autoRepeat = false,
                        bool keysTx = false,
                        ShortcutPolicy policy = ShortcutPolicy::Operating);

    // Binding management
    void setBinding(const QString& actionId, const QKeySequence& key);
    void clearBinding(const QString& actionId);
    void resetToDefaults();

    // Persistence
    void loadBindings();
    void saveBindings();

    // Portable backup format. Rows identify actions by stable id and also carry
    // their human-readable names so imports can fall back across an id rename.
    // QKeySequence::PortableText keeps modifier names cross-platform.
    QByteArray exportBindingsCsv() const;
    ShortcutImportResult importBindingsCsv(const QByteArray& bytes);

    // File I/O for the portable backup — atomic write via QSaveFile, size-gated
    // and error-string-surfacing read. Kept on the manager itself so the gui
    // never learns about QFile/QSaveFile (matches ThemeManager's precedent).
    ShortcutExportResult exportToFile(const QString& path) const;
    ShortcutImportResult importFromFile(const QString& path);

    // Create/destroy QShortcuts on the target widget.
    // Operating actions are window-scoped and consult guardFn. WindowManagement
    // actions are application-scoped and remain available while operating input
    // is disabled/captured. TX actions always retain the operating policy.
    void rebuildShortcuts(QWidget* parent,
                          std::function<bool()> guardFn = nullptr);

    // Enable or disable the operating QShortcuts, so their keys reach the
    // focused widget (a slider's arrows, or every bound key with keyboard
    // shortcuts off, #5483). Remembered: rebuildShortcuts() applies it, so a
    // rebuild cannot re-arm yielded keys.
    void setShortcutsEnabled(bool enabled);
    bool shortcutsEnabled() const { return m_shortcutsEnabled; }

    // Query
    const QVector<Action>& actions() const { return m_actions; }
    Action* action(const QString& id);
    const Action* actionForKey(const QKeySequence& key) const;
    // The action bound to `key` that the keyboard-shortcut master switch
    // governs; nullptr for an unbound key or a window-management action,
    // which runs whatever the switch says.
    const Action* operatingActionForKey(const QKeySequence& key) const;
    // A window-management exemption never widens a keying action.
    static bool isWindowManagement(const Action& a);
    QString conflictCheck(const QKeySequence& key,
                          const QString& excludeId = {}) const;

    // Categories (ordered for legend display)
    static QStringList categories();

signals:
    void bindingsChanged();

private:
    void normalizeDuplicateBindings();

    QVector<Action> m_actions;
    QVector<QShortcut*> m_shortcuts;
    QVector<QShortcut*> m_windowShortcuts;
    bool m_shortcutsEnabled{true};
};

} // namespace AetherSDR
