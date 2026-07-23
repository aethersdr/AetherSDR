#pragma once

#include <QObject>
#include <QByteArray>
#include <QKeySequence>
#include <QShortcut>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

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

class ShortcutManager : public QObject {
    Q_OBJECT
public:
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
    };

    explicit ShortcutManager(QObject* parent = nullptr);

    // Register an action with its default key binding and handler. Pass
    // keysTx=true for any action that keys the transmitter (MOX, TUNE, ATU,
    // two-tone, PTT, CW keying) — automation gates honor the flag.
    void registerAction(const QString& id, const QString& displayName,
                        const QString& category, const QKeySequence& defaultKey,
                        std::function<void()> handler,
                        bool autoRepeat = false,
                        bool keysTx = false);

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
    // guardFn is called before each handler — return false to suppress.
    void rebuildShortcuts(QWidget* parent,
                          std::function<bool()> guardFn = nullptr);

    // Enable or disable all active QShortcut objects. Used to yield key
    // events to focused child widgets (e.g. sliders) that would otherwise
    // have their arrow keys stolen by window-level shortcuts.
    void setShortcutsEnabled(bool enabled);

    // Query
    const QVector<Action>& actions() const { return m_actions; }
    Action* action(const QString& id);
    const Action* actionForKey(const QKeySequence& key) const;
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
};

} // namespace AetherSDR
