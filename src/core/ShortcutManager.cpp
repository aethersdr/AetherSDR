#include "ShortcutManager.h"
#include "AppSettings.h"
#include <QHash>
#include <QWidget>
#include <QDebug>

namespace MasterSDR {

ShortcutManager::ShortcutManager(QObject* parent)
    : QObject(parent)
{
}

void ShortcutManager::registerAction(const QString& id, const QString& displayName,
                                     const QString& category, const QKeySequence& defaultKey,
                                     std::function<void()> handler,
                                     bool autoRepeat)
{
    // Prevent duplicate registration
    for (const auto& a : m_actions) {
        if (a.id == id) {
            qWarning() << "ShortcutManager: duplicate action registration:" << id;
            return;
        }
    }
    m_actions.append({id, displayName, category, defaultKey, defaultKey,
                      std::move(handler), autoRepeat});
}

void ShortcutManager::setBinding(const QString& actionId, const QKeySequence& key)
{
    auto* a = action(actionId);
    if (!a) return;

    // Clear any existing binding on this key (prevent conflicts)
    if (!key.isEmpty()) {
        for (auto& other : m_actions) {
            if (other.id != actionId && other.currentKey == key)
                other.currentKey = QKeySequence();
        }
    }

    a->currentKey = key;
    saveBindings();
    emit bindingsChanged();
}

void ShortcutManager::clearBinding(const QString& actionId)
{
    auto* a = action(actionId);
    if (!a) return;
    a->currentKey = QKeySequence();
    saveBindings();
    emit bindingsChanged();
}

void ShortcutManager::resetToDefaults()
{
    for (auto& a : m_actions)
        a.currentKey = a.defaultKey;
    saveBindings();
    emit bindingsChanged();
}

void ShortcutManager::loadBindings()
{
    auto& s = AppSettings::instance();
    for (auto& a : m_actions) {
        const QString key = QStringLiteral("Shortcut_%1").arg(a.id);
        QString val = s.value(key).toString();
        if (!val.isNull())
            a.currentKey = QKeySequence(val);
        // else keep default
    }

    bool normalized = false;
    QHash<QString, int> ownerByKey;
    for (int i = 0; i < m_actions.size(); ++i) {
        auto& action = m_actions[i];
        if (action.currentKey.isEmpty()) continue;

        const QString keyText = action.currentKey.toString();
        auto it = ownerByKey.find(keyText);
        if (it == ownerByKey.end()) {
            ownerByKey.insert(keyText, i);
            continue;
        }

        auto& incumbent = m_actions[*it];
        const bool incumbentCustomized = incumbent.currentKey != incumbent.defaultKey;
        const bool actionCustomized = action.currentKey != action.defaultKey;

        if (actionCustomized && !incumbentCustomized) {
            qWarning() << "ShortcutManager: clearing duplicate default binding"
                       << incumbent.id << "for key" << keyText
                       << "in favor of customized binding" << action.id;
            incumbent.currentKey = QKeySequence();
            *it = i;
        } else {
            qWarning() << "ShortcutManager: clearing duplicate binding"
                       << action.id << "for key" << keyText
                       << "owned by" << incumbent.id;
            action.currentKey = QKeySequence();
        }
        normalized = true;
    }

    if (normalized)
        saveBindings();
}

void ShortcutManager::saveBindings()
{
    auto& s = AppSettings::instance();
    for (const auto& a : m_actions) {
        const QString key = QStringLiteral("Shortcut_%1").arg(a.id);
        s.setValue(key, a.currentKey.toString());
    }
    s.save();
}

void ShortcutManager::rebuildShortcuts(QWidget* parent,
                                       std::function<bool()> guardFn)
{
    // Destroy existing shortcuts
    qDeleteAll(m_shortcuts);
    m_shortcuts.clear();

    for (const auto& a : m_actions) {
        if (a.currentKey.isEmpty() || !a.handler) continue;

        auto* sc = new QShortcut(a.currentKey, parent);
        sc->setAutoRepeat(a.autoRepeat);
        auto handler = a.handler;
        connect(sc, &QShortcut::activated, this, [guardFn, handler]() {
            if (guardFn && !guardFn()) return;
            handler();
        });
        m_shortcuts.append(sc);
    }
}

void ShortcutManager::setShortcutsEnabled(bool enabled)
{
    for (auto* sc : m_shortcuts)
        sc->setEnabled(enabled);
}

ShortcutManager::Action* ShortcutManager::action(const QString& id)
{
    for (auto& a : m_actions) {
        if (a.id == id) return &a;
    }
    return nullptr;
}

const ShortcutManager::Action* ShortcutManager::actionForKey(const QKeySequence& key) const
{
    if (key.isEmpty()) return nullptr;
    for (const auto& a : m_actions) {
        if (a.currentKey == key) return &a;
    }
    return nullptr;
}

QString ShortcutManager::conflictCheck(const QKeySequence& key,
                                       const QString& excludeId) const
{
    if (key.isEmpty()) return {};
    for (const auto& a : m_actions) {
        if (a.id != excludeId && a.currentKey == key)
            return a.displayName;
    }
    return {};
}

QStringList ShortcutManager::categories()
{
    return {"Frequency", "Band", "Mode", "TX", "CW", "Audio", "Slice",
            "Filter", "Tuning", "DSP", "AGC", "EQ", "Display", "RIT/XIT"};
}

} // namespace MasterSDR
