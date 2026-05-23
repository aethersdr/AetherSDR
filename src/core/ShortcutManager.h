#pragma once

#include <QObject>
#include <QKeySequence>
#include <QShortcut>
#include <QString>
#include <QVector>
#include <functional>

namespace MasterSDR {

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
    };

    explicit ShortcutManager(QObject* parent = nullptr);

    // Register an action with its default key binding and handler
    void registerAction(const QString& id, const QString& displayName,
                        const QString& category, const QKeySequence& defaultKey,
                        std::function<void()> handler,
                        bool autoRepeat = false);

    // Binding management
    void setBinding(const QString& actionId, const QKeySequence& key);
    void clearBinding(const QString& actionId);
    void resetToDefaults();

    // Persistence
    void loadBindings();
    void saveBindings();

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
    QVector<Action> m_actions;
    QVector<QShortcut*> m_shortcuts;
};

} // namespace MasterSDR
